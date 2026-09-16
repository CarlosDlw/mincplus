// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "support/consteval/literal.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace minc::support {
namespace {

constexpr std::uint64_t kUint64Max = ~std::uint64_t{0};

// Value of one hex digit, or -1.
[[nodiscard]] int hexValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return 10 + (c - 'a');
  }
  if (c >= 'A' && c <= 'F') {
    return 10 + (c - 'A');
  }
  return -1;
}

[[nodiscard]] std::string quoted(std::string_view text) {
  return "'" + std::string(text) + "'";
}

// `_` is the modern spelling of a digit separator and `'` is C23's; both are
// *spelling*, so both are removed before a value is read and the value cannot
// depend on which one was written (`literals.md`, decision 1).
[[nodiscard]] constexpr bool isSeparator(char c) {
  return c == '_' || c == '\'';
}

[[nodiscard]] constexpr bool isOctalDigit(char c) {
  return c >= '0' && c <= '7';
}

// The escape alphabet that is one byte wide, as a value: -1 when the byte after
// a backslash is not one of them. `\e` is here because every terminal protocol
// is written with it and GCC and Clang have accepted it in C and in C++ for
// decades (`literals.md`, the escape table).
[[nodiscard]] constexpr int simpleEscapeValue(char c) {
  switch (c) {
  case 'n':
    return 10;
  case 'r':
    return 13;
  case 't':
    return 9;
  case 'v':
    return 11;
  case 'f':
    return 12;
  case 'b':
    return 8;
  case 'a':
    return 7;
  case 'e':
    return 27; // ESC
  default:
    return -1;
  }
}

// A `\u`/`\U` escape names a code point UTF-8 can encode: in range, and not a
// surrogate half. C constrains universal character names the same way, and the
// alternative is a literal that cannot be represented at all.
[[nodiscard]] constexpr bool isScalarValue(std::uint32_t value) {
  return value <= 0x10FFFFU && (value < 0xD800U || value > 0xDFFFU);
}

// A run of digits of an escape, and how many there were. `maxDigits == 0` means
// "as many as follow", which is what the braced forms and `\x` want.
struct EscapeDigits {
  std::uint32_t value = 0;
  std::size_t digits = 0;
};

// `index` walks past the digits; the caller is left pointing at whatever ended
// the run (a brace, another byte, or the end of the body).
[[nodiscard]] EscapeDigits readEscapeDigits(std::string_view body, std::size_t& index,
                                            unsigned base, std::size_t maxDigits) {
  // Saturates above every limit an escape has, so a run longer than the value
  // range cannot wrap around the test it is about to be given
  // (`\x{FFFFFFFFFFFFFFFF}` is above one byte, and must stay there).
  constexpr std::uint32_t kSaturated = 0x110000U;
  EscapeDigits run;
  while (index < body.size() && (maxDigits == 0 || run.digits < maxDigits)) {
    const int digit = hexValue(body[index]);
    if (digit < 0 || static_cast<unsigned>(digit) >= base) {
      break;
    }
    if (run.value < kSaturated) {
      run.value = run.value * base + static_cast<std::uint32_t>(digit);
    }
    ++index;
    ++run.digits;
  }
  return run;
}

} // namespace

