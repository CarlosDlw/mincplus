// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The invariant scan: the module this compiler built, checked against the rules
// it promised to obey.
//
// `llvm::verifyModule` -- run by the lowering itself -- proves a module is *well
// formed*. This proves it is **ours**: that the closed list of assumptions in
// `ir.md` is the list actually in the module, that every division was guarded,
// and that every access states the alignment its type gives. None of those is a
// rule about LLVM, so the verifier cannot see any of them: they are rules about
// this language, and the reason they are written down is that violating one is a
// *miscompile* rather than a diagnostic.
//
// It is a scan and not a promise because a promise is what got broken the first
// time. Every check here reads the finished module, so an emitter that forgets a
// guard or adds a `nsw` fails a test instead of producing wrong code.
#include "ir/invariants.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"

#include "ir/ir.h"
#include "storage.h"

namespace minc::ir {
namespace {

void add(std::vector<IRDiagnostic>& out, IRDiagnosticCode code, std::string message) {
  IRDiagnostic diagnostic;
  diagnostic.code = code;
  diagnostic.message = std::move(message);
  out.push_back(std::move(diagnostic));
}

// The attribute names the model forbids outright. Each one is a promise to the
// optimizer that this language does not make: `noalias` would say two pointers
// do not alias (`memory.md` says aliasing is not typed and only an explicit
// `restrict` may promise otherwise), `nonnull`/`dereferenceable`/`noundef` would
// say a value is one the source never proved, and `nsw` in attribute form is
// the same refusal the instruction flags get.
[[nodiscard]] bool isForbiddenAttribute(llvm::StringRef name) {
  return name == "noalias" || name == "nonnull" || name == "noundef" || name == "dereferenceable" ||
         name == "dereferenceable_or_null" || name == "align" || name == "signext" ||
         name == "zeroext" || name == "inreg";
}

void scanAttributes(const llvm::AttributeList& attributes, const llvm::Function& function,
                    std::vector<IRDiagnostic>& out) {
  const auto check = [&](const llvm::Attribute& attribute) {
    if (!isForbiddenAttribute(attribute.getKindAsString())) {
      return;
    }
    add(out, IRDiagnosticCode::Assumption,
        "`" + function.getName().str() + "` carries the attribute `" +
            attribute.getKindAsString().str() +
            "`, which the language does not state; see `ir.md`, *The assumption list*");
  };
  for (const llvm::Attribute& attribute : attributes.getFnAttrs()) {
    check(attribute);
  }
  for (const llvm::Attribute& attribute : attributes.getRetAttrs()) {
    check(attribute);
  }
  for (unsigned i = 0; i < attributes.getNumAttrSets(); ++i) {
    for (const llvm::Attribute& attribute : attributes.getParamAttrs(i)) {
      check(attribute);
    }
  }
}

// Does the condition tree of a branch test `divisor`? The guard `checkedDiv`
// emits is an `icmp eq divisor, 0` (or a `or` of that with the `INT_MIN / -1`
// test), so a division whose divisor is never compared is one that skipped the
// guard.
[[nodiscard]] bool testsValue(const llvm::Value* condition, const llvm::Value* divisor, int depth) {
  if (condition == nullptr || depth > 8) {
    return false;
  }
  if (const auto* comparison = llvm::dyn_cast<llvm::ICmpInst>(condition)) {
    return comparison->getOperand(0) == divisor || comparison->getOperand(1) == divisor;
  }
  if (const auto* binary = llvm::dyn_cast<llvm::BinaryOperator>(condition)) {
    if (binary->getOpcode() == llvm::Instruction::And ||
        binary->getOpcode() == llvm::Instruction::Or) {
      return testsValue(binary->getOperand(0), divisor, depth + 1) ||
             testsValue(binary->getOperand(1), divisor, depth + 1);
    }
  }
  return false;
}

[[nodiscard]] bool guardedDivision(const llvm::BinaryOperator& division) {
  const llvm::BasicBlock* block = division.getParent();
  if (block == nullptr) {
    return false;
  }
  // `getUniquePredecessor` and not `getSinglePredecessor`: the latter also
  // demands that the predecessor's terminator branch *only* to this block, and
  // the whole point of a guard block is that it branches to the trap as well.
  const llvm::BasicBlock* predecessor = block->getUniquePredecessor();
  if (predecessor == nullptr) {
    return false;
  }
  const auto* branch = llvm::dyn_cast<llvm::BranchInst>(predecessor->getTerminator());
  if (branch == nullptr || !branch->isConditional()) {
    return false;
  }
  const llvm::Value* divisor = division.getOperand(1);
  if (llvm::isa<llvm::ConstantInt>(divisor)) {
    // A constant divisor is already proven, and asking for the test again would
    // fail for the wrong reason: `x / 3` folds `icmp eq 3, 0` to `false`, so the
    // comparison the source's guard wrote is not in the module any more. The
    // conditional predecessor is the part that still says this went through
    // `checkedDiv`; a constant divisor of zero is a program that traps on every
    // execution, which is the *defined* answer and not a missing guard.
    return true;
  }
  return testsValue(branch->getCondition(), divisor, /*depth=*/0);
}

[[nodiscard]] bool isDivision(llvm::Instruction::BinaryOps opcode) {
  switch (opcode) {
  case llvm::Instruction::SDiv:
  case llvm::Instruction::UDiv:
  case llvm::Instruction::SRem:
  case llvm::Instruction::URem:
    return true;
  default:
    return false;
  }
}

void scanInstruction(const llvm::Instruction& instruction, const llvm::Function& function,
                     const llvm::DataLayout& layout, std::vector<IRDiagnostic>& out) {
  const std::string where =
      "`" + function.getName().str() + "` in block `" +
      (instruction.getParent() != nullptr ? instruction.getParent()->getName().str()
                                          : std::string("?")) +
      "`";

  // No metadata at all. `!tbaa` is the one the model names -- aliasing is not
  // typed, so there is no metadata to emit -- and the rule is stated as
  // "none" rather than "no `!tbaa`" because every other metadata kind is an
  // assumption about the program that this compiler has not been asked to make.
  if (instruction.hasMetadata()) {
    add(out, IRDiagnosticCode::Assumption,
        "an instruction in " + where + " carries metadata; the language emits none");
  }

  if (const auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&instruction)) {
    // `inbounds` is the pointer analogue of `nsw`: a promise that the offset
    // stays inside the object. Nothing here records such a proof, so no
    // `inbounds` may appear (`ir.md`, *The assumption list*).
    if (gep->isInBounds()) {
      add(out, IRDiagnosticCode::Assumption,
          "an `inbounds` getelementptr reached the module in " + where +
              "; no proof of it was recorded");
    }
  }

