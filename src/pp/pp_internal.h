// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Small helpers shared by the preprocessor's implementation files. Not part of
// the module's interface: nothing outside `src/pp` should include this.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "lex/token.h"
#include "pp/pp_token.h"
#include "pp/token_text.h"

namespace minc::pp::detail {

// The `#` that a directive can start with. It is an ordinary token: the lexer
// classifies the byte, and only *position* turns a `Hash` into a directive
// introducer. Nothing here inspects a spelling to find one.
[[nodiscard]] inline bool isHash(const lex::Token& token) {
  return token.is(lex::TokenKind::Hash);
}

[[nodiscard]] inline bool isHash(const PPToken& token) {
  return token.is(lex::TokenKind::Hash);
}

[[nodiscard]] inline bool isSignificant(const PPToken& token) {
  return !isPPTrivia(token.kind) && !token.isEndOfFile();
}

// Advances `index` past trivia.
inline void skipTrivia(const std::vector<PPToken>& line, std::size_t& index) {
  while (index < line.size() && isPPTrivia(line[index].kind)) {
    ++index;
  }
}

// The next significant token at or after `index`, or the end.
[[nodiscard]] inline const PPToken* nextSignificant(const std::vector<PPToken>& line,
                                                    std::size_t& index) {
  skipTrivia(line, index);
  return index < line.size() ? &line[index] : nullptr;
}

// The index just past the directive's name, so a handler can look at the
// directive's *operand* without re-walking the `#` and the name. Every handler
// starts here, which is why no handler contains the constant `2`.
[[nodiscard]] inline std::size_t afterDirectiveName(const std::vector<PPToken>& line) {
  std::size_t index = 1; // past the '#'
  if (nextSignificant(line, index) != nullptr) {
    ++index;
  }
  return index;
}

// A copy of `tokens` with trivia removed. Used everywhere a token sequence is
// about to be expanded or evaluated, because both are defined over
// preprocessing tokens, not over the spaces between them.
[[nodiscard]] inline std::vector<PPToken> withoutTrivia(std::span<const PPToken> tokens) {
  std::vector<PPToken> out;
  out.reserve(tokens.size());
  for (const PPToken& token : tokens) {
    if (isSignificant(token)) {
      out.push_back(token);
    }
  }
  return out;
}

// A copy of `tokens` with leading and trailing trivia removed but interior
// trivia kept. Stringification needs the interior: one space is inserted exactly
// where the original had whitespace.
[[nodiscard]] inline std::vector<PPToken> trimTrivia(std::span<const PPToken> tokens) {
  std::size_t begin = 0;
  std::size_t end = tokens.size();
  while (begin < end && isPPTrivia(tokens[begin].kind)) {
    ++begin;
  }
  while (end > begin && isPPTrivia(tokens[end - 1].kind)) {
    --end;
  }
  return std::vector<PPToken>(tokens.begin() + static_cast<std::ptrdiff_t>(begin),
                              tokens.begin() + static_cast<std::ptrdiff_t>(end));
}

// The source range covering every token, from the first to the last, in one
// file. Returns the first token's location when the rest cannot be ordered
// (which cannot happen for a directive line, and degrades safely if it does).
[[nodiscard]] inline SourceLoc cover(std::span<const PPToken> tokens) {
  if (tokens.empty()) {
    return SourceLoc{};
  }
  const SourceLoc& first = tokens.front().loc.spelling;
  const SourceLoc& last = tokens.back().loc.spelling;
  if (first.file != last.file || last.offset < first.offset) {
    return first;
  }
  return SourceLoc{first.file, first.offset, (last.offset + last.length) - first.offset};
}

} // namespace minc::pp::detail
