// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Byte classification shared by the lexer and the literal scanner.
//
// Internal to the lex module: not installed, not included from `include/`.
// Everything here is `constexpr` and allocation-free so it inlines into the
// scanning loops and stays testable.
//
// The classification is deliberately ASCII-only. Source text is already known
// to be valid UTF-8 (SourceManager validates it), so a byte >= 0x80 can only
// appear inside a comment, a string, or an escape, and anywhere else it is an
// `Invalid` token. Unicode identifiers would be a language decision, and this
// is the single place that decision would change.
#pragma once

#include <cstdint>
#include <string_view>

namespace minc::lex {

using Byte = unsigned char;

[[nodiscard]] constexpr bool isAsciiDigit(Byte c) {
  return c >= static_cast<Byte>('0') && c <= static_cast<Byte>('9');
}

// Numeric value of a hex digit, or 0xFF when `c` is not one.
[[nodiscard]] constexpr Byte hexValue(Byte c) {
  if (c >= static_cast<Byte>('0') && c <= static_cast<Byte>('9')) {
    return static_cast<Byte>(c - static_cast<Byte>('0'));
  }
  if (c >= static_cast<Byte>('a') && c <= static_cast<Byte>('f')) {
    return static_cast<Byte>(c - static_cast<Byte>('a') + 10U);
  }
  if (c >= static_cast<Byte>('A') && c <= static_cast<Byte>('F')) {
    return static_cast<Byte>(c - static_cast<Byte>('A') + 10U);
  }
  return 0xFF;
}

[[nodiscard]] constexpr bool isHexDigit(Byte c) {
  return hexValue(c) != 0xFF;
}

[[nodiscard]] constexpr bool isOctalDigit(Byte c) {
  return c >= static_cast<Byte>('0') && c <= static_cast<Byte>('7');
}

// Digit test that works for every base the literal scanner accepts (2, 8, 10,
// 16). The `c >= '0'` guard keeps the subtraction from underflowing on bytes
// below the digit range.
[[nodiscard]] constexpr bool isDigitInBase(Byte c, unsigned base) {
  if (base == 16U) {
    return isHexDigit(c);
  }
  return c >= static_cast<Byte>('0') && static_cast<unsigned>(c - static_cast<Byte>('0')) < base;
}

[[nodiscard]] constexpr bool isIdentifierStart(Byte c) {
  return (c >= static_cast<Byte>('a') && c <= static_cast<Byte>('z')) ||
         (c >= static_cast<Byte>('A') && c <= static_cast<Byte>('Z')) ||
         c == static_cast<Byte>('_');
}

[[nodiscard]] constexpr bool isIdentifierContinue(Byte c) {
  return isIdentifierStart(c) || isAsciiDigit(c);
}

// True for a UTF-8 continuation byte (10xxxxxx). Used only to keep one
// non-ASCII character inside a single token; the lexer never decodes UTF-8.
[[nodiscard]] constexpr bool isContinuationByte(Byte c) {
  return (c & static_cast<Byte>(0xC0U)) == static_cast<Byte>(0x80U);
}

// Length of the line terminator at `offset` (LF, CRLF, or a lone CR), or 0
// when there is none.
//
// Recognising all three here is what makes the token stream identical for a
// Unix, a Windows, and a classic-Mac checkout of the same file, and it is the
// same rule LineTable uses so positions and tokens cannot disagree.
[[nodiscard]] constexpr std::uint32_t newlineLengthAt(std::string_view text, std::size_t offset) {
  if (offset >= text.size()) {
    return 0;
  }
  const char c = text[offset];
  if (c == '\n') {
    return 1;
  }
  if (c == '\r') {
    // A CRLF pair is one terminator owned by one token, so the CR can never
    // leak into a rendered line.
    return offset + 1 < text.size() && text[offset + 1] == '\n' ? 2U : 1U;
  }
  return 0;
}

} // namespace minc::lex