std::optional<DecodedElement> decodeElement(std::string_view body, std::size_t index) {
  if (index >= body.size()) {
    return std::nullopt;
  }
  DecodedElement out;
  const unsigned char byte = static_cast<unsigned char>(body[index]);
  if (byte != '\\') {
    // A byte that is not a backslash is itself, and always well formed. A source
    // byte above `0x7F` is its own code unit: `"é"` is the two bytes the file
    // holds, and `str` is a byte string, so no decoding happens here.
    out.value = byte;
    out.next = index + 1;
    out.ok = true;
    return out;
  }
  if (index + 1 >= body.size()) {
    out.message = "the body ends in a backslash";
    out.next = index + 1;
    return out;
  }
  const char escape = body[index + 1];

  // `\` immediately before a line ending: the line continues and the escape
  // contributes nothing. `CRLF` is one ending, because the source may come from
  // either kind of machine and the value must not depend on which
  // (`literals.md`, decision 18).
  if (escape == '\n' || escape == '\r') {
    out.isContinuation = true;
    out.next = index + 2;
    if (escape == '\r' && out.next < body.size() && body[out.next] == '\n') {
      ++out.next;
    }
    out.ok = true;
    return out;
  }

  if (const int simple = simpleEscapeValue(escape); simple >= 0) {
    out.value = static_cast<std::uint32_t>(simple);
    out.next = index + 2;
    out.ok = true;
    return out;
  }

  switch (escape) {
  case '?':
  case '\'':
  case '"':
  case '\\':
    out.value = static_cast<std::uint32_t>(static_cast<unsigned char>(escape));
    out.next = index + 2;
    out.ok = true;
    return out;
  case 'x':
  case 'o': {
    // A byte escape: one byte wide, and the two spellings differ only in the
    // base (`literals.md`, the escape table). Both take a delimited form, which
    // is what says where the digits end (`\x41B` is `AB`, `\x{41}B` is `A` `B`).
    const bool hex = escape == 'x';
    const unsigned base = hex ? 16U : 8U;
    const std::string spelling = std::string("\\") + escape;
    std::size_t next = index + 2;
    const bool braced = next < body.size() && body[next] == '{';
    if (braced) {
      ++next;
    }
    const EscapeDigits run = readEscapeDigits(body, next, base, /*maxDigits=*/0);
    if (braced) {
      if (run.digits == 0 || next >= body.size() || body[next] != '}') {
        out.message = "`" + spelling + "{...}` needs at least one digit between its braces";
        out.next = next;
        return out;
      }
      ++next; // the closing brace
    } else if (run.digits == 0) {
      out.message =
          "`" + spelling + "` needs at least one " + (hex ? "hex " : "octal ") + "digit after it";
      out.next = next;
      return out;
    }
    // A byte escape names a *byte*, and `\x{1F600}` is 128512 -- a value this
    // spelling can carry but no byte can hold. The **width rule is the
    // consumer's** and not this reader's: a `str` refuses it (it has no byte for
    // it) and a `char` is a type that cannot hold it, which the checker states
    // with the two spellings that do mean it (`literals.md`, decision 14).
    // Refusing here as well would put a second sentence on one mistake, and this
    // one could not know which fix to name.
    out.value = run.value;
    out.next = next;
    out.ok = true;
    return out;
  }
  case 'u':
  case 'U': {
    // A universal character name: four hex digits for `\u`, eight for `\U`, or
    // any number between braces. It names a *code point*, and what a caller does
    // with one is the caller's business -- a string encodes it as UTF-8, a
    // character has to hold it in one byte (`literals.md`, decisions 13-15).
    const std::size_t unbraced = escape == 'U' ? 8U : 4U;
    std::size_t next = index + 2;
    const bool braced = next < body.size() && body[next] == '{';
    EscapeDigits run;
    if (braced) {
      ++next;
      run = readEscapeDigits(body, next, 16U, /*maxDigits=*/0);
      if (run.digits == 0 || next >= body.size() || body[next] != '}') {
        out.message = "`\\u{...}` needs at least one hex digit between its braces";
        out.next = next;
        return out;
      }
      ++next; // the closing brace
    } else {
      run = readEscapeDigits(body, next, 16U, unbraced);
      if (run.digits != unbraced) {
        out.message = "`\\" + std::string(1, escape) + "` needs exactly " +
                      std::to_string(unbraced) +
                      " hex digits; `\\u{...}` takes as many as are "
                      "between the braces";
        out.next = next;
        return out;
      }
    }
    if (!isScalarValue(run.value)) {
      out.message = "`\\" + std::string(1, escape) +
                    "...` does not name a character: a code point is at most U+10FFFF, and a "
                    "surrogate half is not a code point";
      out.next = next;
      return out;
    }
    out.value = run.value;
    out.isCodePoint = true;
    out.next = next;
    out.ok = true;
    return out;
  }
  case 'N': {
    // C++23's named universal character escape. Refused *by name*, so the
    // sentence can say what to write instead: the Unicode name table is a
    // generated data file this compiler does not carry (`literals.md`,
    // decision 16). The braces are skipped so the caller's index lands past the
    // whole escape and one mistake stays one diagnostic.
    out.message = "`\\N{...}` names a Unicode character by name, and that needs a name table this "
                  "compiler does not carry; write the code point as `\\u{...}`";
    std::size_t next = index + 2;
    if (next < body.size() && body[next] == '{') {
      ++next;
      while (next < body.size() && body[next] != '}' && body[next] != '\n' && body[next] != '\r') {
        ++next;
      }
      if (next < body.size() && body[next] == '}') {
        ++next;
      }
    }
    out.next = next;
    return out;
  }
  default:
    break;
  }

  if (isOctalDigit(escape)) {
    // C's own limit -- one to three digits -- and it is what keeps `"\1012"` the
    // byte `A` followed by `2`: a fourth digit is a character, not part of the
    // escape. As above, the *value* is what this reader owns; `\777` is 511, and
    // whether 511 fits is the consumer's question.
    std::size_t next = index + 1;
    const EscapeDigits run = readEscapeDigits(body, next, 8U, /*maxDigits=*/3);
    out.value = run.value;
    out.next = next;
    out.ok = true;
    return out;
  }

  out.message = "`\\" + std::string(1, escape) + "` is not an escape this language has";
  out.next = index + 2;
  return out;
}

