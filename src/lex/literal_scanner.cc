// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Internal headers are included relatively so the `src/lex` directory never
// has to be on the include path: anything reachable as "lex/..." is public.
#include "literal_scanner.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "byte_class.h"
#include "support/consteval/suffix.h"

namespace minc::lex::detail {
namespace {

[[nodiscard]] Token makeToken(std::uint32_t offset, std::size_t end, TokenKind kind,
                              TokenFlags flags) {
  return Token{offset, static_cast<std::uint32_t>(end) - offset, kind, flags};
}

// ---------------------------------------------------------------------------
// Numbers
// ---------------------------------------------------------------------------

// `_` is the modern spelling of a digit separator and C23's is `'`; both are
// *claimed into the token* and removed before the value is read, so neither can
// change what a number means (`literals.md`, decisions 1-3).
[[nodiscard]] constexpr bool isSeparator(Byte c) {
  return c == static_cast<Byte>('_') || c == static_cast<Byte>('\'');
}

struct DigitRun {
  std::size_t end = 0;
  bool anyDigit = false;
  // A separator whose left or right neighbour is not a digit of this run: a
  // leading one (`0x_FF`), a trailing one (`1000_`, `10_u8`), or a doubled pair
  // (`1__0`). The placement rule is one sentence, and this is where it is
  // decided -- the reader states it, and the flag makes it visible in `check`.
  bool misplaced = false;
};

// One run of digits of one base, separators included.
//
// A separator *before* a digit of this run is claimed too (`0x_FF` has no other
// reading), so one mistake gets one sentence instead of `0x` and a stray
// identifier. A leading separator of a *new* run is not a special case: `_1000`
// never reaches here, because an identifier is matched before a number ever
// starts.
//
// `'` is claimed only when a digit of this base follows it, which is what keeps
// `1'a'` two tokens: a quote with a character after it is a character literal,
// and a separator that stole it would turn a plain mistake into two. Nothing
// legal depends on the difference -- `1'a'` has no reading -- but a lexer that
// cannot be surprised by the source is the whole reason this is one function.
[[nodiscard]] DigitRun scanDigitRun(std::string_view text, std::size_t i, unsigned base) {
  DigitRun run;
  bool lastWasSeparator = false;
  while (i < text.size()) {
    const Byte c = static_cast<Byte>(text[i]);
    if (isDigitInBase(c, base)) {
      run.anyDigit = true;
      lastWasSeparator = false;
      ++i;
      continue;
    }
    if (!isSeparator(c)) {
      break;
    }
    // A leading separator (right after the prefix, or the first byte of a run),
    // a doubled pair, and -- checked below -- a trailing one.
    const bool leading = !run.anyDigit;
    const bool doubled = lastWasSeparator;
    const bool quoted = c == static_cast<Byte>('\'');
    if (quoted && (i + 1 >= text.size() || !isDigitInBase(static_cast<Byte>(text[i + 1]), base))) {
      break; // `1'a'`: a quote that does not separate two digits of this run
    }
    if (leading || doubled) {
      run.misplaced = true;
    }
    lastWasSeparator = true;
    ++i;
  }
  run.end = i;
  if (lastWasSeparator) {
    run.misplaced = true; // `1000_`, `10_u8`, `1e1_`
  }
  return run;
}

// The digits of one exponent, which are decimal in both spellings (`e` and `p`).
//
// Consumes `marker`, its optional sign, and the digits. Returns the marker
// unchanged when no digit follows -- so `1else` lexes as `1` then `else` -- but
// claims the run when what follows is a separator, because `1e_5` was meant to
// be an exponent and the sentence about separator placement is a better answer
// than "a literal and a name may not be written together".
[[nodiscard]] std::size_t scanExponent(std::string_view text, std::size_t marker,
                                       TokenFlags& flags) {
  std::size_t i = marker + 1;
  if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
    ++i;
  }
  if (i >= text.size()) {
    return marker;
  }
  const Byte c = static_cast<Byte>(text[i]);
  if (!isAsciiDigit(c) && !isSeparator(c)) {
    return marker;
  }
  const DigitRun run = scanDigitRun(text, i, 10);
  if (run.misplaced) {
    flags |= flagOf(TokenFlag::MisplacedSeparator);
  }
  return run.end;
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

// The tail every float spelling shares: the exponent when one is written, and the
// suffix whether or not one is.
//
// `.5e3`, `1.5e3`, `1_000.5f32` and `0x1.8p3` differ only in what comes *before*
// this point in the grammar, so they share one tail and cannot drift apart: a
// spelling that had no integer part in front of its point used to stop at the
// fraction and leave `e3` as an identifier, which is the same number written in a
// shape the language refused.
[[nodiscard]] NumberParts scanFloatTail(std::string_view text, unsigned base, std::size_t i,
                                        NumberParts parts) {
  const std::size_t size = text.size();
  const char c = i < size ? text[i] : '\0';
  const bool isExponentMarker = base == 16 ? (c == 'p' || c == 'P') : (c == 'e' || c == 'E');
  if (isExponentMarker) {
    const std::size_t next = scanExponent(text, i, parts.flags);
    if (next != i) {
      i = next;
      parts.isFloat = true;
    }
  }

  // The suffix, and whether the token has one at all is this scanner's answer:
  // `10u8` is one literal, while `10z` is the number `10` and then the name `z`,
  // because a trailing run counts only when it is a spelling the language knows
  // (`suffix.h`). The run is claimed *into the token*, so the reader sees the
  // bytes the source wrote and nothing has to be reassembled.
  std::size_t suffixLength = support::suffixLengthAt(text, i, parts.isFloat);
  // `10_u8`, `1.5_f32`: a separator is not part of the suffix, but the run has no
  // reading at all -- the separator is grouped with the suffix so the mistake gets
  // the sentence about placement, and not "a literal and a name may not be written
  // together" about a name nobody meant to write (`literals.md`, decision 9).
  if (suffixLength == 0 && i < size && isSeparator(static_cast<Byte>(text[i]))) {
    const std::size_t afterSeparator = support::suffixLengthAt(text, i + 1, parts.isFloat);
    if (afterSeparator != 0) {
      parts.flags |= flagOf(TokenFlag::MisplacedSeparator);
      ++i;
      suffixLength = afterSeparator;
    }
  }
  if (suffixLength != 0) {
    const support::LiteralSuffix suffix =
        support::classifySuffix(text.substr(i, suffixLength), parts.isFloat);
    // A float suffix on an integer-spelled literal *makes* it a float: `12f` is
    // `12.0` as an `f32`, and the value is exact (`casts.md`). The token kind is
    // therefore decided after the suffix rather than before it.
    if (suffix.makesFloat) {
      parts.isFloat = true;
    }
    i += suffixLength;
  }

  parts.end = i;
  return parts;
}

[[nodiscard]] NumberParts scanNumberParts(std::string_view text, std::uint32_t offset) {
  const std::size_t size = text.size();
  std::size_t i = offset;
  NumberParts parts;

  if (text[i] == '.') {
    // `.5`. The caller only routes here when a digit follows the dot. The point is
    // consumed here and everything after the digits is the code every other float
    // takes, so `.5e3`, `.5f32` and `.5e-2` are spellings of one number and not a
    // second-class shape that stops at the fraction.
    const DigitRun fraction = scanDigitRun(text, i + 1, 10);
    if (fraction.misplaced) {
      parts.flags |= flagOf(TokenFlag::MisplacedSeparator);
    }
    parts.isFloat = true;
    return scanFloatTail(text, 10, fraction.end, parts);
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

  const DigitRun whole = scanDigitRun(text, i, base);
  i = whole.end;
  if (whole.misplaced) {
    parts.flags |= flagOf(TokenFlag::MisplacedSeparator);
  }
  // `0x.8p3`: a hexadecimal float whose digits all sit after the point. C allows
  // it (`hexadecimal-fractional-constant` with no integer part) and this is the
  // one shape the fraction rule below cannot reach, because there is nothing in
  // front of the point for it to follow.
  const bool fractionWithoutWhole = base == 16 && i < size && text[i] == '.' && i + 1 < size &&
                                    isHexDigit(static_cast<Byte>(text[i + 1]));
  if (!whole.anyDigit && !fractionWithoutWhole) {
    // `0x` and nothing after. The prefix is still a token so the stream stays
    // lossless and the caller gets one clear diagnostic instead of `0` `x`.
    parts.flags |= flagOf(TokenFlag::MissingDigits);
    parts.end = i;
    return parts;
  }

  // Fractions and exponents only exist for decimal and hex floats. A '.' keeps
  // the literal only when a digit of the current base follows it, which leaves
  // `5.` as `5` `.` -- decision 8: a trailing point is the one spelling whose
  // meaning would change if the language grew member access.
  if (base == 10 || base == 16) {
    if (i < size && text[i] == '.' && i + 1 < size &&
        isDigitInBase(static_cast<Byte>(text[i + 1]), base)) {
      const DigitRun fraction = scanDigitRun(text, i + 1, base);
      if (fraction.misplaced) {
        parts.flags |= flagOf(TokenFlag::MisplacedSeparator);
      }
      i = fraction.end;
      parts.isFloat = true;
    }

    // The fraction is done; the exponent and the suffix are the shared tail.
    return scanFloatTail(text, base, i, parts);
  }

  parts.end = i;
  return parts;
}

// ---------------------------------------------------------------------------
// Escapes
// ---------------------------------------------------------------------------

// The escape alphabet that is one byte wide, as a value: -1 when the byte after
// a backslash is not one of them. `\e` is here because every terminal protocol
// is written with it and GCC and Clang have accepted it in C and in C++ for
// decades (`literals.md`, the escape table).
[[nodiscard]] constexpr int simpleEscape(Byte c) {
  switch (c) {
  case static_cast<Byte>('n'):
    return 10;
  case static_cast<Byte>('r'):
    return 13;
  case static_cast<Byte>('t'):
    return 9;
  case static_cast<Byte>('v'):
    return 11;
  case static_cast<Byte>('f'):
    return 12;
  case static_cast<Byte>('b'):
    return 8;
  case static_cast<Byte>('a'):
    return 7;
  case static_cast<Byte>('e'):
    return 27; // ESC
  default:
    return -1;
  }
}

// The result of scanning one escape, which is what the caller has to know: a
// continuation contributes no code unit to a character literal, and a backslash
// at the end of the input leaves the literal unterminated.
// The base type is explicit for the same reason `TokenFlag`'s is: an enumerator
// set this small belongs in a byte, and the width is a decision rather than
// whatever the compiler picked.
enum class EscapeScan : std::uint8_t {
  Unit,
  Continuation,
  EndOfInput,
};

// A run of digits of an escape, saturated above every limit any escape has so a
// long run cannot wrap around the range test it is about to be given.
struct EscapeDigitsRun {
  std::size_t end = 0;
  std::uint32_t value = 0;
  std::size_t digits = 0;
};

[[nodiscard]] EscapeDigitsRun scanEscapeDigits(std::string_view text, std::size_t i, unsigned base,
                                               std::size_t maxDigits) {
  constexpr std::uint32_t kSaturated = 0x110000U;
  EscapeDigitsRun run;
  while (i < text.size() && (maxDigits == 0 || run.digits < maxDigits)) {
    const Byte c = static_cast<Byte>(text[i]);
    const bool isDigit = base == 16U ? isHexDigit(c) : isOctalDigit(c);
    if (!isDigit) {
      break;
    }
    if (run.value < kSaturated) {
      run.value =
          run.value * base +
          (base == 16U ? hexValue(c) : static_cast<std::uint32_t>(c - static_cast<Byte>('0')));
    }
    ++i;
    ++run.digits;
  }
  run.end = i;
  return run;
}

// Scans the escape at `i` (which points at the backslash), advancing `i` past it
// and ORing any problem into `flags`.
//
// Every problem this can find is a *flag* and not a message, and that is a
// requirement rather than a style: an escape the reader would refuse has to be
// visible in `check`, or the program passes the checker and is discovered by the
// lowering as an internal error -- which is what happened to `"\x1FF"` and to
// `'\400'` before this (`literals.md`, *The failure model this record repairs*).
//
// What is *not* here is as decided as what is: a character literal's own width
// rule (one byte) is the checker's, because only the checker holds the type and
// the value at once and can say what to write instead. Flagging it here as well
// would put two sentences on one mistake, and the flag's sentence cannot know
// whether the fix is a wider integer or a string. A `str` is the other way round:
// nothing later reads its escapes, so this is the only stage that can refuse one.
[[nodiscard]] EscapeScan scanEscape(std::string_view text, std::size_t& i, TokenFlags& flags,
                                    bool charLiteral) {
  ++i; // past the backslash
  if (i >= text.size()) {
    return EscapeScan::EndOfInput;
  }
  // `\` immediately before a line ending: the line continues and the escape
  // contributes nothing. `CRLF` is handled as one ending, because the source may
  // come from either kind of machine and the value must not depend on which.
  if (text[i] == '\n' || text[i] == '\r') {
    if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
      i += 2;
    } else {
      ++i;
    }
    return EscapeScan::Continuation;
  }

  const Byte c = static_cast<Byte>(text[i]);
  if (simpleEscape(c) >= 0) {
    ++i;
    return EscapeScan::Unit;
  }
  switch (c) {
  case static_cast<Byte>('?'):
  case static_cast<Byte>('"'):
  case static_cast<Byte>('\''):
  case static_cast<Byte>('\\'):
    ++i;
    return EscapeScan::Unit;
  case static_cast<Byte>('x'):
  case static_cast<Byte>('o'): {
    const unsigned base = c == static_cast<Byte>('x') ? 16U : 8U;
    if (i + 1 < text.size() && text[i + 1] == '{') {
      const EscapeDigitsRun run = scanEscapeDigits(text, i + 2, base, /*maxDigits=*/0);
      if (run.digits == 0 || run.end >= text.size() || text[run.end] != '}') {
        flags |= flagOf(TokenFlag::EscapeDigits);
        i = run.end;
        return EscapeScan::Unit;
      }
      if (!charLiteral && run.value > 0xFFU) {
        flags |= flagOf(TokenFlag::EscapeTooWide);
      }
      i = run.end + 1;
      return EscapeScan::Unit;
    }
    const EscapeDigitsRun run = scanEscapeDigits(text, i + 1, base, /*maxDigits=*/0);
    if (run.digits == 0) {
      flags |= flagOf(TokenFlag::EscapeDigits);
      ++i;
      return EscapeScan::Unit;
    }
    // C's own rule is that a hexadecimal escape takes as many digits as follow
    // and that a value which does not fit is *unspecified*. The first half is
    // kept, because it is what keeps `"\x041"` the byte `A`; the second half is
    // refused, because "unspecified" is what this language is organized against.
    if (!charLiteral && run.value > 0xFFU) {
      flags |= flagOf(TokenFlag::EscapeTooWide);
    }
    i = run.end;
    return EscapeScan::Unit;
  }
  case static_cast<Byte>('u'):
  case static_cast<Byte>('U'): {
    if (i + 1 < text.size() && text[i + 1] == '{') {
      const EscapeDigitsRun run = scanEscapeDigits(text, i + 2, 16U, /*maxDigits=*/0);
      if (run.digits == 0 || run.end >= text.size() || text[run.end] != '}') {
        flags |= flagOf(TokenFlag::EscapeDigits);
        i = run.end;
        return EscapeScan::Unit;
      }
      // The digits are all there, but the value still has to name a character
      // that exists: an encoding constraint, not a type question, so the type
      // system is not needed to decide it here.
      if (!isUnicodeScalarValue(run.value)) {
        flags |= flagOf(TokenFlag::InvalidEscapeValue);
      }
      i = run.end + 1;
      return EscapeScan::Unit;
    }
    const std::size_t want = c == static_cast<Byte>('u') ? 4U : 8U;
    std::size_t next = i + 1;
    std::size_t seen = 0;
    std::uint32_t value = 0;
    while (seen < want && next < text.size() && isHexDigit(static_cast<Byte>(text[next]))) {
      value = value * 16U + hexValue(static_cast<Byte>(text[next]));
      ++next;
      ++seen;
    }
    if (seen != want) {
      flags |= flagOf(TokenFlag::EscapeDigits);
      i = next;
      return EscapeScan::Unit;
    }
    if (!isUnicodeScalarValue(value)) {
      flags |= flagOf(TokenFlag::InvalidEscapeValue);
    }
    i = next;
    return EscapeScan::Unit;
  }
  case static_cast<Byte>('N'): {
    // C++23's named universal character escape. Refused *by name* rather than
    // left to the unknown-escape arm, so the sentence can say what to write:
    // the name table is a data dependency this compiler does not carry
    // (`literals.md`, decision 16).
    flags |= flagOf(TokenFlag::NamedEscape);
    ++i; // `N`
    if (i < text.size() && text[i] == '{') {
      std::size_t next = i + 1;
      while (next < text.size() && text[next] != '}' && text[next] != '\n' && text[next] != '\r') {
        ++next;
      }
      i = next < text.size() && text[next] == '}' ? next + 1 : next;
    }
    return EscapeScan::Unit;
  }
  default:
    break;
  }

  if (isOctalDigit(c)) {
    // C's own limit, and it is what keeps `"\1012"` the byte `A` followed by
    // `2`: a fourth digit is a character, not part of the escape.
    const EscapeDigitsRun run = scanEscapeDigits(text, i, 8U, /*maxDigits=*/3);
    if (!charLiteral && run.value > 0xFFU) {
      flags |= flagOf(TokenFlag::EscapeTooWide);
    }
    i = run.end;
    return EscapeScan::Unit;
  }

  flags |= flagOf(TokenFlag::UnknownEscape);
  ++i;
  return EscapeScan::Unit;
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
      if (scanEscape(text, i, flags, /*charLiteral=*/false) == EscapeScan::EndOfInput) {
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
      const EscapeScan scan = scanEscape(text, i, flags, /*charLiteral=*/true);
      if (scan == EscapeScan::EndOfInput) {
        break;
      }
      // A line continuation is no code unit at all: `'\<newline>a'` is `'a'`,
      // which is C's rule (splicing happens before a token exists) and the
      // reason this counts units and not bytes.
      if (scan == EscapeScan::Unit) {
        ++units;
      }
      continue;
    }
    ++i;
    ++units;
  }

  if (!terminated) {
    flags |= flagOf(TokenFlag::UnterminatedChar);
  } else if (units == 0) {
    // `''` is never a valid character literal. A *multi-unit* literal is not
    // flagged here: `'ab'` and `'\xFF\xFF'` are spellings, and the rule that
    // refuses them is the language's, which the checker states with the value in
    // hand (`literals.md`, decisions 21-23).
    flags |= flagOf(TokenFlag::EmptyCharLiteral);
  }
  return makeToken(offset, i, TokenKind::CharLiteral, flags);
}

} // namespace minc::lex::detail
