// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// What the parser reads from.
//
// Deliberately tiny, and deliberately an interface. The parser today reads a
// file's significant tokens; the preprocessor will read what the lexer produces
// and hand the parser its expanded stream. If the parser talked to
// `lex::TokenStream` directly, that second source would be a rewrite of the
// grammar instead of a second implementation of this. Design record for the
// preprocessor side: `docs/architectures/preprocessor.md`.
//
// Trivia is never returned: the parser is trivia-blind by design. Attaching
// trivia to the tree is the builder's job, not the grammar's.
#pragma once

#include <cstdint>
#include <memory>

#include "lex/token_kind.h"
#include "support/span/span.h"

namespace minc::lex {
class TokenStream;
} // namespace minc::lex

namespace minc::parse {

class TokenSource {
public:
  TokenSource() = default;
  TokenSource(const TokenSource&) = delete;
  TokenSource& operator=(const TokenSource&) = delete;
  virtual ~TokenSource() = default;

  // The token the parser is looking at. Returns `EndOfFile` once the source is
  // exhausted, so the parser never has to special-case the end.
  [[nodiscard]] virtual lex::TokenKind current() const = 0;

  // Lookahead: `nth(0)` is `current()`. Clamped, so `nth` far past the end
  // returns `EndOfFile` rather than reading out of bounds.
  [[nodiscard]] virtual lex::TokenKind nth(std::uint32_t n) const = 0;

  // True once `current()` is the end-of-file token. `bump()` is still valid --
  // it emits the end-of-file leaf -- and a second call does nothing.
  [[nodiscard]] virtual bool atEnd() const = 0;

  // Consumes the current token. Never moves past the end.
  virtual void bump() = 0;

  // Span of the current token, for the caret on an error.
  [[nodiscard]] virtual support::Span spanOfCurrent() const = 0;
};

// A TokenSource over a file's significant tokens. Returned as a pointer to the
// interface so the parser cannot accidentally depend on the concrete type, and
// so a token-tree source can be substituted without touching the grammar.
[[nodiscard]] std::unique_ptr<TokenSource> makeTokenStreamSource(const lex::TokenStream& stream);

} // namespace minc::parse
