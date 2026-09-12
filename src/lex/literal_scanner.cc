// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Internal headers are included relatively so the `src/lex` directory never
// has to be on the include path: anything reachable as "lex/..." is public.
#include "literal_scanner.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "byte_class.h"

namespace minc::lex::detail {
namespace {

[[nodiscard]] Token makeToken(std::uint32_t offset, std::size_t end, TokenKind kind,
                              TokenFlags flags) {
  return Token{offset, static_cast<std::uint32_t>(end) - offset, kind, flags};
}

// ---------------------------------------------------------------------------
// Numbers
// ---------------------------------------------------------------------------

[[nodiscard]] std::size_t scanDigits(std::string_view text, std::size_t i, unsigned base) {
  while (i < text.size() && isDigitInBase(static_cast<Byte>(text[i]), base)) {
    ++i;
  }
  return i;
}

// Consumes an exponent (`e`/`E` for decimal, `p`/`P` for hex) with its optional
// sign and at least one digit. Returns `marker` unchanged when no digit
// follows, so `1else` lexes as `1` then `else` instead of a broken float.
[[nodiscard]] std::size_t scanExponent(std::string_view text, std::size_t marker) {
  std::size_t i = marker + 1;
  if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
    ++i;
  }
  if (i >= text.size() || !isAsciiDigit(static_cast<Byte>(text[i]))) {
    return marker;
  }
  return scanDigits(text, i, 10);
}

// Value of a run of hex digits, which the caller has already verified are all
// hex digits. Four digits fit in 16 bits, eight in 32.
[[nodiscard]] constexpr std::uint32_t hexValueOf(std::string_view digits) {
  std::uint32_t value = 0;
  for (const char c : digits) {
    value = value * 16U + hexValue(static_cast<Byte>(c));
  }
  return value;
}

// A `\u`/`\U` escape may name any code point UTF-8 can encode: in range, and
// not a surrogate half. C constrains universal character names the same way,
// and the alternative is a literal that cannot be represented at all.
[[nodiscard]] constexpr bool isUnicodeScalarValue(std::uint32_t value) {
  return value <= 0x10FFFFU && (value < 0xD800U || value > 0xDFFFU);
}

struct NumberParts {
  std::size_t end = 0;
  bool isFloat = false;
  TokenFlags flags = 0;
};

[[nodiscard]] NumberParts scanNumberParts(std::string_view text, std::uint32_t offset) {
  const std::size_t size = text.size();
  std::size_t i = offset;
  NumberParts parts;

  if (text[i] == '.') {
    // `.5`. The caller only routes here when a digit follows the dot.
    parts.isFloat = true;
    parts.end = scanDigits(text, i + 1, 10);
    return parts;
  }

  // Base prefix. A leading zero *without* a prefix is plain decimal: C's
  // implicit octal (`010` meaning eight) is a well-known footgun and `.mx`
  // spells octal out as `0o` instead. See docs/architectures/lexer.md.
  unsigned base = 10;
  if (text[i] == '0' && i + 1 < size) {
    switch (text[i + 1]) {
    case 'x':
    case 'X':
      base = 16;
      break;
    case 'b':
    case 'B':
      base = 2;
      break;
    case 'o':
    case 'O':
      base = 8;
      break;
    default:
      break;
    }
    if (base != 10) {
      i += 2;
    }
  }

  const std::size_t digitsBegin = i;
  i = scanDigits(text, i, base);
  if (i == digitsBegin) {
    // `0x` and nothing after. The prefix is still a token so the stream stays
    // lossless and the caller gets one clear diagnostic instead of `0` `x`.
    parts.flags |= flagOf(TokenFlag::MissingDigits);
    parts.end = i;
    return parts;
  }

  // Fractions and exponents only exist for decimal and hex floats. A '.' keeps
  // the literal only when a digit of the current base follows it, which leaves
  // `1.` as `1` `.` and keeps member access unambiguous if the language ever
  // grows it.
  if (base == 10 || base == 16) {
    if (i < size && text[i] == '.' && i + 1 < size &&
        isDigitInBase(static_cast<Byte>(text[i + 1]), base)) {
      i = scanDigits(text, i + 1, base);
      parts.isFloat = true;
    }

    const char c = i < size ? text[i] : '\0';
    const bool isExponentMarker = base == 16 ? (c == 'p' || c == 'P') : (c == 'e' || c == 'E');
    if (isExponentMarker) {
      const std::size_t next = scanExponent(text, i);
      if (next != i) {
        i = next;
        parts.isFloat = true;
      }
    }
  }

  parts.end = i;
  return parts;
}

// ---------------------------------------------------------------------------
// Escapes
// ---------------------------------------------------------------------------

// Scans the escape at `i` (which points at the backslash), advancing `i` past
// it and ORing any problem into `flags`.
//
// Returns false when the backslash runs into the end of the line: the caller
// then reports the enclosing literal as unterminated, which is the only
// structurally broken thing a backslash can do here. Everything else is a
// well-formed range of characters even when the escape itself is unknown.
[[nodiscard]] bool scanEscape(std::string_view text, std::size_t& i, TokenFlags& flags) {
  ++i; // past the backslash
  if (i >= text.size() || text[i] == '\n' || text[i] == '\r') {
    return false;
  }

  const Byte c = static_cast<Byte>(text[i]);
  switch (c) {
  case static_cast<Byte>('n'):
  case static_cast<Byte>('r'):
  case static_cast<Byte>('t'):
  case static_cast<Byte>('v'):
  case static_cast<Byte>('f'):
  case static_cast<Byte>('b'):
  case static_cast<Byte>('a'):
  case static_cast<Byte>('?'):
  case static_cast<Byte>('"'):
  case static_cast<Byte>('\''):
  case static_cast<Byte>('\\'):
    ++i;
    return true;
  case static_cast<Byte>('x'): {
    ++i;
    const std::size_t begin = i;
    while (i < text.size() && isHexDigit(static_cast<Byte>(text[i]))) {
      ++i;
    }
    if (i == begin) {
      flags |= flagOf(TokenFlag::MissingDigits);
    }
    return true;
  }
  case static_cast<Byte>('u'):
  case static_cast<Byte>('U'): {
    const std::size_t want = c == static_cast<Byte>('u') ? 4U : 8U;
    ++i;
    const std::size_t digitsBegin = i;
    while (i - digitsBegin < want && i < text.size() && isHexDigit(static_cast<Byte>(text[i]))) {
      ++i;
    }
    if (i - digitsBegin != want) {
      flags |= flagOf(TokenFlag::MissingDigits);
      return true;
    }
    // The digits are all there, but the value still has to name a character
    // that exists. Checking it here rather than later is the same call the
    // escape alphabet already gets: it is an encoding constraint, not a type
    // question, so the type system is not needed to decide it.
    if (!isUnicodeScalarValue(hexValueOf(text.substr(digitsBegin, want)))) {
      flags |= flagOf(TokenFlag::InvalidEscapeValue);
    }
    return true;
  }
  default:
    break;
  }

  if (isOctalDigit(c)) {
    std::size_t seen = 0;
    while (seen < 3 && i < text.size() && isOctalDigit(static_cast<Byte>(text[i]))) {
      ++i;
      ++seen;
    }
    return true;
  }

  flags |= flagOf(TokenFlag::UnknownEscape);
  ++i;
  return true;
}

} // namespace

