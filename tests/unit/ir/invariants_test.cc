// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The scan, and the vocabulary it reports in.
//
// The scan is the mechanism behind `ir.md`'s claim that the list of assumptions
// this compiler hands to the optimizer is closed. These tests pin the three
// halves of that claim: a module this compiler built passes clean, the enumeration
// of codes has no entry without a name (so a code added to the enum without a
// table row fails here rather than in a user's terminal), and **every row of the
// assumption list can be tripped** -- a scan nothing can trip is a scan that
// stopped running.
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/Alignment.h"

#include "ir/invariants.h"
#include "ir/ir.h"
#include "ir/ir_fixture.h"
#include "ir/storage.h"

namespace minc::ir {
namespace {

TEST(IrInvariantsTest, EveryBuiltModulePasses) {
  test::IrFixture fixture;
  fixture.source("fn i32 sum(n: i32)\n"
                 "{\n"
                 "  let total: i32 = 0;\n"
                 "  let i: i32 = 0;\n"
                 "  while i < n\n"
                 "  {\n"
                 "    total += i;\n"
                 "    i += 1;\n"
                 "  }\n"
                 "  return total;\n"
                 "}\n"
                 "fn i32 main() { return sum(4) / 2; }\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  const std::vector<IRDiagnostic> violations = fixture.scan();
  for (const IRDiagnostic& violation : violations) {
    ADD_FAILURE() << toString(violation.code) << ": " << violation.message;
  }
  EXPECT_TRUE(violations.empty());
}

TEST(IrInvariantsTest, AnUnbuiltModuleScansClean) {
  // A default-constructed handle owns no module, so there is nothing to scan
  // and nothing to report -- which is what keeps a caller from having to check
  // `built()` before asking.
  const Module module;
  EXPECT_FALSE(module.built());
  EXPECT_TRUE(scanModule(module).empty());
}

TEST(IrInvariantsTest, EveryDiagnosticCodeHasAName) {
  const std::vector<IRDiagnosticCode>& codes = allDiagnosticCodes();
  EXPECT_FALSE(codes.empty());

  std::set<IRDiagnosticCode> unique;
  for (const IRDiagnosticCode code : codes) {
    const std::string name(toString(code));
    EXPECT_FALSE(name.empty()) << static_cast<int>(code);
    EXPECT_EQ(name.rfind("ir-", 0), 0U) << name;
    unique.insert(code);
  }
  // One row per code: a duplicate row would make the table describe a code
  // twice and the enumeration would be longer than the enum.
  EXPECT_EQ(unique.size(), codes.size());
}

TEST(IrInvariantsTest, EveryAssumptionRowHasAName) {
  EXPECT_FALSE(moduleAssumptions().empty());
  EXPECT_EQ(allModuleAssumptions().size(), moduleAssumptions().size());

  std::set<std::string> names;
  for (const ModuleAssumptionInfo& row : moduleAssumptions()) {
    const std::string name(toString(row.assumption));
    EXPECT_FALSE(name.empty()) << static_cast<int>(row.assumption);
    EXPECT_NE(name, "unknown") << static_cast<int>(row.assumption);
    names.insert(name);
  }
  // One row per row: a duplicate would make the table describe an assumption
  // twice and the enumeration longer than the enum.
  EXPECT_EQ(names.size(), moduleAssumptions().size());
}

// --- the row can be tripped ------------------------------------------------------
//
// The half of the mechanism that a comment cannot provide: a check that nothing
// can trip is a check that stopped running. Every row is violated here on a module
// this compiler would never build -- one built *by* the compiler and then broken by
// hand -- which is the only way to reach a violation at all, since the program it
// came from type-checked.

// The program every tripwire starts from: a file-scope object (for the object
// rows), a function with parameters (for the attribute row), a `getelementptr`, an
// integer `add`, a `fadd`, and one guarded operation per guard row -- a `sdiv` and
// a float-to-integer conversion of a value that is *not* a constant, which is the
// only shape the second one has. One program, so a row's mutation is one line
// rather than a module written from scratch.
constexpr std::string_view kTripwireProgram =
    "const SIZE: i32 = 8;\n"
    "fn i32 mix(a: i32, b: i32, p: *i32)\n"
    "{\n"
    "  let x: f32 = 1.5;\n"
    "  let y: f32 = x + x;\n"
    "  *p = a;\n"
    "  return a + b + (a / b) + p[1] + SIZE + (y as i32);\n"
    "}\n"
    "fn i32 main() { return 0; }\n";

// A mutation of that module which states something the language did not.
using Mutation = void (*)(llvm::Module&);

[[nodiscard]] llvm::Function* definitionOf(llvm::Module& module, llvm::StringRef name) {
  llvm::Function* function = module.getFunction(name);
  return function != nullptr && !function->isDeclaration() ? function : nullptr;
}

// The first instruction of a given opcode, in block order, or null. Null only when
// the program does not have one -- which is a failure of the tripwire table, and
// the test reports it as "the row was not detected".
[[nodiscard]] llvm::Instruction* firstOfOpcode(llvm::Function& function, unsigned opcode) {
  for (llvm::BasicBlock& block : function) {
    for (llvm::Instruction& instruction : block) {
      if (instruction.getOpcode() == opcode) {
        return &instruction;
      }
    }
  }
  return nullptr;
}

void tripMetadata(llvm::Module& module) {
  llvm::Function* function = definitionOf(module, "mix");
  llvm::Instruction* instruction =
      function == nullptr ? nullptr : firstOfOpcode(*function, llvm::Instruction::Load);
  if (instruction == nullptr) {
    return;
  }
  // A defined node, so the module stays well formed: what is forbidden is the
  // *attachment*, not the node. `getMDKindID` accepts any name, which is how a
  // kind this compiler has never heard of gets here.
  instruction->setMetadata("minc.tripwire", llvm::MDNode::get(module.getContext(), {}));
}

void tripFunctionAttribute(llvm::Module& module) {
  if (llvm::Function* function = definitionOf(module, "mix")) {
    function->addFnAttr(llvm::Attribute::NoAlias);
  }
}

void tripInbounds(llvm::Module& module) {
  llvm::Function* function = definitionOf(module, "mix");
  auto* gep = function == nullptr ? nullptr
                                  : llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(
                                        firstOfOpcode(*function, llvm::Instruction::GetElementPtr));
  if (gep != nullptr) {
    gep->setIsInBounds(true);
  }
}

void tripWrapping(llvm::Module& module) {
  llvm::Function* function = definitionOf(module, "mix");
  auto* add = function == nullptr ? nullptr
                                  : llvm::dyn_cast_or_null<llvm::BinaryOperator>(
                                        firstOfOpcode(*function, llvm::Instruction::Add));
  if (add != nullptr) {
    add->setHasNoSignedWrap(true);
  }
}

void tripFastMath(llvm::Module& module) {
  llvm::Function* function = definitionOf(module, "mix");
  llvm::Instruction* fadd =
      function == nullptr ? nullptr : firstOfOpcode(*function, llvm::Instruction::FAdd);
  if (fadd != nullptr && llvm::isa<llvm::FPMathOperator>(fadd)) {
    llvm::FastMathFlags flags;
    flags.setAllowReassoc(true);
    fadd->setFastMathFlags(flags);
  }
}

void tripDebugIntrinsic(llvm::Module& module) {
  llvm::Function* function = definitionOf(module, "mix");
  llvm::Instruction* at = function == nullptr ? nullptr : function->getEntryBlock().getTerminator();
  if (at == nullptr) {
    return;
  }
  // The intrinsic *by name*, which is all the scan reads: the two forms may not
  // coexist in one module, and the form this compiler emits is records.
  llvm::Function* intrinsic = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(module.getContext()), /*isVarArg=*/false),
      llvm::GlobalValue::ExternalLinkage, "llvm.dbg.value", &module);
  // The iterator overload: the `Instruction*` insertion position is deprecated
  // (LLVM 20), and this project builds with `-Werror`.
  llvm::CallInst::Create(intrinsic, {}, "", at->getIterator());
}

void tripConstantObject(llvm::Module& module) {
  if (llvm::GlobalVariable* object = module.getNamedGlobal("SIZE")) {
    object->setConstant(true);
  }
}

void tripAlignment(llvm::Module& module) {
  if (llvm::GlobalVariable* object = module.getNamedGlobal("SIZE")) {
    object->setAlignment(llvm::Align(1));
  }
}

void tripUnguardedDivision(llvm::Module& module) {
  llvm::Function* function = definitionOf(module, "mix");
  auto* division = function == nullptr ? nullptr
                                       : llvm::dyn_cast_or_null<llvm::BinaryOperator>(
                                             firstOfOpcode(*function, llvm::Instruction::SDiv));
  if (division == nullptr || division->getParent() == nullptr) {
    return;
  }
  llvm::BasicBlock* predecessor = division->getParent()->getUniquePredecessor();
  auto* branch = predecessor == nullptr
                     ? nullptr
                     : llvm::dyn_cast_or_null<llvm::BranchInst>(predecessor->getTerminator());
  if (branch != nullptr && branch->isConditional()) {
    // The divisor is still tested -- a `true` is a constant, so the scan cannot
    // see the test -- and the division is no longer *reached through* it, which is
    // exactly the code the guard is not.
    branch->setCondition(llvm::ConstantInt::getTrue(module.getContext()));
  }
}

void tripUnguardedFloatToInt(llvm::Module& module) {
  llvm::Function* function = definitionOf(module, "mix");
  auto* conversion = function == nullptr ? nullptr
                                         : llvm::dyn_cast_or_null<llvm::CastInst>(
                                               firstOfOpcode(*function, llvm::Instruction::FPToSI));
  if (conversion == nullptr || conversion->getParent() == nullptr) {
    return;
  }
  llvm::BasicBlock* predecessor = conversion->getParent()->getUniquePredecessor();
  auto* branch = predecessor == nullptr
                     ? nullptr
                     : llvm::dyn_cast_or_null<llvm::BranchInst>(predecessor->getTerminator());
  if (branch != nullptr && branch->isConditional()) {
    // The same mutation the division row uses, for the same reason: the test is
    // still there and the conversion is no longer *reached through* it, which is
    // exactly the code the guard is not.
    branch->setCondition(llvm::ConstantInt::getTrue(module.getContext()));
  }
}

// The checked build's row, and the only one whose module has to be built with a
// flag on: the promise is about a module that *asked* for the guards, and an
// unguarded access in an unchecked module is what an unchecked module is.
//
// The mutation removes the guard rather than weakening it: the conditional branch
// that stands in front of an access is replaced by an unconditional one, which is
// exactly the shape of "an access was emitted without its guard" -- and neither
// half of the scan's rule can be satisfied by what is left.
void tripUnguardedAccess(llvm::Module& module) {
  llvm::Function* function = definitionOf(module, "mix");
  if (function == nullptr) {
    return;
  }
  for (llvm::BasicBlock& block : *function) {
    // The first access whose address the compiler did not put there itself: the
    // same two escapes the scan uses, so the mutation reaches a guarded access and
    // not a binding's slot.
    llvm::Value* address = nullptr;
    for (llvm::Instruction& instruction : block) {
      if (auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
        address = load->getPointerOperand();
      } else if (auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
        address = store->getPointerOperand();
      } else {
        continue;
      }
      if (llvm::isa<llvm::Constant>(address) || llvm::isa<llvm::AllocaInst>(address) ||
          llvm::isa<llvm::Argument>(address)) {
        continue;
      }
      llvm::BasicBlock* predecessor = block.getUniquePredecessor();
      if (predecessor == nullptr || predecessor->getTerminator() == nullptr) {
        continue;
      }
      predecessor->getTerminator()->eraseFromParent();
      llvm::BranchInst::Create(&block, predecessor);
      return;
    }
  }
}

[[nodiscard]] const std::map<ModuleAssumption, Mutation>& tripwires() {
  static const std::map<ModuleAssumption, Mutation> table{
      {ModuleAssumption::Metadata, &tripMetadata},
      {ModuleAssumption::FunctionAttribute, &tripFunctionAttribute},
      {ModuleAssumption::Inbounds, &tripInbounds},
      {ModuleAssumption::Wrapping, &tripWrapping},
      {ModuleAssumption::FastMath, &tripFastMath},
      {ModuleAssumption::DebugIntrinsic, &tripDebugIntrinsic},
      {ModuleAssumption::ConstantObject, &tripConstantObject},
      {ModuleAssumption::Alignment, &tripAlignment},
      {ModuleAssumption::UnguardedDivision, &tripUnguardedDivision},
      {ModuleAssumption::UnguardedFloatToInt, &tripUnguardedFloatToInt},
      {ModuleAssumption::UnguardedAccess, &tripUnguardedAccess},
  };
  return table;
}

TEST(IrInvariantsTest, EveryAssumptionRowCanBeTripped) {
  // The enumeration is walked, and every row has to be *detected* -- not merely
  // declared. A row added without a mutation fails the lookup; a row whose check
  // stopped working fails the scan.
  for (const ModuleAssumptionInfo& row : moduleAssumptions()) {
    const auto found = tripwires().find(row.assumption);
    ASSERT_NE(found, tripwires().end()) << "no tripwire for the row `" << row.name << "`";

    test::IrFixture fixture;
    fixture.source(std::string(kTripwireProgram));
    // The guards, for the one row about them: every other row is tripped on a
    // module of either kind, and building all of them checked would put a guard in
    // front of the accesses these rows are about -- which is fine, but it also
    // means the *clean* module assertion below would be measuring the guards
    // rather than the row.
    fixture.checks(row.assumption == ModuleAssumption::UnguardedAccess);
    ASSERT_TRUE(fixture.build());
    ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();
    // The program itself has to be clean, or the row's violation would be one of
    // many and the test would pass for the wrong reason.
    ASSERT_EQ(fixture.violations(), 0u) << row.name << ":\n" << fixture.module();

    found->second(ModuleAccess::llvmModule(fixture.result().module));
    const std::vector<IRDiagnostic> violations = fixture.scan();

    bool detected = false;
    for (const IRDiagnostic& violation : violations) {
      if (violation.code == row.code) {
        detected = true;
      }
    }
    EXPECT_TRUE(detected) << "the row `" << row.name << "` was not detected:\n" << fixture.module();
  }
}

TEST(IrInvariantsTest, TheAssumptionScanIsExposedForAConsumer) {
  // The scan is a function over an artifact and nothing else, so a later stage
  // (the `link` command, a fuzz harness) can ask it the same question without
  // going through the driver.
  test::IrFixture fixture;
  fixture.source("fn i32 main() { return 1; }\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt());
  EXPECT_TRUE(scanModule(fixture.result().module).empty());
}

} // namespace
} // namespace minc::ir