std::string withoutSeparators(std::string_view number) {
  std::string out;
  out.reserve(number.size());
  for (const char c : number) {
    if (!isSeparator(c)) {
      out.push_back(c);
    }
  }
  return out;
}

IntegerLiteral parseIntegerLiteral(std::string_view text, IntegerBaseRule baseRule) {
  IntegerLiteral result;
  if (text.empty()) {
    result.message = "an empty integer literal";
    return result;
  }

  std::size_t index = 0;
  unsigned base = 10;
  if (text.size() >= 2 && text[0] == '0') {
    const char prefix = text[1];
    if (prefix == 'x' || prefix == 'X') {
      base = 16;
      index = 2;
    } else if (prefix == 'b' || prefix == 'B') {
      base = 2;
      index = 2;
    } else if (prefix == 'o' || prefix == 'O') {
      // `.mx`'s explicit octal. Accepted in both rules: it is unambiguous, and a
      // reader that accepts it in one context and not the other would be the
      // kind of difference this file exists to remove.
      base = 8;
      index = 2;
    } else if (baseRule == IntegerBaseRule::ImplicitOctal && prefix >= '0' && prefix <= '7') {
      base = 8;
      index = 1;
    }
  }

  std::uint64_t value = 0;
  bool overflowed = false;
  bool anyDigit = false;
  bool separatorMisplaced = false;
  bool lastWasSeparator = false;
  for (; index < text.size(); ++index) {
    const char c = text[index];
    if (isSeparator(c)) {
      // A separator is claimed into the token by the scanner only when it sits
      // inside a number; its *placement* is one sentence -- between two digits of
      // this run and nowhere else -- and this is where the value is read, so this
      // is where the rule is stated for the reader (`literals.md`, decision 2).
      // `_1000` never reaches here (it is a name), while `0x_FF` and `1000_` do:
      // they are claimed and refused, so one mistake gets one sentence instead of
      // `0x` and a stray identifier.
      if (!anyDigit || lastWasSeparator) {
        separatorMisplaced = true;
      }
      lastWasSeparator = true;
      continue;
    }
    const int digit = hexValue(c);
    if (digit < 0 || static_cast<unsigned>(digit) >= base) {
      break;
    }
    anyDigit = true;
    lastWasSeparator = false;
    const auto digitValue = static_cast<std::uint64_t>(digit);
    // Overflow is decided *before* multiplying, against the exact room left. A
    // "did the value shrink" or `value > max / base` test rejects
    // `0xFFFFFFFFFFFFFFFF`, which fits in 64 bits and is the very literal a
    // `#if` is most likely to compare against.
    if (value > (kUint64Max - digitValue) / base) {
      overflowed = true;
    }
    // Keep consuming digits after the overflow: the suffix scan starts where
    // the digits end, and stopping early would leave `...0bignumULL` half lexed.
    value = value * base + digitValue;
  }
  if (lastWasSeparator) {
    separatorMisplaced = true; // `1000_`, `10_u8`, `1e1_`
  }
  if (!anyDigit) {
    result.message = "malformed integer literal " + quoted(text);
    return result;
  }
  if (separatorMisplaced) {
    result.message =
        "a digit separator belongs between two digits of the same number: " + quoted(text);
    return result;
  }
  // The digits' own range, before the suffix is classified: the one thing a
  // caller with a type wider than the core needs, and the split belongs to the
  // reader that just decided where the digits stop. The separators are still in
  // it -- this is a view of the source, and the source wrote them.
  result.number = text.substr(0, index);

  // The suffix, through the one table -- the same one the scanner asked to decide
  // how long the token was, so the two cannot disagree about where the number
  // ends. An unknown run is not a suffix at all (`suffix.h`), and a reader that
  // quietly ignored one would be reading a literal the source did not write.
  const std::string_view suffixText = text.substr(index);
  result.suffix = classifySuffix(suffixText, /*literalIsFloat=*/false);
  bool isUnsigned = false;
  switch (result.suffix.status) {
  case SuffixStatus::None:
    if (!suffixText.empty()) {
      result.message = "unknown suffix in integer literal " + quoted(text);
      return result;
    }
    break;
  case SuffixStatus::Refused:
    result.message = result.suffix.message;
    return result;
  case SuffixStatus::Typed:
    // The suffix's own signedness, which is what the digits are read as: `10u` is
    // an unsigned ten, and `10i8` is a signed eight-bit ten.
    isUnsigned = isUnsignedSuffix(result.suffix.type);
    break;
  }

  if (overflowed) {
    result.value = ConstInt{value, isUnsigned};
    result.tooWide = true;
    result.message = "integer literal " + quoted(text) + " does not fit in 64 bits";
    return result;
  }
  // A decimal literal too large for `intmax_t` is evaluated as unsigned, which
  // is the standard's behaviour rather than a diagnostic -- and only when the
  // spelling did not say what it is: a suffixed literal is the type its suffix
  // names, and re-reading `300u8` as unsigned would be inventing a rule.
  if (!result.suffix.typed() && !isUnsigned && value > static_cast<std::uint64_t>(INT64_MAX)) {
    isUnsigned = true;
  }
  result.value = ConstInt{value, isUnsigned};
  result.ok = true;
  return result;
}

