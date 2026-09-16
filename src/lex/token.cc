// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "lex/token.h"

#include <array>
#include <cstddef>

namespace minc::lex {
namespace {

// The single list of flags. `code` is what a user greps for, `name` is what the
// token dump prints, `message` is what a human reads next to the caret, and the
// order here is the order diagnostics are reported in.
constexpr std::array<FlagInfo, 11> kFlagInfos{{
    {TokenFlag::UnterminatedString, "lex-unterminated-string", "unterminated-string",
     "unterminated string literal"},
    {TokenFlag::UnterminatedChar, "lex-unterminated-char", "unterminated-char",
     "unterminated character literal"},
    {TokenFlag::UnterminatedBlockComment, "lex-unterminated-comment", "unterminated-comment",
     "unterminated block comment"},
    {TokenFlag::UnknownEscape, "lex-unknown-escape", "unknown-escape", "unknown escape sequence"},
    {TokenFlag::InvalidEscapeValue, "lex-escape-out-of-range", "escape-out-of-range",
     "escape is not a valid Unicode scalar value"},
    {TokenFlag::EmptyCharLiteral, "lex-empty-char", "empty-char", "empty character literal"},
    {TokenFlag::MissingDigits, "lex-missing-digits", "missing-digits",
     "expected at least one digit after this prefix"},
    {TokenFlag::MisplacedSeparator, "lex-misplaced-separator", "misplaced-separator",
     "a digit separator belongs between two digits"},
    {TokenFlag::EscapeDigits, "lex-escape-digits", "escape-digits",
     "an escape needs digits: `\\x{41}` takes any number of them between braces, and "
     "`\\u`/`\\U` take exactly four or eight without braces"},
    // Only a *string*'s escape reaches this row: a character literal's width is a
    // type rule and the checker owns its sentence, because it is the stage that
    // holds the type and can name the fix (`literals.md`, decision 23).
    {TokenFlag::EscapeTooWide, "lex-escape-too-wide", "escape-too-wide",
     "this escape is wider than one byte: a `str` is bytes, so write the code point as "
     "`\\u{...}`"},
    {TokenFlag::NamedEscape, "lex-named-escape", "named-escape",
     "`\\N{...}` names a Unicode character by name, which needs a name table this compiler does "
     "not carry; write the code point as `\\u{...}`"},
}};

// Derived, not listed again: a flag added to the table above is picked up by
// every consumer of allTokenFlags() without a second edit that could be
// forgotten.
[[nodiscard]] constexpr auto flagsFromTable() {
  std::array<TokenFlag, kFlagInfos.size()> flags{};
  for (std::size_t i = 0; i < kFlagInfos.size(); ++i) {
    flags[i] = kFlagInfos[i].flag;
  }
  return flags;
}

constexpr auto kAllFlags = flagsFromTable();

} // namespace

std::span<const TokenFlag> allTokenFlags() {
  return kAllFlags;
}

std::span<const FlagInfo> flagInfos() {
  return kFlagInfos;
}

const char* toString(TokenFlag flag) {
  for (const FlagInfo& info : kFlagInfos) {
    if (info.flag == flag) {
      return info.name;
    }
  }
  return "none";
}

} // namespace minc::lex
