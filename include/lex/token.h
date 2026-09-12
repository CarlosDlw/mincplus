// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// A lexed token: 12 bytes, trivially copyable, and never owning text.
//
// The lexeme is always `text.substr(offset, length)` of the source the token
// came from, so the lexer copies nothing and the whole file stays the single
// source of truth. Because every byte of the input belongs to exactly one
// token, offsets are contiguous and the token stream is lossless.
#pragma once

#include <cstdint>
#include <span>

#include "lex/token_kind.h"

namespace minc::lex {

// Problems found while recognising a token. They are *flags*, not
// diagnostics: the lexer stays free of span/severity/reporting concerns and
// one pass can therefore report every lexical error instead of bailing out at
// the first. `lex_report.h` turns these into diagnostics.
//
// Every flag describes something structurally wrong with the lexeme itself,
// never anything about its meaning. `\q` is flagged because the escape
// alphabet is part of the lexical grammar; a number that does not fit in i32
// is *not* flagged, because that needs the type system.
//
// The 1-byte base type is load-bearing, not a size micro-optimization: a ninth
// flag would be a shift that does not fit in eight bits, and the enumerator
// value would no longer be representable in the underlying type, so the build
// fails instead of silently truncating the flag to nothing.
//
// The enumerators are ordered to match `flagInfos()`, which is the order
// diagnostics are reported in.
enum class TokenFlag : std::uint8_t {
  None = 0,
  UnterminatedString = 1U << 0U,
  UnterminatedChar = 1U << 1U,
  UnterminatedBlockComment = 1U << 2U,
  UnknownEscape = 1U << 3U,      // `\q`
  InvalidEscapeValue = 1U << 4U, // `\uD800`, `\U00110000`
  EmptyCharLiteral = 1U << 5U,   // `''`
  MissingDigits = 1U << 6U,      // `0x`, `\u`, `1e+`
};

using TokenFlags = std::uint16_t;

[[nodiscard]] constexpr TokenFlags flagOf(TokenFlag flag) {
  return static_cast<TokenFlags>(flag);
}

[[nodiscard]] constexpr bool hasFlag(TokenFlags flags, TokenFlag flag) {
  return (flags & flagOf(flag)) != 0;
}

// Every flag, derived from the `flagInfos()` table rather than listed a second
// time, so there is exactly one place to add a flag.
[[nodiscard]] std::span<const TokenFlag> allTokenFlags();

// Short stable name used by the dump and the tests (`unterminated-string`).
[[nodiscard]] const char* toString(TokenFlag flag);

// Diagnostic code, short name, and message for one flag. Keeping them in one
// row means the code the user sees and the message can never drift apart, and
// searching a code in the source finds the flag that produced it.
struct FlagInfo {
  TokenFlag flag;
  const char* code;
  const char* name;
  const char* message;
};

[[nodiscard]] std::span<const FlagInfo> flagInfos();

struct Token {
  std::uint32_t offset = 0; // byte offset of the first byte of the lexeme
  std::uint32_t length = 0; // byte length; 0 only for EndOfFile
  TokenKind kind = TokenKind::EndOfFile;
  TokenFlags flags = 0;

  [[nodiscard]] constexpr std::uint32_t end() const {
    return offset + length;
  }
  [[nodiscard]] constexpr bool isTrivia() const {
    return minc::lex::isTrivia(kind);
  }
  [[nodiscard]] constexpr bool is(TokenKind other) const {
    return kind == other;
  }
  [[nodiscard]] constexpr bool hasAnyFlag() const {
    return flags != 0;
  }
  [[nodiscard]] constexpr bool has(TokenFlag flag) const {
    return hasFlag(flags, flag);
  }
};

static_assert(sizeof(Token) <= 12, "keep tokens small; the parser walks millions of them");

} // namespace minc::lex
