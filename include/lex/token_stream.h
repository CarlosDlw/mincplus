// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The token buffer for one file: every byte of the source, tiled by tokens.
//
// `lexOne` answers "what is the next token?"; this class answers "what is the
// whole file?". It is the layer that makes the *lossless* guarantee checkable,
// because it is the only place that can see the sequence as a whole:
//
//   concatenating the spelling of every token reproduces the file byte for
//   byte, the first token starts at 0, the last is EndOfFile at size(), and
//   each token starts exactly where the previous one ended.
//
// Trivia is kept. Dropping whitespace and comments at lex time cannot be
// undone later, and it is exactly what a formatter, a semantic highlighter, and
// doc-comment hover need. Consumers that only want code filter on
// `Token::isTrivia()` or walk `significantIndices()`.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "lex/token.h"
#include "support/span/file_id.h"
#include "support/span/span.h"

namespace minc::lex {

class TokenStream {
public:
  TokenStream() = default;

  // Lexes `text` in full. `text` is *not* copied and must outlive the stream;
  // in the normal flow it is `SourceFile::text`, owned by the Session.
  [[nodiscard]] static TokenStream lex(support::FileId file, std::string_view text);

  [[nodiscard]] support::FileId file() const {
    return file_;
  }
  [[nodiscard]] std::string_view text() const {
    return text_;
  }

  [[nodiscard]] const std::vector<Token>& tokens() const {
    return tokens_;
  }
  [[nodiscard]] std::size_t size() const {
    return tokens_.size();
  }
  [[nodiscard]] bool empty() const {
    return tokens_.empty();
  }
  [[nodiscard]] const Token& operator[](std::size_t index) const {
    return tokens_[index];
  }
  [[nodiscard]] const Token& at(std::size_t index) const {
    return tokens_.at(index);
  }
  [[nodiscard]] const Token& back() const {
    return tokens_.back();
  }

  [[nodiscard]] std::size_t triviaCount() const {
    return triviaCount_;
  }
  // EndOfFile counts as significant: it is the parser's stop condition, not
  // something to skip.
  [[nodiscard]] std::size_t significantCount() const {
    return significant_.size();
  }

  // Indices into tokens() of the non-trivia tokens, in source order. The
  // parser walks this instead of re-testing every token on every step.
  [[nodiscard]] std::span<const std::uint32_t> significantIndices() const {
    return significant_;
  }
  [[nodiscard]] const Token& significantAt(std::size_t index) const {
    return tokens_[significant_[index]];
  }

  // Span covering the token's lexeme, in this stream's file.
  [[nodiscard]] support::Span spanOf(const Token& token) const;

  // True when the tokens tile the input exactly. Always true for a stream
  // built by lex(); exposed so tests and any future incremental splice can
  // assert it instead of trusting it.
  [[nodiscard]] bool lossless() const {
    return lossless_;
  }

private:
  support::FileId file_ = support::kInvalidFile;
  std::string_view text_;
  std::vector<Token> tokens_;
  std::vector<std::uint32_t> significant_;
  std::size_t triviaCount_ = 0;
  bool lossless_ = true;
};

} // namespace minc::lex
