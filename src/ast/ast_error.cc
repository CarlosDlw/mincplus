// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "ast/ast_error.h"

#include <array>
#include <cstddef>
#include <string_view>
#include <utility>

namespace minc::ast {
namespace {

// The codes, in one table: adding a code is a row here plus an enumerator in
// ast_error.h. `toString` and `allAstErrorCodes()` read this instead of
// repeating the list, and the tests require every row to be reachable from some
// input. The count is explicit so a code added to the enum without a row -- or a
// row added twice -- is a compile error here rather than a hole to find.
// NOLINTBEGIN(readability-identifier-naming): the table name is a convention.
constexpr std::array<AstErrorCodeInfo, 3> kAstErrorCodeInfos{{
    {AstErrorCode::MissingTypeOrInitializer, "ast-missing-type"},
    {AstErrorCode::ConstantWithoutInitializer, "ast-const-without-value"},
    {AstErrorCode::NodeLimit, "ast-node-limit"},
}};
// NOLINTEND(readability-identifier-naming)

// Derived, not listed again: the pack expansion initializes every slot from the
// table, so no element is ever zero-initialized into a code that does not exist.
template <std::size_t... Indexes>
[[nodiscard]] constexpr auto codesFromTable(std::index_sequence<Indexes...>) {
  return std::array<AstErrorCode, sizeof...(Indexes)>{kAstErrorCodeInfos[Indexes].code...};
}

constexpr auto kAllAstErrorCodes =
    codesFromTable(std::make_index_sequence<kAstErrorCodeInfos.size()>{});

} // namespace

std::span<const AstErrorCodeInfo> astErrorCodeInfos() {
  return kAstErrorCodeInfos;
}

std::span<const AstErrorCode> allAstErrorCodes() {
  return kAllAstErrorCodes;
}

std::string_view toString(AstErrorCode code) {
  for (const AstErrorCodeInfo& info : kAstErrorCodeInfos) {
    if (info.code == code) {
      return info.name;
    }
  }
  return "ast-unknown";
}

} // namespace minc::ast
