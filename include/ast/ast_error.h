// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// A structural error, as a value.
//
// Same shape as the lexer's flags, the parser's `ParseError` and the
// preprocessor's `PPError`: this stage never reports. `resolve_report.h` is the
// only file that knows about severity, so `minc_ast` links no diagnostics and
// the checks can be tested without a `Session` or a terminal.
//
// Design record: `docs/architectures/resolve.md`.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "support/span/span.h"

namespace minc::ast {

// The closed set. A code is an enumerator with one table row rather than a
// string literal at each call site, so the stage cannot invent a code no test
// knows about and two sites cannot spell the same condition two ways.
//
// It is short on purpose, and it grows with the language rather than with the
// implementation: a rule is here only once the syntax that uses it exists, so
// every code is reachable from a real input and therefore testable. The
// structural table in `resolve.md` names the checks still to come (`return`
// outside a function, a variadic parameter that is not last, a nested `fn`); they
// land with their syntax, not with a placeholder that can never fire.
enum class AstErrorCode : std::uint8_t {
  // A `let`/`const` with neither a type annotation nor an initializer. The
  // parser accepts the shape (both parts are optional) and only the language can
  // say the binding is then unknowable.
  MissingTypeOrInitializer,
  // A `const` with a type but no initializer. Immutable and never given a value
  // is a binding that can never be read, so the language forbids it; the parser
  // sees two independent optional clauses and cannot tell.
  ConstantWithoutInitializer,
  // Past `support::kMaxAstNodesPerUnit`. A hazard bound, not a language rule.
  NodeLimit,
};

struct AstErrorCodeInfo {
  AstErrorCode code;
  const char* name;
};

[[nodiscard]] std::span<const AstErrorCodeInfo> astErrorCodeInfos();

// Every code, derived from the table, so a code added to the enum without a row
// is caught by the tests.
[[nodiscard]] std::span<const AstErrorCode> allAstErrorCodes();

[[nodiscard]] std::string_view toString(AstErrorCode code);

struct AstError {
  support::Span span;
  std::string message;
  AstErrorCode code = AstErrorCode::MissingTypeOrInitializer;
};

} // namespace minc::ast