FloatLiteral readFloatLiteral(std::string_view text) {
  FloatLiteral result;
  result.number = numericPartOfFloat(text);
  result.suffix = classifySuffix(text.substr(result.number.size()), /*literalIsFloat=*/true);
  return result;
}

CharLiteral parseCharLiteral(std::string_view text) {
  CharLiteral result;
  // The lexer guarantees the quotes; the body may be empty or escaped, which is
  // the lexer's finding to report, not this reader's to invent.
  if (text.size() < 2) {
    result.message = "malformed character literal " + quoted(text);
    return result;
  }
  const std::string_view body = text.substr(1, text.size() - 2);
  std::uint64_t value = 0;
  std::size_t index = 0;
  while (index < body.size()) {
    const std::optional<DecodedElement> element = decodeElement(body, index);
    if (!element.has_value()) {
      break;
    }
    if (!element->ok) {
      result.message = "in character literal " + quoted(text) + ": " + element->message;
      return result;
    }
    index = element->next;
    // A line continuation is no code unit at all, which is why this counts units
    // and not bytes: `'\<newline>a'` is `'a'`.
    if (element->isContinuation) {
      continue;
    }
    // Multi-character literals are implementation-defined; this is the packed
    // value GCC produces, which is what the preprocessor needs on C input. The
    // *language's* rule -- a `char` is one byte, so one unit -- is the checker's,
    // which is why the unit count is handed over with the value
    // (`literals.md`, decisions 20-23).
    value = (value << 8U) | element->value;
    ++result.units;
  }
  result.value = ConstInt::fromSigned(static_cast<std::int64_t>(value));
  result.ok = true;
  return result;
}

