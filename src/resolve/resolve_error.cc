// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "resolve/resolve_error.h"

#include <array>
#include <cstddef>
#include <string_view>
#include <utility>

namespace minc::resolve {
namespace {

// The codes, in one table: the name a user greps for and the severity cannot
// drift from the enumerator, and a code added to the enum without a row is a
// compile error here rather than a hole to find.
// NOLINTBEGIN(readability-identifier-naming): table name follows the project's
// convention for the other stages' code tables.
constexpr std::array<ResolveErrorCodeInfo, 8> kResolveErrorCodeInfos{{
    {ResolveErrorCode::UnknownName, "resolve-unknown-name", false},
    {ResolveErrorCode::Redeclaration, "resolve-redeclaration", false},
    {ResolveErrorCode::UnusedEntity, "resolve-unused-entity", true},
    {ResolveErrorCode::ShadowedName, "resolve-shadowed-name", true},
    {ResolveErrorCode::ReservedIdentifier, "resolve-reserved-identifier", false},
    {ResolveErrorCode::LimitDefs, "resolve-limit-defs", false},
    {ResolveErrorCode::LimitScopes, "resolve-limit-scopes", false},
    {ResolveErrorCode::LimitRefs, "resolve-limit-refs", false},
}};
// NOLINTEND(readability-identifier-naming)

template <std::size_t... Indexes>
[[nodiscard]] constexpr auto codesFromTable(std::index_sequence<Indexes...>) {
  return std::array<ResolveErrorCode, sizeof...(Indexes)>{kResolveErrorCodeInfos[Indexes].code...};
}

constexpr auto kAllResolveErrorCodes =
    codesFromTable(std::make_index_sequence<kResolveErrorCodeInfos.size()>{});

} // namespace

std::span<const ResolveErrorCodeInfo> resolveErrorCodeInfos() {
  return kResolveErrorCodeInfos;
}

std::span<const ResolveErrorCode> allResolveErrorCodes() {
  return kAllResolveErrorCodes;
}

std::string_view toString(ResolveErrorCode code) {
  for (const ResolveErrorCodeInfo& info : kResolveErrorCodeInfos) {
    if (info.code == code) {
      return info.name;
    }
  }
  return "resolve-unknown";
}

bool isWarning(ResolveErrorCode code) {
  for (const ResolveErrorCodeInfo& info : kResolveErrorCodeInfos) {
    if (info.code == code) {
      return info.warning;
    }
  }
  return false;
}

} // namespace minc::resolve
