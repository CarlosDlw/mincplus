// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "pp/pp_token_source.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "lex/token_kind.h"
#include "pp/pp_token.h"
#include "support/span/span.h"

namespace minc::pp {
namespace {

// Trivia is skipped here and nowhere else in this file, which is the same rule
// the lexer's own source follows: the preprocessed stream carries whitespace so
// the tree builder can hold every byte, and the grammar must never see it. A
// `Token` from this source is always "the next *significant* token".
class PPStreamSource final : public parse::TokenSource {
public:
  explicit PPStreamSource(std::span<const PPToken> tokens) : tokens_(tokens) {}

  [[nodiscard]] lex::TokenKind current() const override {
    return kindAhead(0);
  }
  [[nodiscard]] lex::TokenKind nth(std::uint32_t n) const override {
    // Clamped rather than undefined: a lookahead past the end is
    // `EndOfFile`, which is what the parser's stop conditions already test for.
    return kindAhead(n);
  }
  [[nodiscard]] bool atEnd() const override {
    return current() == lex::TokenKind::EndOfFile;
  }
  void bump() override {
    skipTrivia();
    if (index_ < tokens_.size()) {
      ++index_;
    }
  }
  [[nodiscard]] support::Span spanOfCurrent() const override {
    const std::size_t index = significantIndex();
    return index < tokens_.size() ? tokens_[index].span() : support::Span{};
  }

private:
  // The index of the first significant token at or after `index_`.
  [[nodiscard]] std::size_t significantIndex() const {
    std::size_t index = index_;
    while (index < tokens_.size() && isPPTrivia(tokens_[index].kind)) {
      ++index;
    }
    return index;
  }
  // The kind of the `ahead`-th significant token. Walks rather than indexes,
  // because trivia makes significant tokens non-contiguous; a lookahead count of
  // one or two and short runs of whitespace make that the cheap way.
  [[nodiscard]] lex::TokenKind kindAhead(std::uint32_t ahead) const {
    std::size_t index = index_;
    while (index < tokens_.size()) {
      if (isPPTrivia(tokens_[index].kind)) {
        ++index;
        continue;
      }
      if (ahead == 0) {
        break;
      }
      --ahead;
      ++index;
    }
    return index < tokens_.size() ? tokens_[index].kind : lex::TokenKind::EndOfFile;
  }
  void skipTrivia() {
    while (index_ < tokens_.size() && isPPTrivia(tokens_[index_].kind)) {
      ++index_;
    }
  }

  std::span<const PPToken> tokens_;
  std::size_t index_ = 0;
};

} // namespace

std::unique_ptr<parse::TokenSource> makePPTokenSource(std::span<const PPToken> tokens) {
  return std::make_unique<PPStreamSource>(tokens);
}

} // namespace minc::pp
