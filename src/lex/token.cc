// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "lex/token.h"

#include <array>
#include <cstddef>

namespace minc::lex {
namespace {

// One row per flag. `code` is what a user greps for, `name` is what the token
// dump prints, `message` is what a human reads next to the caret. A missing row
// is caught by the test that walks allTokenFlags().
constexpr std::array<FlagInfo, 6> kFlagInfos{{
    {TokenFlag::UnterminatedString, "lex-unterminated-string", "unterminated-string",
     "unterminated string literal"},
    {TokenFlag::UnterminatedChar, "lex-unterminated-char", "unterminated-char",
     "unterminated character literal"},
    {TokenFlag::UnterminatedBlockComment, "lex-unterminated-comment", "unterminated-comment",
     "unterminated block comment"},
    {TokenFlag::UnknownEscape, "lex-unknown-escape", "unknown-escape", "unknown escape sequence"},
    {TokenFlag::EmptyCharLiteral, "lex-empty-char", "empty-char", "empty character literal"},
    {TokenFlag::MissingDigits, "lex-missing-digits", "missing-digits",
     "expected at least one digit after this prefix"},
}};

constexpr std::array<TokenFlag, kFlagInfos.size()> kAllFlags{{
    TokenFlag::UnterminatedString,
    TokenFlag::UnterminatedChar,
    TokenFlag::UnterminatedBlockComment,
    TokenFlag::UnknownEscape,
    TokenFlag::EmptyCharLiteral,
    TokenFlag::MissingDigits,
}};

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
