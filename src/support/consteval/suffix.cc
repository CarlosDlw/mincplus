// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "support/consteval/suffix.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace minc::support {
namespace {

// One exact spelling. The language's own type names (`i32`, `f64`, `usize`) are
// matched letter for letter, and the C letters are matched by the rows below or by
// `classifyLetters`, because there are twenty combinations and listing twenty rows
// is a table that drifts from the rule it encodes.
//
// Case is *not* a spelling of its own for a type name: `I32` is an identifier.
// It is for the letters C gives two cases to -- `u`/`U`, `l`/`L`, `f`/`F` -- which
// is why those have a row each instead of a rule.
struct Row {
  std::string_view spelling;
  SuffixType type;
  bool makesFloat;
};

// The table, in the order the header's comment gives it: the language's own
// names, then the float names, then the C spellings that are one letter.
constexpr Row kRows[] = {
    {"i8", SuffixType::I8, false},
    {"i16", SuffixType::I16, false},
    {"i32", SuffixType::I32, false},
    {"i64", SuffixType::I64, false},
    {"i128", SuffixType::I128, false},
    {"isize", SuffixType::Isize, false},
    {"u8", SuffixType::U8, false},
    {"u16", SuffixType::U16, false},
    {"u32", SuffixType::U32, false},
    {"u64", SuffixType::U64, false},
    {"u128", SuffixType::U128, false},
    {"usize", SuffixType::Usize, false},
    // `f` is C's float suffix, and C accepts it in both cases -- so `12F` is the
    // same spelling as `12f` and is *claimed* as one token rather than left to the
    // lexer as an identifier that happens to follow a number.
    {"f", SuffixType::F32, true},
    {"F", SuffixType::F32, true},
    {"f32", SuffixType::F32, true},
    {"f64", SuffixType::F64, true},
    {"f80", SuffixType::F80, true},
    {"f128", SuffixType::F128, true},
    {"u", SuffixType::CUnsignedInt, false},
    {"U", SuffixType::CUnsignedInt, false},
    // `long long` is 64 bits on every triple this compiler states, so it is not a
    // target question -- which is why only `long` and `long double` are.
    {"ll", SuffixType::I64, false},
    {"LL", SuffixType::I64, false},
    {"lL", SuffixType::I64, false},
    {"Ll", SuffixType::I64, false},
    // `l`/`L` are resolved by the literal's own class: `long` on an integer,
    // `long double` on a float.
    {"l", SuffixType::CLong, false},
    {"L", SuffixType::CLong, false},
};

// The C combinations that are `unsigned long long`: one `u`, two `l`s.
[[nodiscard]] constexpr SuffixType lettersType(unsigned uCount, unsigned lCount) {
  if (uCount == 1 && lCount == 2) {
    return SuffixType::U64;
  }
  if (uCount == 0 && lCount == 2) {
    return SuffixType::I64;
  }
  if (uCount == 1 && lCount == 1) {
    return SuffixType::CUnsignedLong;
  }
  if (uCount == 0 && lCount == 1) {
    return SuffixType::CLong;
  }
  if (uCount == 1 && lCount == 0) {
    return SuffixType::CUnsignedInt;
  }
  return SuffixType::None;
}

// `l`, `L`, `u`, `U` in any order and case, which is C's rule (`ul`, `lu`, `uLL`,
// `LLu`, ...). Returns `None` when the run is not made only of those letters, or
// when the counts name nothing (`uu`, `lll`, `ulul`).
[[nodiscard]] constexpr SuffixType classifyLetters(std::string_view text) {
  unsigned uCount = 0;
  unsigned lCount = 0;
  for (const char c : text) {
    if (c == 'u' || c == 'U') {
      ++uCount;
    } else if (c == 'l' || c == 'L') {
      ++lCount;
    } else {
      return SuffixType::None;
    }
  }
  if (uCount + lCount != text.size()) {
    return SuffixType::None;
  }
  return lettersType(uCount, lCount);
}

// C23's bit-precise suffix. Claimed -- so `10wb` is one token -- and refused with
// the type to write instead (`N2775`).
[[nodiscard]] bool isBitPreciseSuffix(std::string_view text) {
  if (text == "wb" || text == "WB" || text == "Wb" || text == "wB") {
    return true;
  }
  if (text.size() == 3 && (text[0] == 'u' || text[0] == 'U')) {
    const std::string_view tail = text.substr(1);
    return tail == "wb" || tail == "WB" || tail == "Wb" || tail == "wB";
  }
  return false;
}

[[nodiscard]] bool isIdentifierContinue(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

} // namespace

std::string_view toString(SuffixType type) {
  switch (type) {
  case SuffixType::None:
    return "none";
  case SuffixType::I8:
    return "i8";
  case SuffixType::I16:
    return "i16";
  case SuffixType::I32:
    return "i32";
  case SuffixType::I64:
    return "i64";
  case SuffixType::I128:
    return "i128";
  case SuffixType::Isize:
    return "isize";
  case SuffixType::U8:
    return "u8";
  case SuffixType::U16:
    return "u16";
  case SuffixType::U32:
    return "u32";
  case SuffixType::U64:
    return "u64";
  case SuffixType::U128:
    return "u128";
  case SuffixType::Usize:
    return "usize";
  case SuffixType::F32:
    return "f32";
  case SuffixType::F64:
    return "f64";
  case SuffixType::F80:
    return "f80";
  case SuffixType::F128:
    return "f128";
  case SuffixType::CUnsignedInt:
    return "c-unsigned-int";
  case SuffixType::CLong:
    return "c-long";
  case SuffixType::CUnsignedLong:
    return "c-unsigned-long";
  case SuffixType::CLongDouble:
    return "c-long-double";
  }
  return "none";
}

LiteralSuffix classifySuffix(std::string_view text, bool literalIsFloat) {
  LiteralSuffix result;
  if (text.empty()) {
    return result;
  }

  // The one spelling the language knows and refuses. `wb` is an integer suffix by
  // construction, so a float literal carrying it is refused by the float rule
  // below rather than by this one -- and either way the sentence names the fix.
  if (!literalIsFloat && isBitPreciseSuffix(text)) {
    result.status = SuffixStatus::Refused;
    result.message = "`" + std::string(text) +
                     "` names a C23 bit-precise integer, and this language has no such type: "
                     "write `i64` (or `i128`)";
    return result;
  }

  for (const Row& row : kRows) {
    if (row.spelling != text) {
      continue;
    }
    // `l`/`L` mean two different types depending on what the number looks like,
    // and a float literal has no `long`: it has `long double`.
    if (row.type == SuffixType::CLong) {
      if (!literalIsFloat) {
        result.status = SuffixStatus::Typed;
        result.type = SuffixType::CLong;
        return result;
      }
      result.status = SuffixStatus::Typed;
      result.type = SuffixType::CLongDouble;
      return result;
    }
    if (!literalIsFloat) {
      result.status = SuffixStatus::Typed;
      result.type = row.type;
      result.makesFloat = row.makesFloat;
      return result;
    }
    // A float literal takes only the float suffixes: `f`, `f32`, `f64`, `f80`,
    // `f128`, and `l`/`L` (handled above).
    if (row.makesFloat) {
      result.status = SuffixStatus::Typed;
      result.type = row.type;
      return result;
    }
    result.status = SuffixStatus::Refused;
    result.message = "a float literal cannot have the integer suffix `" + std::string(text) +
                     "`: write the conversion -- `(" + std::string(text) + ")1.5`, or `1.5 as " +
                     std::string(text) + "`";
    return result;
  }

  // A float literal takes no letter combination either: every one of them names an
  // integer.
  const SuffixType letters = classifyLetters(text);
  if (letters != SuffixType::None && !literalIsFloat) {
    result.status = SuffixStatus::Typed;
    result.type = letters;
    return result;
  }
  if (letters != SuffixType::None) {
    result.status = SuffixStatus::Refused;
    result.message = "a float literal cannot have the integer suffix `" + std::string(text) +
                     "`: write the conversion -- `(" + std::string(text) + ")1.5`, or `1.5 as " +
                     std::string(text) + "`";
    return result;
  }

  // Not a suffix at all: the run is the next token.
  return result;
}

std::size_t suffixLengthAt(std::string_view text, std::size_t at, bool literalIsFloat) {
  std::size_t end = at;
  while (end < text.size() && isIdentifierContinue(text[end])) {
    ++end;
  }
  if (end == at) {
    return 0;
  }
  const LiteralSuffix classified = classifySuffix(text.substr(at, end - at), literalIsFloat);
  return classified.present() ? end - at : 0;
}

std::string_view numericPartOfFloat(std::string_view text) {
  const auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
  const auto isHex = [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  };
  const auto isBinary = [](char c) { return c == '0' || c == '1'; };
  const auto isOctal = [](char c) { return c >= '0' && c <= '7'; };

  unsigned base = 10;
  std::size_t i = 0;
  if (text.size() >= 2 && text[0] == '0') {
    switch (text[1]) {
    case 'x':
    case 'X':
      base = 16;
      i = 2;
      break;
    case 'b':
    case 'B':
      base = 2;
      i = 2;
      break;
    case 'o':
    case 'O':
      base = 8;
      i = 2;
      break;
    default:
      break;
    }
  }
  const auto skipDigits = [&](std::size_t at) {
    std::size_t j = at;
    while (j < text.size()) {
      const char c = text[j];
      const bool digit = base == 16  ? isHex(c)
                         : base == 2 ? isBinary(c)
                         : base == 8 ? isOctal(c)
                                     : isDigit(c);
      if (!digit) {
        break;
      }
      ++j;
    }
    return j;
  };

  i = skipDigits(i);
  if (i < text.size() && text[i] == '.') {
    i = skipDigits(i + 1);
  }
  // The exponent marker is the base's: `e`/`E` for decimal, `p`/`P` for hex. A
  // marker with no digits after it is the *scanner's* failure (`MissingDigits`),
  // and the honest reading of the spelling is that the marker is not one.
  const char marker = base == 16 ? 'p' : 'e';
  const bool hasExponent = base == 16 || base == 10;
  if (hasExponent && i < text.size() && (text[i] == marker || text[i] == marker - 32)) {
    std::size_t j = i + 1;
    if (j < text.size() && (text[j] == '+' || text[j] == '-')) {
      ++j;
    }
    const std::size_t digitsEnd = skipDigits(j);
    if (digitsEnd != j) {
      i = digitsEnd;
    }
  }
  return text.substr(0, i);
}

std::span<const SuffixSpelling> allSuffixSpellings() {
  // The exact rows, plus one spelling per C combination -- the walk is what keeps
  // "a suffix added to the language is a row in this file" true, and the case
  // variants are covered by their own test rather than by twenty extra rows.
  static constexpr SuffixSpelling kSpellings[] = {
      {"i8", false},    {"i16", false},   {"i32", false},  {"i64", false}, {"i128", false},
      {"isize", false}, {"u8", false},    {"u16", false},  {"u32", false}, {"u64", false},
      {"u128", false},  {"usize", false}, {"f", false},    {"F", false},   {"f32", false},
      {"f64", false},   {"f80", false},   {"f128", false}, {"u", false},   {"U", false},
      {"l", false},     {"L", false},     {"ll", false},   {"LL", false},  {"lL", false},
      {"Ll", false},    {"ul", false},    {"lu", false},   {"ull", false}, {"llu", false},
      {"f", true},      {"F", true},      {"f32", true},   {"f64", true},  {"f80", true},
      {"f128", true},   {"l", true},      {"L", true},
  };
  return kSpellings;
}

} // namespace minc::support