  if (const auto* binary = llvm::dyn_cast<llvm::BinaryOperator>(&instruction)) {
    if (binary->hasNoSignedWrap() || binary->hasNoUnsignedWrap()) {
      add(out, IRDiagnosticCode::Assumption,
          "an integer operation with `nsw`/`nuw` reached the module in " + where +
              "; the language defines wrapping, so it may not promise otherwise");
    }
    if (isDivision(binary->getOpcode()) && !guardedDivision(*binary)) {
      add(out, IRDiagnosticCode::UnguardedOp,
          "a division or remainder in " + where +
              " is not reached through a test of its divisor; the language defines the "
              "operation as a trap");
    }
  }

  if (llvm::isa<llvm::FPMathOperator>(&instruction) && instruction.getFastMathFlags().any()) {
    add(out, IRDiagnosticCode::Assumption,
        "a floating-point operation with fast-math flags reached the module in " + where);
  }

  // Every access states the alignment its type gives. Overstating it is
  // *undefined behaviour* in LLVM and not slow code, so this is an equality and
  // not a lower bound: the lowering has one rule for the number, and a second
  // rule appearing anywhere is what the check exists to catch.
  if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
    const llvm::Align required = layout.getABITypeAlign(load->getType());
    if (load->getAlign() != required) {
      add(out, IRDiagnosticCode::Alignment,
          "a load in " + where + " states alignment " + std::to_string(load->getAlign().value()) +
              " where its type requires " + std::to_string(required.value()));
    }
  }
  if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
    const llvm::Align required = layout.getABITypeAlign(store->getValueOperand()->getType());
    if (store->getAlign() != required) {
      add(out, IRDiagnosticCode::Alignment,
          "a store in " + where + " states alignment " + std::to_string(store->getAlign().value()) +
              " where its type requires " + std::to_string(required.value()));
    }
  }
}

} // namespace

std::vector<IRDiagnostic> scanModule(const Module& module) {
  std::vector<IRDiagnostic> violations;
  if (!ModuleAccess::built(module)) {
    return violations;
  }
  const llvm::Module& llvmModule = ModuleAccess::llvmModule(module);
  const llvm::DataLayout& layout = ModuleAccess::layout(module);

  for (const llvm::Function& function : llvmModule) {
    scanAttributes(function.getAttributes(), function, violations);
    for (const llvm::BasicBlock& block : function) {
      for (const llvm::Instruction& instruction : block) {
        scanInstruction(instruction, function, layout, violations);
      }
    }
  }
  return violations;
}

} // namespace minc::ir