namespace {

// One Unicode scalar value, encoded the way UTF-8 spells it. The source names a
// *character* and the object stores *bytes*, so the encoding happens here, once,
// rather than at every consumer of a `str`.
void appendUtf8(std::vector<std::uint8_t>& out, std::uint32_t code) {
  if (code <= 0x7FU) {
    out.push_back(static_cast<std::uint8_t>(code));
  } else if (code <= 0x7FFU) {
    out.push_back(static_cast<std::uint8_t>(0xC0U | (code >> 6U)));
    out.push_back(static_cast<std::uint8_t>(0x80U | (code & 0x3FU)));
  } else if (code <= 0xFFFFU) {
    out.push_back(static_cast<std::uint8_t>(0xE0U | (code >> 12U)));
    out.push_back(static_cast<std::uint8_t>(0x80U | ((code >> 6U) & 0x3FU)));
    out.push_back(static_cast<std::uint8_t>(0x80U | (code & 0x3FU)));
  } else {
    out.push_back(static_cast<std::uint8_t>(0xF0U | (code >> 18U)));
    out.push_back(static_cast<std::uint8_t>(0x80U | ((code >> 12U) & 0x3FU)));
    out.push_back(static_cast<std::uint8_t>(0x80U | ((code >> 6U) & 0x3FU)));
    out.push_back(static_cast<std::uint8_t>(0x80U | (code & 0x3FU)));
  }
}

} // namespace

StringLiteral parseStringLiteral(std::string_view text) {
  StringLiteral result;
  // The lexer guarantees the quotes; a body with no terminator is the lexer's
  // finding, and this reader answers about the bytes it was given.
  if (text.size() < 2) {
    result.message = "malformed string literal " + quoted(text);
    return result;
  }
  const std::string_view body = text.substr(1, text.size() - 2);
  std::size_t index = 0;
  while (index < body.size()) {
    const std::optional<DecodedElement> element = decodeElement(body, index);
    if (!element.has_value()) {
      break;
    }
    if (!element->ok) {
      result.message = "in string literal " + quoted(text) + ": " + element->message;
      return result;
    }
    index = element->next;
    // `\` + a newline contributes nothing at all: the line continues, and the
    // bytes of the literal are the bytes on both sides of it.
    if (element->isContinuation) {
      continue;
    }
    if (element->isCodePoint) {
      // One code point, one UTF-8 encoding, on every target: no locale and no
      // ABI is consulted (`literals.md`, decision 13).
      appendUtf8(result.bytes, element->value);
      continue;
    }
    if (element->value > 0xFFU) {
      result.message = "in string literal " + quoted(text) +
                       ": this escape is wider than one byte, and a `str` is bytes; write the "
                       "code point as `\\u{...}`";
      return result;
    }
    result.bytes.push_back(static_cast<std::uint8_t>(element->value));
  }
  result.ok = true;
  return result;
}

} // namespace minc::support
