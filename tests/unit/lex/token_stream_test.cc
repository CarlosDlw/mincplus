// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "lex/lexer.h"
#include "lex/token_kind.h"
#include "lex/token_stream.h"
#include "support/line/line_col.h"
#include "support/line/line_table.h"

namespace minc::lex {
namespace {

// Every offset in the stream is contiguous and the whole input is covered:
// that is the invariant everything downstream depends on, so assert it
// wherever a stream is built.
void expectTilesExactly(const TokenStream& stream, std::string_view text) {
  std::uint32_t offset = 0;
  for (const Token& token : stream.tokens()) {
    ASSERT_EQ(token.offset, offset) << "gap or overlap at " << offset;
    offset = token.end();
  }
  ASSERT_FALSE(stream.tokens().empty());
  EXPECT_EQ(stream.tokens().back().kind, TokenKind::EndOfFile);
  EXPECT_EQ(offset, text.size());
  EXPECT_TRUE(stream.lossless());
}

TEST(TokenStreamTest, EmptyInput) {
  const TokenStream stream = TokenStream::lex(0, "");
  ASSERT_EQ(stream.size(), 1u);
  EXPECT_EQ(stream.back().kind, TokenKind::EndOfFile);
  EXPECT_EQ(stream.triviaCount(), 0u);
  EXPECT_EQ(stream.significantCount(), 1u); // EndOfFile is significant
  expectTilesExactly(stream, "");
}

TEST(TokenStreamTest, WholeTextIsCoveredByTokens) {
  const std::string_view text = "// c\nlet x = \"a\\\"b\"; /* b */ 0x1F\n";
  const TokenStream stream = TokenStream::lex(0, text);
  expectTilesExactly(stream, text);

  // Concatenating the lexemes reproduces the input byte for byte.
  std::string rebuilt;
  for (const Token& token : stream.tokens()) {
    rebuilt.append(stream.text().substr(token.offset, token.length));
  }
  EXPECT_EQ(rebuilt, text);
}

TEST(TokenStreamTest, CountsPartitionTheStream) {
  const std::string_view text = "let x = 1; // y\n";
  const TokenStream stream = TokenStream::lex(0, text);
  EXPECT_EQ(stream.triviaCount() + stream.significantCount(), stream.size());
}

TEST(TokenStreamTest, SignificantIndicesSkipTriviaInOrder) {
  const std::string_view text = "  let  x = 1;  \n";
  const TokenStream stream = TokenStream::lex(0, text);

  ASSERT_GE(stream.significantCount(), 1u);
  std::uint32_t previous = 0;
  bool first = true;
  for (const std::uint32_t index : stream.significantIndices()) {
    const Token& token = stream.tokens()[index];
    EXPECT_FALSE(token.isTrivia());
    if (!first) {
      EXPECT_GT(index, previous);
    }
    previous = index;
    first = false;
  }
  EXPECT_EQ(stream.significantAt(0).kind, TokenKind::KwLet);
  EXPECT_EQ(stream.significantAt(stream.significantCount() - 1).kind, TokenKind::EndOfFile);
}

TEST(TokenStreamTest, SpanOfCoversTheLexeme) {
  const std::string_view text = "let x;";
  const TokenStream stream = TokenStream::lex(7, text);
  const Token& token = stream.significantAt(1); // `x`
  const support::Span span = stream.spanOf(token);
  EXPECT_EQ(span.file, 7u);
  EXPECT_EQ(span.begin, 4u);
  EXPECT_EQ(span.end, 5u);
  EXPECT_TRUE(span.valid());
  EXPECT_EQ(text.substr(span.begin, span.size()), "x");
}

TEST(TokenStreamTest, EndOfFileSpanIsEmptyAtTheEnd) {
  const std::string_view text = "ab";
  const TokenStream stream = TokenStream::lex(0, text);
  const support::Span span = stream.spanOf(stream.back());
  EXPECT_EQ(span.begin, 2u);
  EXPECT_EQ(span.end, 2u);
  EXPECT_TRUE(span.empty());
}

// Token offsets are the stream's business; the line table turns them into
// positions. Checking that a multi-line block comment moves the *following*
// token to a later line is what keeps the two from drifting.
TEST(TokenStreamTest, OffsetsResolveThroughTheLineTable) {
  const std::string_view text = "let a = 1;\r\nlet b = 2;\n/* x\ny */ let c = 3;\n";
  const TokenStream stream = TokenStream::lex(0, text);
  const support::LineTable lines(text);
  const auto size = static_cast<std::uint32_t>(text.size());

  // The two first statements start their line; the third one shares line 4 with
  // the tail of the block comment that spans a newline.
  const std::vector<std::uint32_t> expectedLines{1, 2, 4};
  std::vector<std::uint32_t> actualLines;
  const Token* lastLet = nullptr;
  for (const Token& token : stream.tokens()) {
    if (token.kind != TokenKind::KwLet) {
      continue;
    }
    actualLines.push_back(lines.lookup(token.offset, size).line);
    lastLet = &token;
  }
  EXPECT_EQ(actualLines, expectedLines);

  ASSERT_NE(lastLet, nullptr);
  const support::LineCol pos = lines.lookup(lastLet->offset, size);
  EXPECT_EQ(pos.line, 4u);
  EXPECT_EQ(pos.col, 6u); // `y */ let`
}

// Deterministic byte soup. This is the property a fuzzer would check, kept in
// the suite so it runs on every platform with no extra tooling: whatever the
// bytes are, lexing terminates, tiles the input, and never loses a byte.
TEST(TokenStreamTest, LosslessOnArbitraryBytes) {
  constexpr std::string_view kAlphabet =
      "ab1 \t\r\n\"'\\/*-+<>=&|^!~.,;:(){}[]#@_0x\x01\x7f\xC3\xA9\xA9";

  std::uint32_t state = 0x12345678U;
  const auto next = [&state]() {
    state = state * 1664525U + 1013904223U;
    return state >> 24U;
  };

  for (int iteration = 0; iteration < 2000; ++iteration) {
    std::string text;
    const std::size_t length = next() % 64U;
    text.reserve(length);
    for (std::size_t i = 0; i < length; ++i) {
      text.push_back(kAlphabet[next() % kAlphabet.size()]);
    }

    const TokenStream stream = TokenStream::lex(0, text);
    ASSERT_TRUE(stream.lossless()) << "iteration " << iteration;
    expectTilesExactly(stream, text);
  }
}

TEST(TokenStreamTest, LexingIsPureAndRepeatable) {
  const std::string_view text = "let x = 1; // c\n";
  const TokenStream first = TokenStream::lex(0, text);
  const TokenStream second = TokenStream::lex(0, text);
  ASSERT_EQ(first.size(), second.size());
  for (std::size_t i = 0; i < first.size(); ++i) {
    EXPECT_EQ(first[i].kind, second[i].kind) << i;
    EXPECT_EQ(first[i].offset, second[i].offset) << i;
    EXPECT_EQ(first[i].length, second[i].length) << i;
    EXPECT_EQ(first[i].flags, second[i].flags) << i;
  }
}

// The lexer is a pure function of (text, offset), which is what lets the same
// suffix be re-lexed for an incremental editor update. Starting mid-file must
// therefore produce exactly the tokens the whole-file pass produced.
TEST(TokenStreamTest, AnyTokenBoundaryIsAValidRestartPoint) {
  const std::string_view text = "let s = \"a\\\"b\"; /* c\nd */ 0x1F // e\nfn";
  const TokenStream whole = TokenStream::lex(0, text);

  for (const Token& token : whole.tokens()) {
    if (token.kind == TokenKind::EndOfFile) {
      continue;
    }
    const Token restarted = lexOne(text, token.offset);
    EXPECT_EQ(restarted.kind, token.kind) << token.offset;
    EXPECT_EQ(restarted.length, token.length) << token.offset;
    EXPECT_EQ(restarted.flags, token.flags) << token.offset;
  }
}

} // namespace
} // namespace minc::lex
