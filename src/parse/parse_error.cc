// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "parse/parse_error.h"

#include <array>
#include <cstddef>
#include <utility>

namespace minc::parse {
namespace {

// The codes, in one table. Adding a code is a row here plus an enumerator in
// parse_error.h; `toString` and the derived `allParseErrorCodes()` read this
// instead of repeating the list, and the tests require every row to be
// reachable from some input.
constexpr std::array<ParseErrorCodeInfo, 9> kParseErrorCodeInfos{{
    {ParseErrorCode::ExpectedToken, "parse-expected-token"},
    {ParseErrorCode::ExpectedItem, "parse-expected-item"},
    {ParseErrorCode::ExpectedName, "parse-expected-name"},
    {ParseErrorCode::ExpectedType, "parse-expected-type"},
    {ParseErrorCode::ExpectedExpression, "parse-expected-expression"},
    {ParseErrorCode::ExpectedStatement, "parse-expected-statement"},
    {ParseErrorCode::MissingExtern, "parse-missing-extern"},
    {ParseErrorCode::ExternWithBody, "parse-extern-with-body"},
    {ParseErrorCode::Aborted, "parse-aborted"},
}};

// Derived, not listed again: a code added to the table above is picked up by
// every consumer of allParseErrorCodes() without a second edit that could be
// forgotten. The pack expansion initializes every slot from the table, so no
// element is ever value-initialized to a zero that is not a valid code.
template <std::size_t... Indexes>
[[nodiscard]] constexpr auto codesFromTable(std::index_sequence<Indexes...>) {
  return std::array<ParseErrorCode, sizeof...(Indexes)>{kParseErrorCodeInfos[Indexes].code...};
}

constexpr auto kAllParseErrorCodes =
    codesFromTable(std::make_index_sequence<kParseErrorCodeInfos.size()>{});

} // namespace

std::span<const ParseErrorCodeInfo> parseErrorCodeInfos() {
  return kParseErrorCodeInfos;
}

std::span<const ParseErrorCode> allParseErrorCodes() {
  return kAllParseErrorCodes;
}

std::string_view toString(ParseErrorCode code) {
  for (const ParseErrorCodeInfo& info : kParseErrorCodeInfos) {
    if (info.code == code) {
      return info.name;
    }
  }
  return "parse-unknown";
}

} // namespace minc::parse