Token scanNumber(std::string_view text, std::uint32_t offset) {
  const NumberParts parts = scanNumberParts(text, offset);
  return makeToken(offset, parts.end,
                   parts.isFloat ? TokenKind::FloatLiteral : TokenKind::IntegerLiteral,
                   parts.flags);
}

Token scanString(std::string_view text, std::uint32_t offset) {
  TokenFlags flags = 0;
  std::size_t i = offset + 1;
  bool terminated = false;

  while (i < text.size()) {
    const char c = text[i];
    if (c == '"') {
      ++i;
      terminated = true;
      break;
    }
    if (c == '\n' || c == '\r') {
      break; // a literal never crosses a line; see lexer.h
    }
    if (c == '\\') {
      if (!scanEscape(text, i, flags)) {
        break;
      }
      continue;
    }
    ++i;
  }

  if (!terminated) {
    flags |= flagOf(TokenFlag::UnterminatedString);
  }
  return makeToken(offset, i, TokenKind::StringLiteral, flags);
}

Token scanChar(std::string_view text, std::uint32_t offset) {
  TokenFlags flags = 0;
  std::size_t i = offset + 1;
  std::size_t units = 0; // characters or escapes seen between the quotes
  bool terminated = false;

  while (i < text.size()) {
    const char c = text[i];
    if (c == '\'') {
      ++i;
      terminated = true;
      break;
    }
    if (c == '\n' || c == '\r') {
      break;
    }
    if (c == '\\') {
      if (!scanEscape(text, i, flags)) {
        break;
      }
      ++units;
      continue;
    }
    ++i;
    ++units;
  }

  if (!terminated) {
    flags |= flagOf(TokenFlag::UnterminatedChar);
  } else if (units == 0) {
    // `''` is never a valid character literal. Multi-character literals are
    // deliberately *not* flagged here: what `'ab'` means is a type question.
    flags |= flagOf(TokenFlag::EmptyCharLiteral);
  }
  return makeToken(offset, i, TokenKind::CharLiteral, flags);
}

} // namespace minc::lex::detail
