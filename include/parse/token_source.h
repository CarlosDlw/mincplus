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
#include <string_view>

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

  // How many tokens have been handed out before `current()` -- zero at the
  // start, one after one `bump()`. Trivia is not a token: a source that walks a
  // stream carrying it (the preprocessor's) counts the ones the parser sees.
  //
  // The parser asks it for exactly one thing, and it is worth saying which,
  // because a rule written against an absolute position is a rule that breaks
  // when the tokens before it change. A **bound**: a declaration's return type is
  // read from a run whose end a scan found (`fn Vec<i32> f()` must end the type
  // before `f`), and the bound is `position() + count`. Nothing else asks, and
  // nothing else may.
  [[nodiscard]] virtual std::uint32_t position() const = 0;

  // Consumes the current token. Never moves past the end.
  virtual void bump() = 0;

  // Span of the current token, for the caret on an error.
  [[nodiscard]] virtual support::Span spanOfCurrent() const = 0;

  // Span of the token `nth(n)` looks at, clamped like `nth` itself. Two tokens
  // are *written together* when one's end is the other's start in the same file,
  // and that is a question only the spans can answer: `10z` and `10 z` are the
  // same two token kinds and two different programs (`casts.md`).
  [[nodiscard]] virtual support::Span spanOf(std::uint32_t n) const = 0;

  // The spelling of the token `nth(n)` looks at, exactly as the source wrote it.
  //
  // The kind is what almost every rule is written against, and this exists for
  // the one place where the *text* is the rule: `_`, the inferred count, is an
  // `Identifier` like every name, and `[n]` and `[_]` are a parse error and a
  // legal count respectively. Empty past the end, like the empty span above.
  //
  // A source that does not *hold* the bytes answers empty, and that is part of
  // the contract rather than a gap: a token's spelling can live in a file this
  // source never saw (a macro body, a header), and a caller that needs the text
  // needs a source over the text -- which is `TokenStreamSource` over a stream
  // whose tokens tile it. The grammar's use is fail-safe by construction: a rule
  // written against a spelling that is not there does not match, and a `[_]`
  // that does not match is a *refusal* about the count, never a wrong reading of
  // it.
  [[nodiscard]] virtual std::string_view textOf(std::uint32_t n) const = 0;
};

// A TokenSource over a file's significant tokens. Returned as a pointer to the
// interface so the parser cannot accidentally depend on the concrete type, and
// so a token-tree source can be substituted without touching the grammar.
[[nodiscard]] std::unique_ptr<TokenSource> makeTokenStreamSource(const lex::TokenStream& stream);

} // namespace minc::parse
