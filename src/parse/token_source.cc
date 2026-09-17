// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "parse/token_source.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "lex/token_stream.h"

namespace minc::parse {
namespace {

// The one TokenSource that exists today: a file's significant tokens.
//
// Trivia is skipped here and nowhere else, which is what makes the parser
// trivia-blind and the tree lossless at the same time: the builder reads the
// same stream and is the component that attaches the trivia the parser never
// saw.
class TokenStreamSource final : public TokenSource {
public:
  explicit TokenStreamSource(const lex::TokenStream& stream)
      : stream_(&stream), significant_(stream.significantIndices()) {}

  [[nodiscard]] lex::TokenKind current() const override {
    return index_ < significant_.size() ? kindAt(index_) : lex::TokenKind::EndOfFile;
  }

  [[nodiscard]] lex::TokenKind nth(std::uint32_t n) const override {
    // Clamped: looking past the end yields the end-of-file token rather than
    // reading out of bounds, so the grammar can look ahead without bounds
    // checks of its own.
    const std::size_t index = index_ + static_cast<std::size_t>(n);
    return index < significant_.size() ? kindAt(index) : lex::TokenKind::EndOfFile;
  }

  [[nodiscard]] bool atEnd() const override {
    return current() == lex::TokenKind::EndOfFile;
  }

  [[nodiscard]] std::uint32_t position() const override {
    return static_cast<std::uint32_t>(index_);
  }

  void bump() override {
    // Stops at the end rather than running off it: once the end-of-file token
    // has been consumed, `index_` equals the token count and every later call
    // is a no-op.
    if (index_ < significant_.size()) {
      ++index_;
    }
  }

  [[nodiscard]] std::string_view textOf(std::uint32_t n) const override {
    const std::size_t index = index_ + static_cast<std::size_t>(n);
    if (index >= significant_.size()) {
      return {};
    }
    // From the token's own offset and length, and not from a span: a token's
    // span is the *written* one (a macro body, a header), which is where a caret
    // belongs and is not necessarily where these bytes are in this text.
    const lex::Token& token = stream_->significantAt(index);
    return stream_->text().substr(token.offset, token.length);
  }

  [[nodiscard]] support::Span spanOfCurrent() const override {
    if (index_ < significant_.size()) {
      // `spanOfAt` rather than `spanOf`: a preprocessed stream carries the span
      // each token was *written* at, which can be a header or a macro body, and
      // a caret must point there rather than at the preprocessed text.
      return stream_->spanOfAt(significant_[index_]);
    }
    // At the end, an empty span at the very end of the file, so a caret for
    // "expected ';'" sits where the text stops.
    const std::uint32_t end = static_cast<std::uint32_t>(stream_->text().size());
    return support::Span(stream_->file(), end, end);
  }

  [[nodiscard]] support::Span spanOf(std::uint32_t n) const override {
    const std::size_t index = index_ + static_cast<std::size_t>(n);
    if (index < significant_.size()) {
      return stream_->spanOfAt(significant_[index]);
    }
    const std::uint32_t end = static_cast<std::uint32_t>(stream_->text().size());
    return support::Span(stream_->file(), end, end);
  }

private:
  [[nodiscard]] lex::TokenKind kindAt(std::size_t index) const {
    return stream_->significantAt(index).kind;
  }

  const lex::TokenStream* stream_;
  std::span<const std::uint32_t> significant_;
  std::size_t index_ = 0;
};

} // namespace

// Factory so the concrete source stays out of the header.
[[nodiscard]] std::unique_ptr<TokenSource> makeTokenStreamSource(const lex::TokenStream& stream) {
  return std::make_unique<TokenStreamSource>(stream);
}

} // namespace minc::parse
