// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "sema/sema_error.h"

#include <array>
#include <cstddef>
#include <string_view>
#include <utility>

namespace minc::sema {
namespace {

// The codes, in one table: the name a user greps for and the severity cannot
// drift from the enumerator, and a code added to the enum without a row is a
// compile error here rather than a hole to find.
// NOLINTBEGIN(readability-identifier-naming): table name follows the project's
// convention for the other stages' code tables.
// The size is **deduced** (`std::to_array`): it used to be written out, so a code
// added to the enum and the table also needed an edit to a number in a file the
// reader was not editing -- and forgetting it failed the build there. The table is
// now the only thing that decides how big it is.
constexpr auto kSemaErrorCodeInfos = std::to_array<SemaErrorCodeInfo>({
    {SemaErrorCode::UnknownType, "sema-unknown-type", false},
    {SemaErrorCode::MalformedType, "sema-malformed-type", false},
    {SemaErrorCode::TypeAliasCycle, "sema-type-alias-cycle", false},
    {SemaErrorCode::TypeNotValue, "sema-type-not-value", false},
    {SemaErrorCode::LiteralOutOfRange, "sema-literal-out-of-range", false},
    {SemaErrorCode::ConditionNotBool, "sema-condition-not-bool", false},
    {SemaErrorCode::InvalidOperands, "sema-invalid-operands", false},
    {SemaErrorCode::ComparisonChain, "sema-comparison-chain", false},
    {SemaErrorCode::InvalidAssignment, "sema-invalid-assignment", false},
    {SemaErrorCode::AssignToConst, "sema-assign-to-const", false},
    {SemaErrorCode::IncDecNotLvalue, "sema-incdec-not-lvalue", false},
    {SemaErrorCode::NeverReturns, "sema-never-returns", false},
    {SemaErrorCode::NeverBodyCompletes, "sema-never-body-completes", false},
    {SemaErrorCode::NotAFunction, "sema-not-a-function", false},
    {SemaErrorCode::ArgumentCount, "sema-argument-count", false},
    {SemaErrorCode::ReturnMismatch, "sema-return-mismatch", false},
    {SemaErrorCode::ReturnMissingValue, "sema-return-missing-value", false},
    {SemaErrorCode::ReturnVoidValue, "sema-return-void-value", false},
    {SemaErrorCode::MissingReturn, "sema-missing-return", false},
    {SemaErrorCode::MainSignature, "sema-main-signature", false},
    {SemaErrorCode::FunctionRedefinition, "sema-function-redefinition", false},
    {SemaErrorCode::SignatureMismatch, "sema-signature-mismatch", false},
    {SemaErrorCode::ExternAggregate, "sema-extern-aggregate", false},
    {SemaErrorCode::UnknownMember, "sema-unknown-member", true},
    {SemaErrorCode::VariadicAggregate, "sema-variadic-aggregate", true},
    {SemaErrorCode::SliceNotViewable, "sema-slice-not-viewable", false},
    {SemaErrorCode::SlicePointerNeedsBothBounds, "sema-slice-pointer-needs-both-bounds", false},
    {SemaErrorCode::SliceBoundsReversed, "sema-slice-bounds-reversed", false},
    {SemaErrorCode::BreakOutsideLoop, "sema-break-outside-loop", false},
    {SemaErrorCode::ContinueOutsideLoop, "sema-continue-outside-loop", false},
    {SemaErrorCode::DivisionByZero, "sema-division-by-zero", false},
    {SemaErrorCode::ConstantOutOfRange, "sema-constant-out-of-range", false},
    {SemaErrorCode::ShiftCountOutOfRange, "sema-shift-count-out-of-range", false},
    {SemaErrorCode::UseBeforeAssignment, "sema-use-before-assignment", false},
    {SemaErrorCode::DerefNotPointer, "sema-deref-not-pointer", false},
    {SemaErrorCode::PointerVoidAccess, "sema-pointer-void-access", false},
    {SemaErrorCode::PointerVoidArithmetic, "sema-pointer-void-arithmetic", false},
    {SemaErrorCode::AddressOfNonLvalue, "sema-address-of-non-lvalue", false},
    {SemaErrorCode::AddressOfConst, "sema-address-of-const", false},
    {SemaErrorCode::IndexNotInteger, "sema-index-not-integer", false},
    {SemaErrorCode::IndexOutOfRange, "sema-index-out-of-range", false},
    {SemaErrorCode::PointerMismatch, "sema-pointer-mismatch", false},
    {SemaErrorCode::PointerInteger, "sema-pointer-integer", false},
    {SemaErrorCode::BuiltinNotAValue, "sema-builtin-not-a-value", false},
    {SemaErrorCode::BuiltinWidth, "sema-builtin-width", false},
    {SemaErrorCode::GlobalNotConstant, "sema-global-not-constant", false},
    {SemaErrorCode::GlobalCycle, "sema-global-cycle", false},
    {SemaErrorCode::LimitTypes, "sema-limit-types", false},
    {SemaErrorCode::UnreachableCode, "sema-unreachable-code", true},
    {SemaErrorCode::LiteralTypeUnknown, "sema-literal-type-unknown", false},
    {SemaErrorCode::InitializerShape, "sema-initializer-shape", false},
    {SemaErrorCode::ImplicitConversion, "sema-implicit-conversion", true},
    {SemaErrorCode::CastInvalid, "sema-cast-invalid", false},
    {SemaErrorCode::CastLoses, "sema-cast-loses", true},
    {SemaErrorCode::ProvenanceCast, "sema-provenance-cast", true},
    {SemaErrorCode::AddressFromConstant, "sema-address-from-constant", false},
    {SemaErrorCode::DestructuringArity, "sema-destructuring-arity", false},
    {SemaErrorCode::DestructuringNotProduct, "sema-destructuring-not-product", false},
    {SemaErrorCode::GenericExtern, "sema-generic-extern", false},
    {SemaErrorCode::GenericMain, "sema-generic-main", false},
    {SemaErrorCode::GenericNameNotValue, "sema-generic-name-not-value", false},
    {SemaErrorCode::GenericNotInferable, "sema-generic-not-inferable", false},
    {SemaErrorCode::GenericTypeArgs, "sema-generic-type-args", false},
    {SemaErrorCode::TypeConstantUnknown, "sema-type-constant-unknown", false},
    {SemaErrorCode::TypeConstantNotGranted, "sema-type-constant-not-granted", false},
    {SemaErrorCode::GenericOperation, "sema-generic-operation", false},
    {SemaErrorCode::ConstraintNotAClass, "sema-constraint-not-a-class", false},
    {SemaErrorCode::ConstraintUnsatisfied, "sema-constraint-unsatisfied", false},
    {SemaErrorCode::GenericInstanceLimit, "sema-generic-instance-limit", false},
});
// NOLINTEND(readability-identifier-naming)

template <std::size_t... Indexes>
[[nodiscard]] constexpr auto codesFromTable(std::index_sequence<Indexes...>) {
  return std::array<SemaErrorCode, sizeof...(Indexes)>{kSemaErrorCodeInfos[Indexes].code...};
}

constexpr auto kAllSemaErrorCodes =
    codesFromTable(std::make_index_sequence<kSemaErrorCodeInfos.size()>{});

} // namespace

std::span<const SemaErrorCodeInfo> semaErrorCodeInfos() {
  return kSemaErrorCodeInfos;
}

std::span<const SemaErrorCode> allSemaErrorCodes() {
  return kAllSemaErrorCodes;
}

std::string_view toString(SemaErrorCode code) {
  for (const SemaErrorCodeInfo& info : kSemaErrorCodeInfos) {
    if (info.code == code) {
      return info.name;
    }
  }
  return "sema-unknown";
}

bool isWarning(SemaErrorCode code) {
  for (const SemaErrorCodeInfo& info : kSemaErrorCodeInfos) {
    if (info.code == code) {
      return info.warning;
    }
  }
  return false;
}

} // namespace minc::sema
