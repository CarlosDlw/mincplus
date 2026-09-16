// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The invariant scan: the module this compiler built, checked against the rules
// it promised to obey.
//
// `llvm::verifyModule` -- run by the lowering itself -- proves a module is *well
// formed*. This proves it is **ours**: that the closed list of assumptions in
// `ir.md` is the list actually in the module, that every division was guarded,
// and that every alignment -- an access's, and a file-scope object's -- is the
// one its type gives. None of those is a
// rule about LLVM, so the verifier cannot see any of them: they are rules about
// this language, and the reason they are written down is that violating one is a
// *miscompile* rather than a diagnostic.
//
// It is a scan and not a promise because a promise is what got broken the first
// time. Every check here reads the finished module, so an emitter that forgets a
// guard or adds a `nsw` fails a test instead of producing wrong code.
#include "ir/invariants.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
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
#include "ir/storage.h"

namespace minc::ir {
namespace {

// The assumption list, in one table with the enumeration: a row added to the enum
// without one is a compile error here, and a row whose enumerator no longer
// exists is caught by `allModuleAssumptions()` being derived from this table and
// compared against it in a test. Same shape as `sema`'s access tables, for the
// same reason.
// NOLINTBEGIN(readability-identifier-naming): table name follows the project's
// convention for the other stages' tables.
constexpr std::array<ModuleAssumptionInfo, 11> kModuleAssumptionInfos{{
    {ModuleAssumption::Metadata, "metadata", IRDiagnosticCode::Assumption},
    {ModuleAssumption::FunctionAttribute, "function-attribute", IRDiagnosticCode::Assumption},
    {ModuleAssumption::Inbounds, "inbounds", IRDiagnosticCode::Assumption},
    {ModuleAssumption::Wrapping, "wrapping", IRDiagnosticCode::Assumption},
    {ModuleAssumption::FastMath, "fast-math", IRDiagnosticCode::Assumption},
    {ModuleAssumption::DebugIntrinsic, "debug-intrinsic", IRDiagnosticCode::Assumption},
    {ModuleAssumption::ConstantObject, "constant-object", IRDiagnosticCode::Assumption},
    {ModuleAssumption::Alignment, "alignment", IRDiagnosticCode::Alignment},
    {ModuleAssumption::UnguardedDivision, "unguarded-division", IRDiagnosticCode::UnguardedOp},
    {ModuleAssumption::UnguardedFloatToInt, "unguarded-float-to-int",
     IRDiagnosticCode::UnguardedOp},
    {ModuleAssumption::UnguardedAccess, "unguarded-access", IRDiagnosticCode::UnguardedOp},
}};
// NOLINTEND(readability-identifier-naming)

template <std::size_t... Indexes>
[[nodiscard]] constexpr auto assumptionsFromTable(std::index_sequence<Indexes...>) {
  return std::array<ModuleAssumption, sizeof...(Indexes)>{
      kModuleAssumptionInfos[Indexes].assumption...};
}

constexpr auto kAllModuleAssumptions =
    assumptionsFromTable(std::make_index_sequence<kModuleAssumptionInfos.size()>{});

void add(std::vector<IRDiagnostic>& out, IRDiagnosticCode code, std::string message) {
  IRDiagnostic diagnostic;
  diagnostic.code = code;
  diagnostic.message = std::move(message);
  out.push_back(std::move(diagnostic));
}

// The attributes the language never states, **by kind and not by name**.
//
// Each one is a promise to the optimizer that this language does not make:
// `noalias` would say two pointers do not alias (`memory.md` says aliasing is not
// typed and only an explicit `restrict` may promise otherwise),
// `nonnull`/`dereferenceable`/`noundef` would say a value is one the source never
// proved, and the integer extension hints say the ABI wants a wider value than
// the type does.
//
// A kind, and not `Attribute::getKindAsString()`: the name is a *library* table
// lookup, and the value this compiler reads through it is not the attribute --
// measured, not assumed -- so a name comparison would leave this row silently
// unchecked. The enum value is the compiler's own spelling and needs no table.
// The message below therefore names each kind from this compiler's side, and a
// kind added here without a name is a compile error at the `switch`.
[[nodiscard]] const char* forbiddenAttributeName(llvm::Attribute::AttrKind kind) {
  switch (kind) {
  case llvm::Attribute::NoAlias:
    return "noalias";
  case llvm::Attribute::NonNull:
    return "nonnull";
  case llvm::Attribute::NoUndef:
    return "noundef";
  case llvm::Attribute::Dereferenceable:
    return "dereferenceable";
  case llvm::Attribute::DereferenceableOrNull:
    return "dereferenceable_or_null";
  case llvm::Attribute::Alignment:
    return "align";
  case llvm::Attribute::SExt:
    return "signext";
  case llvm::Attribute::ZExt:
    return "zeroext";
  case llvm::Attribute::InReg:
    return "inreg";
  default:
    // Every other attribute, including the string attributes, which only a
    // written annotation can produce and which the language does not have.
    return nullptr;
  }
}

void scanAttributes(const llvm::AttributeList& attributes, const llvm::Function& function,
                    std::vector<IRDiagnostic>& out) {
  const auto check = [&](const llvm::Attribute& attribute) {
    // A string attribute has no enum kind at all, and `getKindAsEnum` asserts
    // over one -- the question is asked first, so the accessor is only reached
    // for the attributes it is defined for.
    if (!attribute.isEnumAttribute()) {
      return;
    }
    const char* name = forbiddenAttributeName(attribute.getKindAsEnum());
    if (name == nullptr) {
      return;
    }
    add(out, IRDiagnosticCode::Assumption,
        "`" + function.getName().str() + "` carries the attribute `" + name +
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

// Does the condition tree of a branch test `subject`? Both guards -- `checkedDiv`
// and `checkedFloatToInt` -- are a comparison of the *operand* against something
// (zero, a range bound, `INT_MIN`), so an operation whose operand is never
// compared is one that skipped its guard. Asked of the condition tree rather than
// of the immediate comparison because a guard may combine its tests with `or`.
[[nodiscard]] bool testsValue(const llvm::Value* condition, const llvm::Value* subject, int depth) {
  if (condition == nullptr || depth > 8) {
    return false;
  }
  // `CmpInst` and not `ICmpInst`: the float guard's test is an `fcmp`, and the
  // tree walk is about operand identity, not about the comparison's type.
  if (const auto* comparison = llvm::dyn_cast<llvm::CmpInst>(condition)) {
    return comparison->getOperand(0) == subject || comparison->getOperand(1) == subject;
  }
  // **The address's integer image counts as the address.** The checked build's
  // alignment guard is `ptrtoint address` against the low bits, because an
  // alignment is a property of the number and not of the pointer LLVM keeps; a
  // walk that only compared values would call that guard invisible and report a
  // guarded access as unguarded. `ptrtoint` and `bitcast` are the two ways to ask
  // about an address without changing which address it is.
  if (const auto* cast = llvm::dyn_cast<llvm::CastInst>(condition)) {
    const unsigned opcode = cast->getOpcode();
    if ((opcode == llvm::Instruction::PtrToInt || opcode == llvm::Instruction::BitCast) &&
        cast->getOperand(0) == subject) {
      return true;
    }
  }
  if (const auto* binary = llvm::dyn_cast<llvm::BinaryOperator>(condition)) {
    if (binary->getOpcode() == llvm::Instruction::And ||
        binary->getOpcode() == llvm::Instruction::Or) {
      return testsValue(binary->getOperand(0), subject, depth + 1) ||
             testsValue(binary->getOperand(1), subject, depth + 1);
    }
  }
  return false;
}

[[nodiscard]] bool guardedDivision(const llvm::BinaryOperator& division) {
  const llvm::Value* divisor = division.getOperand(1);
  // A constant divisor is proven by being constant, and this is asked **before**
  // the block is looked at. Two reasons, and the second is a bug this rule used
  // to have: `x / 3` folds `icmp eq 3, 0` to `false`, so the comparison the
  // source's guard wrote is not in the module any more -- and a constant divisor
  // needs no predecessor to be safe, while an operation in the *entry block* has
  // none to find. The rotate lowering is where that showed up: `urem %count, 32`
  // is proven by its own operand and sits at the top of the function, and the
  // scan called it unguarded.
  //
  // A constant zero divisor is not a missing guard either: it traps on every
  // execution, which is the language's *defined* answer for the operation.
  if (llvm::isa<llvm::ConstantInt>(divisor)) {
    return true;
  }
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
  return testsValue(branch->getCondition(), divisor, /*depth=*/0);
}

// Is the operand of this conversion tested before it converts? The guard
// `checkedFloatToInt` emits is an `fcmp` of the operand against the destination's
// two bounds, with the trap on the failing edge, and the `fptosi`/`fptoui` sits in
// the block the passing edge enters -- so the shape is the division row's exactly.
//
// **No constant-operand escape**, and it is worth saying why the division row has
// one and this does not: a constant divisor folds its own test away (`icmp eq 3,
// 0` is `false`), and a constant divisor needs no test to be safe. A constant
// *float* operand is not the same case: the lowering decides one instead of
// guarding it (`checkedFloatToInt`), so the two disagreeing possibilities -- a
// folded-away test and a value that may not fit -- are both absent, and an
// `fptosi` over a constant in this module is somebody's hand-written instruction
// rather than something this compiler emitted.
[[nodiscard]] bool guardedFloatToInt(const llvm::CastInst& cast) {
  const llvm::BasicBlock* block = cast.getParent();
  if (block == nullptr) {
    return false;
  }
  const llvm::BasicBlock* predecessor = block->getUniquePredecessor();
  if (predecessor == nullptr) {
    return false;
  }
  const auto* branch = llvm::dyn_cast<llvm::BranchInst>(predecessor->getTerminator());
  if (branch == nullptr || !branch->isConditional()) {
    return false;
  }
  return testsValue(branch->getCondition(), cast.getOperand(0), /*depth=*/0);
}

// Is this access reached through a test of its address?
//
// The test is looked for in the chain of unique conditional predecessors and not
// in the block immediately above, because one access can carry three guards in a
// row: the null test's passing edge enters the alignment test's block, whose
// passing edge enters the bounds test's block, and only the *first* of the three
// compares the address itself. Walking the chain is what makes the rule about the
// access rather than about which guard happened to be emitted last.
//
// A block with no unique predecessor stops the walk: two paths into the block mean
// the guard is on one of them, which is not a guard for the access.
[[nodiscard]] bool guardedAddress(const llvm::Instruction& access, const llvm::Value* address) {
  constexpr int kMaxGuardChain = 8;
  const llvm::BasicBlock* block = access.getParent();
  // **The block immediately above is a conditional branch.** Without this, the
  // rule would be satisfied by an access emitted in a straight line whose address
  // some *earlier* guard happened to test -- which is a module where a guard was
  // dropped and the file still passes. The test below is the other half: the
  // branch's condition is about *this* address.
  if (block != nullptr) {
    const llvm::BasicBlock* immediate = block->getUniquePredecessor();
    const auto* terminator = immediate == nullptr
                                 ? nullptr
                                 : llvm::dyn_cast<llvm::BranchInst>(immediate->getTerminator());
    if (terminator == nullptr || !terminator->isConditional()) {
      return false;
    }
  }
  for (int depth = 0; block != nullptr && depth < kMaxGuardChain; ++depth) {
    const llvm::BasicBlock* predecessor = block->getUniquePredecessor();
    if (predecessor == nullptr) {
      return false;
    }
    const auto* branch = llvm::dyn_cast<llvm::BranchInst>(predecessor->getTerminator());
    if (branch == nullptr || !branch->isConditional()) {
      return false;
    }
    if (testsValue(branch->getCondition(), address, /*depth=*/0)) {
      return true;
    }
    block = predecessor;
  }
  return false;
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
                     const llvm::DataLayout& layout, const ScanOptions& options,
                     std::vector<IRDiagnostic>& out) {
  const std::string where =
      "`" + function.getName().str() + "` in block `" +
      (instruction.getParent() != nullptr ? instruction.getParent()->getName().str()
                                          : std::string("?")) +
      "`";

  // **A permit-list, not a deny-list** (`codegen.md`, decision 16). The one
  // permitted attachment is the debug location, and the reason is the reason the
  // list exists at all: metadata is a claim the optimizer is licensed to exploit,
  // and `!dbg` licenses no transformation -- it says only *where in the source
  // this came from*. Everything else (`!tbaa` above all, since aliasing is not
  // typed here) is a claim about the program that this compiler was not asked to
  // make. Written as "nothing except" rather than "no `!tbaa`" so a new metadata
  // kind cannot join the exceptions by not being mentioned.
  if (instruction.hasMetadataOtherThanDebugLoc()) {
    add(out, IRDiagnosticCode::Assumption,
        "an instruction in " + where +
            " carries metadata other than a debug location; the language emits none");
  }

  // The other half of the debug-representation rule: `#dbg_declare` records, never
  // `llvm.dbg.*` intrinsic calls. The reference forbids the two in one module --
  // mixing them produces a module that verifies and a debugger that lies -- so the
  // scan asserts the form the lowering is supposed to have produced, which is the
  // only way a future change to `DIBuilder`'s default can be caught here rather
  // than by a person with `gdb` open.
  if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
    if (const llvm::Function* callee = call->getCalledFunction()) {
      if (callee->getName().starts_with("llvm.dbg.")) {
        add(out, IRDiagnosticCode::Assumption,
            "`" + callee->getName().str() + "` reached the module in " + where +
                "; debug information is built from records, and the two forms may not coexist");
      }
    }
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

  // The second operation with a precondition, and the same shape of test: the
  // operand is compared against the destination's bounds, and the conversion only
  // happens on the edge that passed. A bare one is *poison* for an operand the
  // destination cannot hold, and a check the optimizer may delete is not a check
  // (`casts.md`, *Float → integer*).
  if (const auto* cast = llvm::dyn_cast<llvm::CastInst>(&instruction)) {
    const unsigned opcode = cast->getOpcode();
    if ((opcode == llvm::Instruction::FPToSI || opcode == llvm::Instruction::FPToUI) &&
        !guardedFloatToInt(*cast)) {
      add(out, IRDiagnosticCode::UnguardedOp,
          "a float-to-integer conversion in " + where +
              " is not reached through a test of its operand; the language defines the "
              "conversion as a trap on a value the destination cannot hold");
    }
  }

  // --- the checked build's guards --------------------------------------------
  //
  // A module that was built with `-fcheck` promises that every access through a
  // pointer is guarded (`checks.md`, `ir.md` § *The checked build's guards*), and a
  // guard is exactly the kind of invariant this file exists for: a forgotten one
  // is not a compile error anywhere, it is a program that reads address zero in
  // the *checked* build -- which is the one build where the promise is load
  // bearing.
  //
  // The rule reads the *shape the lowering emits*, which is the same shape the two
  // rows above read: a test of the address, and the access in the block the passing
  // edge enters. It is walked transitively and not one step, because one access may
  // carry three guards in a row (null, then alignment, then bounds) and only the
  // first of them tests the address itself.
  //
  // The three escapes are one sentence: a check is unnecessary exactly when the
  // object under the address is one the compiler -- or the ABI -- put there. A
  // **constant** address is a global (never null) or a null constant, and an
  // access through a null constant is refused before the lowering
  // (`sema-address-from-constant`, `casts.md` decision 11b), and a constant
  // comparison folds, which is why the escape is about the *address* rather than
  // about the guard (`guardBranch`, `checks.cc`); an **`alloca`** is a frame object
  // whose address is non-null by construction; an **`Argument`** is the storage of
  // a by-reference aggregate parameter, which is the pointer the calling
  // convention promised and not one the program computed (`arrays.md` decision 13).
  if (options.checks) {
    const llvm::Value* address = nullptr;
    if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
      address = load->getPointerOperand();
    } else if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
      address = store->getPointerOperand();
    }
    if (address != nullptr && !llvm::isa<llvm::Constant>(address) &&
        !llvm::isa<llvm::AllocaInst>(address) && !llvm::isa<llvm::Argument>(address) &&
        !guardedAddress(instruction, address)) {
      add(out, IRDiagnosticCode::UnguardedOp,
          "an access through a pointer in " + where +
              " is not reached through a test of its address; the checked build guards every "
              "access it did not put an object under (see `checks.md`)");
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

// A file-scope object, checked against the two things the model states about one.
//
// The instruction scan cannot see either of them. Its `align` check reads an
// access, and a global's alignment is a claim about the *object* the access goes
// through -- and `constant` is written on the object and never appears in any
// instruction, so a `constant` global is invisible to a scan that only walks
// function bodies. That is the shape of the mistake this whole file exists to
// prevent: a rule stated in a record, enforced nowhere.
void scanGlobal(const llvm::GlobalVariable& global, const llvm::DataLayout& layout,
                std::vector<IRDiagnostic>& out) {
  const std::string name = global.getName().str();

  // **No `constant`, ever** (`memory.md`, decision 15). `const` in this language
  // protects a *name*: it says the binding may not be assigned, and says nothing
  // about the bytes. LLVM's `constant` is the opposite claim -- it says no write
  // to the object happens, and a write through a pointer to it is undefined
  // behaviour rather than a diagnostic. Today the source cannot reach such a
  // write ("the address of a `const`" is refused, precisely because a pointer to
  // it would be a way to write it), which is exactly why this must be scanned
  // rather than reasoned about: it is an optimisation a future relaxation of
  // *that* refusal would silently turn into a miscompile.
  if (global.isConstant()) {
    add(out, IRDiagnosticCode::Assumption,
        "the file-scope object `" + name +
            "` is emitted as a constant, which tells the optimizer that nothing writes it; a "
            "`const` here protects a name and not memory, so the object may be written through a "
            "pointer -- see `memory.md`, decision 15");
  }

  // The alignment claim, over the object rather than over an access to it, and an
  // *equality* for the same reason the access scan uses one: understating it is
  // slow code, overstating it is undefined behaviour, and the lowering has one
  // rule for the number. A global with **no** stated alignment is not a violation:
  // LLVM then derives it from the type and the layout, which is the same value.
  const llvm::MaybeAlign stated = global.getAlign();
  if (stated.has_value()) {
    const llvm::Align required = layout.getABITypeAlign(global.getValueType());
    if (*stated != required) {
      add(out, IRDiagnosticCode::Alignment,
          "the file-scope object `" + name + "` states alignment " +
              std::to_string(stated->value()) + " where its type requires " +
              std::to_string(required.value()));
    }
  }
}

} // namespace

std::span<const ModuleAssumptionInfo> moduleAssumptions() {
  return kModuleAssumptionInfos;
}

std::span<const ModuleAssumption> allModuleAssumptions() {
  return kAllModuleAssumptions;
}

std::string_view toString(ModuleAssumption assumption) {
  for (const ModuleAssumptionInfo& info : kModuleAssumptionInfos) {
    if (info.assumption == assumption) {
      return info.name;
    }
  }
  return "unknown";
}

std::vector<IRDiagnostic> scanModule(const Module& module, const ScanOptions& options) {
  std::vector<IRDiagnostic> violations;
  if (!ModuleAccess::built(module)) {
    return violations;
  }
  const llvm::Module& llvmModule = ModuleAccess::llvmModule(module);
  const llvm::DataLayout& layout = ModuleAccess::layout(module);

  // The objects first, then the bodies: the order the module declares things in,
  // so a violation reads in the order a reader of the dump meets it.
  for (const llvm::GlobalVariable& global : llvmModule.globals()) {
    scanGlobal(global, layout, violations);
  }
  for (const llvm::Function& function : llvmModule) {
    scanAttributes(function.getAttributes(), function, violations);
    for (const llvm::BasicBlock& block : function) {
      for (const llvm::Instruction& instruction : block) {
        scanInstruction(instruction, function, layout, options, violations);
      }
    }
  }
  return violations;
}

} // namespace minc::ir
