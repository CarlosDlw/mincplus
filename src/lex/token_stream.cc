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

support::Span TokenStream::spanOf(const Token& token) const {
  return support::Span{file_, token.offset, token.end()};
}

} // namespace minc::lex
