// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "lex/token_stream.h"

#include "lex/lexer.h"

namespace minc::lex {

TokenStream TokenStream::lex(support::FileId file, std::string_view text) {
  TokenStream stream;
  stream.file_ = file;
  stream.text_ = text;

  // Rough sizing: one token per four bytes. Purely a hint, and intentionally
  // conservative for pathological input (a file of single-byte punctuators).
  stream.tokens_.reserve(text.size() / 4u + 1u);

  std::uint32_t offset = 0;
  bool contiguous = true;

  while (true) {
    const Token token = lexOne(text, offset);
    if (token.offset != offset) {
      contiguous = false;
    }
    if (token.isTrivia()) {
      ++stream.triviaCount_;
    } else {
      stream.significant_.push_back(static_cast<std::uint32_t>(stream.tokens_.size()));
    }
    stream.tokens_.push_back(token);

    if (token.kind == TokenKind::EndOfFile) {
      break;
    }
    if (token.end() <= offset) {
      // A scanner failed to make progress. That cannot happen for a correct
      // lexer, but looping forever on malformed input is the one failure mode
      // worth an explicit guard: stop and let lossless() report the break.
      contiguous = false;
      break;
    }
    offset = token.end();
  }

  const Token& last = stream.tokens_.back();
  const bool endsAtEndOfFile =
      last.kind == TokenKind::EndOfFile && last.offset == static_cast<std::uint32_t>(text.size());
  stream.lossless_ = contiguous && endsAtEndOfFile;
  return stream;
}

TokenStream TokenStream::fromPreprocessed(support::FileId unit, std::string_view text,
                                          std::vector<Token> tokens,
                                          std::vector<support::Span> origins) {
  TokenStream stream;
  stream.file_ = unit;
  stream.text_ = text;
  stream.tokens_ = std::move(tokens);
  stream.origins_ = std::move(origins);
  if (!stream.origins_.empty() && stream.origins_.size() != stream.tokens_.size()) {
    // Not tolerated: a short origins table would silently point the carets of
    // the tail at the wrong place, which is worse than not pointing at all.
    stream.origins_.clear();
  }

  // The same rules `lex` applies, computed the same way, so a consumer cannot
  // tell a preprocessed stream from a lexed one except by its spans. There is no
  // contiguity check: these tokens were produced by tiling the text, so
  // `lossless` is a property of the producer, and a stream that is not a tiling
  // would mean the producer is wrong -- which its own tests assert.
  stream.significant_.reserve(stream.tokens_.size());
  std::uint32_t covered = 0;
  bool contiguous = true;
  for (std::size_t i = 0; i < stream.tokens_.size(); ++i) {
    const Token& token = stream.tokens_[i];
    if (token.offset != covered) {
      contiguous = false;
    }
    covered = token.end();
    if (token.isTrivia()) {
      ++stream.triviaCount_;
    } else {
      stream.significant_.push_back(static_cast<std::uint32_t>(i));
    }
  }
  stream.lossless_ = contiguous && covered == static_cast<std::uint32_t>(text.size());
  return stream;
}

support::Span TokenStream::spanOf(const Token& token) const {
  return support::Span{file_, token.offset, token.end()};
}

support::Span TokenStream::spanOfAt(std::size_t index) const {
  if (index < origins_.size()) {
    return origins_[index];
  }
  if (index < tokens_.size()) {
    return spanOf(tokens_[index]);
  }
  return support::Span{file_, static_cast<std::uint32_t>(text_.size()),
                       static_cast<std::uint32_t>(text_.size())};
}

} // namespace minc::lex
