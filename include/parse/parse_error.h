// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// A syntax error, as a value.
//
// Errors are deliberately *not* part of the syntax tree. An `Error` node marks
// where the offending bytes are; this carries what was wrong with them. Keeping
// them apart means the tree stays a pure value, the same tree can be compared
// byte for byte across runs, and a consumer that only wants structure never
// walks diagnostics.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "support/span/span.h"

namespace minc::parse {

// The closed set of things that can go wrong in the grammar.
//
// A code is an enumerator rather than a string literal at each call site for
// the same reason `TokenFlag` is: the parser cannot invent a code that no test
// and no documentation knows about, two sites cannot spell the same condition
// two ways, and `allParseErrorCodes()` can be checked against the inputs that
// produce them, so a code with no trigger is caught instead of shipped dead.
enum class ParseErrorCode : std::uint8_t {
  // A required token was absent; `expect()` inserts a zero-width one.
  ExpectedToken,
  // Something where a declaration (today: a function) was required.
  ExpectedItem,
  // A name slot with nothing name-shaped in it.
  ExpectedName,
  // A type position with nothing type-shaped in it.
  ExpectedType,
  // An expression position with nothing expression-shaped in it.
  ExpectedExpression,
  // A token that cannot start a statement.
  ExpectedStatement,
  // A function declaration with neither a body nor `extern`. The two spellings
  // are exactly what tells a definition from a declaration, so one of them has
  // to be there; the message names the one to type.
  MissingExtern,
  // `extern fn` with a body. `extern` says the definition lives in another unit,
  // so a body here contradicts the word rather than being an extra detail.
  ExternWithBody,
  // `extern` written before a binding (`extern let x: i32;`). The *form* is not
  // built yet -- a global has storage, an initializer, and an address the loader
  // initializes rather than this unit -- so the word is refused by name instead
  // of being misread as a function declaration that lost its `fn`.
  ExternBinding,
  // `static` written together with `extern`. The two words answer one question
  // -- who may see this name -- with opposite answers, so both cannot be right.
  ConflictingLinkage,
  // `static` somewhere other than in front of a declaration. It is a declaration
  // word, not a statement one: a function-local `static` is a different feature.
  StaticPosition,
  // `...` in a function that has a body. *Reading* a variadic argument needs
  // `va_start`, which the language does not have, so only a declaration may be
  // variadic -- the marker in a definition would be a function nobody can write.
  VariadicDefinition,
  // `...` somewhere other than the end of a parameter list, or with no parameter
  // before it. The marker ends the list, so a list with anything after it has no
  // meaning to give the arguments that follow.
  VariadicPosition,
  // The parser stopped: too many errors, or input nested past the guard.
  Aborted,
};

// Code and its stable short name (`parse-expected-token`). Keeping them in one
// row means the string a user greps for cannot drift from the enumerator, and
// searching a code in the source finds the enumerator that produced it.
struct ParseErrorCodeInfo {
  ParseErrorCode code;
  const char* name;
};

[[nodiscard]] std::span<const ParseErrorCodeInfo> parseErrorCodeInfos();

// Every code, derived from the table above rather than listed a second time, so
// a code added to the enum without a row is caught by the tests.
[[nodiscard]] std::span<const ParseErrorCode> allParseErrorCodes();

// The stable short name (`parse-expected-token`), never localized.
[[nodiscard]] std::string_view toString(ParseErrorCode code);

struct ParseError {
  // Byte range inside one file. A recovered "expected ';'" points at the spot
  // the token is missing from, so the caret lands where the user must type.
  support::Span span;
  std::string message;
  ParseErrorCode code = ParseErrorCode::Aborted;

  // The name to report, without a second copy of the code in this struct.
  [[nodiscard]] std::string_view codeName() const {
    return toString(code);
  }
};

} // namespace minc::parse
