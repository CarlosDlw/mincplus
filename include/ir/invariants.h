// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The invariant scan: the module this compiler built, checked against the rules
// it promised to obey.
//
// `llvm::verifyModule` proves a module is *well formed*. This proves it is
// **ours**: that the closed list of assumptions in `ir.md` § *The assumption
// list* is the list actually in the module, that every division went through a
// guard, and that every alignment -- an access's and a file-scope object's -- is
// the one its type requires. The
// verifier cannot check any of those, because none of them is a rule about LLVM
// -- they are rules about this language, and the whole reason they are written
// down is that a violation of one of them is a *miscompile* rather than a
// diagnostic.
//
// It is a separate translation unit and a separate function because it is a
// different question asked of the same artifact, and because it is the thing a
// future contributor is supposed to *extend* when they add an assumption: one
// row in one table, plus the check that reads it.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "ir/ir.h"

namespace minc::ir {

// The rows of `ir.md` § *The assumption list* that the scan checks, one
// enumerator each, with the code a violation of the row is reported as.
//
// It exists so the list is **data and not a paragraph**: `ir.md` says a new
// assumption is "one row in one table, plus the check that reads it", and a check
// whose row cannot be *tripped* is a check that stopped running without anyone
// noticing. `tests/unit/ir/invariants_test.cc` walks this enumeration and has a
// module for every row -- so an assumption added to the enum without a test input
// fails there, which is the friction that is the point.
//
// The rows are the rules with a module-level *signature*. A rule whose violation
// leaves no trace in the module cannot be scanned and is not a row: the bottom
// type's one `poison`, for instance, is not looked for here -- it is *derived*
// from the coercion record in `lowerOperand`, which is a check inside the
// lowering rather than a scan of its output.
enum class ModuleAssumption : std::uint8_t {
  // A metadata node attached to an instruction, other than the debug location
  // (`!tbaa` above all: aliasing is not typed here, `memory.md` decisions 3, 9).
  Metadata,
  // A function or parameter attribute the language did not state (`noalias`,
  // `nonnull`, `noundef`, `dereferenceable`, `align`, ...).
  FunctionAttribute,
  // An `inbounds` `getelementptr` with no recorded proof (`memory.md`, decision
  // 10). Today the rule is literally "no `inbounds` at all".
  Inbounds,
  // `nsw`/`nuw` on an integer operation, or in attribute form: the language
  // defines wrapping, so it may not promise otherwise.
  Wrapping,
  // A fast-math flag on a floating-point operation: the language defines its
  // float results, and reassociating one is not this stage's decision.
  FastMath,
  // An `llvm.dbg.*` intrinsic call in a module that uses debug *records*. The two
  // forms may not coexist -- a module that mixes them verifies and a debugger
  // lies -- and debug information is built from records here.
  DebugIntrinsic,
  // A file-scope object emitted as `constant`, which says nothing writes it.
  // `const` protects a name, not memory (`memory.md`, decision 15).
  ConstantObject,
  // An alignment -- an access's, or an object's -- that is not the one its type
  // gives. Overstating it is undefined behaviour in LLVM, not slow code.
  Alignment,
  // A division or remainder reached without a test of its divisor: the language
  // defines the operation as a trap, so the guard is part of the code.
  UnguardedDivision,
  // A float-to-integer conversion without a test of its operand. `fptosi` on a
  // value the destination cannot hold is LLVM *poison* and this language has no
  // poison, so the conversion is one more operation with a precondition -- and
  // the second row of the same shape, which is why the two are separate rows and
  // not one: the bounds and the operand are different in each.
  UnguardedFloatToInt,
};

struct ModuleAssumptionInfo {
  ModuleAssumption assumption;
  const char* name;
  // What a violation of this row is reported as. A row and a code are not one to
  // one -- five rows are `ir-assumption` because they are one claim ("the module
  // states something the language did not") about five different spellings of it.
  IRDiagnosticCode code;
};

[[nodiscard]] std::span<const ModuleAssumptionInfo> moduleAssumptions();
// Every row, derived from the table above, so a row added to the enum without one
// fails a test rather than going unscanned.
[[nodiscard]] std::span<const ModuleAssumption> allModuleAssumptions();
[[nodiscard]] std::string_view toString(ModuleAssumption assumption);

// Every violated invariant, in the order they were found. Empty means the module
// is one this compiler is allowed to have built.
//
// A non-empty result is *always* a bug in this compiler -- the program it came
// from was checked and accepted -- so a caller reports these the way it reports
// an internal error, with the module dumped beside them.
[[nodiscard]] std::vector<IRDiagnostic> scanModule(const Module& module);

} // namespace minc::ir
