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
#include <utility>
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

  // A stream over tokens that did not all come from one file: the output of the
  // preprocessor, whose tokens were written in headers, in macro bodies, or
  // nowhere at all (a pasted or stringified spelling).
  //
  // `text` is the preprocessed text the tokens tile, `unit` names the
  // translation unit it belongs to, and `origins` gives each token the source
  // span it was *written* at -- so a caret still points at the header the bytes
  // came from while the tree is built over the preprocessed text. `origins` must
  // be either empty (every token came from `unit`) or exactly `tokens.size()`
  // long.
  //
  // Not `lex`: the tokens are already tokenized, and re-lexing the preprocessed
  // text would throw away the provenance that makes the diagnostics right.
  [[nodiscard]] static TokenStream fromPreprocessed(support::FileId unit, std::string_view text,
                                                    std::vector<Token> tokens,
                                                    std::vector<support::Span> origins);

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
  [[nodiscard]] const Token& operator[](std::size_t index) const {
    return tokens_[index];
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

  // Span covering the token's lexeme, in this stream's file. Use this for a
  // stream that really is one file: `lex_report` walks one and wants the stream
  // to be right rather than to be started.
  [[nodiscard]] support::Span spanOf(const Token& token) const;

  // Span of the token at `index`: the origin it was written at when the stream
  // carries origins, the stream's own file otherwise. This is what the parser
  // asks for, because it is the one that has to point a caret at the source.
  [[nodiscard]] support::Span spanOfAt(std::size_t index) const;

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
  // Empty for a lexed stream (one file for every token); see `fromPreprocessed`.
  std::vector<support::Span> origins_;
  std::size_t triviaCount_ = 0;
  bool lossless_ = true;
};

} // namespace minc::lex
