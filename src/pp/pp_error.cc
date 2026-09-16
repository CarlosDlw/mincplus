// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "pp/pp_error.h"

#include <array>
#include <cstddef>
#include <utility>

namespace minc::pp {
namespace {

// The codes, in one table: adding a code is a row here plus an enumerator in
// pp_error.h. `toString` and `allPPErrorCodes()` read this instead of repeating
// the list, and the tests require every row to be reachable from some input.
// The count is explicit so a code added to the enum without a row -- or a row
// added twice -- is a compile error here rather than a hole the tests have to
// find.
constexpr std::array<PPErrorCodeInfo, 34> kPPErrorCodeInfos{{
    // NOLINT(readability-identifier-naming)
    {PPErrorCode::InvalidDirective, "pp-invalid-directive"},
    {PPErrorCode::UnknownPragma, "pp-unknown-pragma"},
    {PPErrorCode::NotSupported, "pp-not-supported"},
    {PPErrorCode::ErrorDirective, "pp-error-directive"},
    {PPErrorCode::WarningDirective, "pp-warning-directive"},
    {PPErrorCode::InvalidLineDirective, "pp-invalid-line"},
    {PPErrorCode::UnterminatedConditional, "pp-unterminated-conditional"},
    {PPErrorCode::UnexpectedConditional, "pp-unexpected-conditional"},
    {PPErrorCode::ElseAfterElse, "pp-else-after-else"},
    {PPErrorCode::ConditionalNestingExceeded, "pp-conditional-nesting"},
    {PPErrorCode::MacroRedefined, "pp-macro-redefined"},
    {PPErrorCode::ReservedIdentifier, "pp-reserved-identifier"},
    {PPErrorCode::MacroParameterLimit, "pp-macro-parameter-limit"},
    {PPErrorCode::MissingMacroArguments, "pp-missing-macro-arguments"},
    {PPErrorCode::TooManyMacroArguments, "pp-too-many-macro-arguments"},
    {PPErrorCode::UnterminatedMacroArguments, "pp-unterminated-macro-arguments"},
    {PPErrorCode::InvalidPaste, "pp-invalid-paste"},
    {PPErrorCode::InvalidHashOperand, "pp-invalid-hash-operand"},
    {PPErrorCode::MissingMacroName, "pp-missing-macro-name"},
    {PPErrorCode::ExpansionDepthExceeded, "pp-expansion-depth"},
    {PPErrorCode::ExpansionBudgetExceeded, "pp-expansion-budget"},
    {PPErrorCode::ExpressionSyntax, "pp-expression-syntax"},
    {PPErrorCode::UndefinedIdentifier, "pp-undefined-identifier"},
    {PPErrorCode::StrayHashOperator, "pp-stray-hash"},
    {PPErrorCode::InvalidPragmaOperand, "pp-invalid-pragma-operand"},
    {PPErrorCode::IncludeNotFound, "pp-include-not-found"},
    {PPErrorCode::IncludeUnreadable, "pp-include-unreadable"},
    {PPErrorCode::IncludeSelfReference, "pp-include-self-reference"},
    {PPErrorCode::IncludeDepthExceeded, "pp-include-depth"},
    {PPErrorCode::IncludeBudgetExceeded, "pp-include-budget"},
    {PPErrorCode::MissingIncludeGuard, "pp-missing-include-guard"},
    {PPErrorCode::PreprocessedBytesExceeded, "pp-output-budget"},
    {PPErrorCode::TokenTooLong, "pp-token-too-long"},
    {PPErrorCode::DateWithoutEpoch, "pp-date-without-epoch"},
}};

// Derived, not listed again: the pack expansion initializes every slot from the
// table, so no element is ever zero-initialized into a code that does not exist.
template <std::size_t... Indexes>
[[nodiscard]] constexpr auto codesFromTable(std::index_sequence<Indexes...>) {
  return std::array<PPErrorCode, sizeof...(Indexes)>{kPPErrorCodeInfos[Indexes].code...};
}

constexpr auto kAllPPErrorCodes =
    codesFromTable(std::make_index_sequence<kPPErrorCodeInfos.size()>{});

} // namespace

std::span<const PPErrorCodeInfo> ppErrorCodeInfos() {
  return kPPErrorCodeInfos;
}

std::span<const PPErrorCode> allPPErrorCodes() {
  return kAllPPErrorCodes;
}

std::string_view toString(PPErrorCode code) {
  for (const PPErrorCodeInfo& info : kPPErrorCodeInfos) {
    if (info.code == code) {
      return info.name;
    }
  }
  return "pp-unknown";
}

} // namespace minc::pp
