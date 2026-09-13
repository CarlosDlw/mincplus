// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// A resolution problem, as a value.
//
// Same shape as every other stage: this layer never reports. Only
// `resolve_report.h` knows about severity and `DiagBag`, which is what keeps
// `minc_resolve` free of the diagnostic machinery and lets the resolver be
// tested with no `Session` and no terminal.
//
// The safety codes are in here on purpose: hitting a budget *is* an error with a
// name, never a silent truncation and never a hang -- the same rule the lexer
// and the preprocessor follow.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "support/span/span.h"

namespace minc::resolve {

enum class ResolveErrorCode : std::uint8_t {
  // No declaration in the requested namespace is visible under that name.
  UnknownName,
  // The same scope and namespace declares two incompatible things.
  Redeclaration,
  // A declaration nothing refers to (warning; `-Wunused`).
  UnusedEntity,
  // A declaration hides another that is still in scope (warning; `-Wshadow`).
  ShadowedName,
  // Budgets. Each is always on, checked before the insertion, with a test.
  LimitDefs,
  LimitScopes,
  LimitRefs,
};

struct ResolveErrorCodeInfo {
  ResolveErrorCode code;
  const char* name;
  bool warning;
};

[[nodiscard]] std::span<const ResolveErrorCodeInfo> resolveErrorCodeInfos();
// Derived from the table, so a code added to the enum without a row is caught.
[[nodiscard]] std::span<const ResolveErrorCode> allResolveErrorCodes();
[[nodiscard]] std::string_view toString(ResolveErrorCode code);
[[nodiscard]] bool isWarning(ResolveErrorCode code);

struct ResolveError {
  support::Span span;
  std::string message;
  ResolveErrorCode code = ResolveErrorCode::UnknownName;
  // A secondary diagnostic, when there is one worth printing. Used for "the name
  // exists in another namespace": one error, one note, rather than two errors
  // for one mistake.
  std::string note;
  support::Span noteSpan;
};

} // namespace minc::resolve
