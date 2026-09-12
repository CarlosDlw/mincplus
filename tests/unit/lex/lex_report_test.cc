// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include <cstddef>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "lex/lex_report.h"
#include "lex/token.h"
#include "lex/token_stream.h"
#include "support/diag/diag_bag.h"
#include "support/diag/diagnostic.h"
#include "support/span/span.h"

namespace minc::lex {
namespace {

std::size_t report(std::string_view text, support::DiagBag& bag, support::FileId file = 0) {
  const TokenStream stream = TokenStream::lex(file, text);
  return reportLexErrors(stream, bag);
}

TEST(LexReportTest, CleanInputReportsNothing) {
  support::DiagBag bag;
  EXPECT_EQ(report("fn i32 main() {\n  let x = 0x1F;\n  return x;\n}\n", bag), 0u);
  EXPECT_TRUE(bag.empty());
  EXPECT_FALSE(bag.hasErrors());
}

// The whole reason flags exist on the token: one pass finds every problem
// instead of stopping at the first.
TEST(LexReportTest, ReportsEveryProblemInOnePass) {
  support::DiagBag bag;
  const std::size_t count = report("let s = \"abc\nlet c = '';\nlet n = 0x;\n", bag);
  EXPECT_EQ(count, 3u);
  EXPECT_EQ(bag.errorCount(), 3u);
  EXPECT_TRUE(bag.hasErrors());
}

// One token can be wrong in two structural ways at once, and both must surface.
TEST(LexReportTest, EveryFlagOnATokenIsReported) {
  support::DiagBag bag;
  report("\"\\x", bag);
  ASSERT_EQ(bag.size(), 2u);
  // Reported in table order, so the result is deterministic.
  EXPECT_EQ(bag.all()[0].code, "lex-unterminated-string");
  EXPECT_EQ(bag.all()[1].code, "lex-missing-digits");
}

TEST(LexReportTest, FlagInfosCoverEveryFlagExactlyOnce) {
  EXPECT_EQ(flagInfos().size(), allTokenFlags().size());

  for (const TokenFlag flag : allTokenFlags()) {
    EXPECT_STRNE(toString(flag), "none") << "flag " << static_cast<int>(flag);

    std::size_t rows = 0;
    for (const FlagInfo& info : flagInfos()) {
      if (info.flag == flag) {
        ++rows;
      }
    }
    EXPECT_EQ(rows, 1u) << "flag " << static_cast<int>(flag);
  }

  for (const FlagInfo& info : flagInfos()) {
    EXPECT_FALSE(std::string_view(info.code).empty());
    EXPECT_FALSE(std::string_view(info.message).empty());
    EXPECT_NE(std::string_view(info.name), "none");
    // Two flags sharing a name would make the dump ambiguous, and two sharing a
    // code would make grep useless.
    std::size_t sameName = 0;
    std::size_t sameCode = 0;
    for (const FlagInfo& other : flagInfos()) {
      if (std::string_view(other.name) == std::string_view(info.name)) {
        ++sameName;
      }
      if (std::string_view(other.code) == std::string_view(info.code)) {
        ++sameCode;
      }
    }
    EXPECT_EQ(sameName, 1u) << info.name;
    EXPECT_EQ(sameCode, 1u) << info.code;
  }
}

TEST(LexReportTest, MessageAndCodeComeFromTheSameRow) {
  support::DiagBag bag;
  report("0x", bag);
  ASSERT_EQ(bag.size(), 1u);
  EXPECT_EQ(bag.all()[0].code, "lex-missing-digits");
  EXPECT_EQ(bag.all()[0].message, "expected at least one digit after this prefix");
}

TEST(LexReportTest, SpanPointsAtTheOffendingToken) {
  support::DiagBag bag;
  report("let x = 0x;\n", bag, 5);
  ASSERT_EQ(bag.size(), 1u);
  const support::Span span = bag.all()[0].span;
  EXPECT_EQ(span.file, 5u);
  EXPECT_EQ(span.begin, 8u);
  EXPECT_EQ(span.end, 10u);
}

TEST(LexReportTest, PrintableInvalidCharacterIsQuoted) {
  support::DiagBag bag;
  report("let x = #;\n", bag);
  ASSERT_EQ(bag.size(), 1u);
  EXPECT_EQ(bag.all()[0].code, "lex-invalid-character");
  EXPECT_EQ(bag.all()[0].message, "unexpected character '#'");
  EXPECT_EQ(bag.all()[0].span.begin, 8u);
  EXPECT_EQ(bag.all()[0].span.end, 9u);
}

TEST(LexReportTest, QuoteAndBackslashInTheMessageStayReadable) {
  support::DiagBag bag;
  report("\\", bag);
  ASSERT_EQ(bag.size(), 1u);
  // The backslash is doubled so the quotes around it stay readable.
  EXPECT_EQ(bag.all()[0].message, R"(unexpected character '\\')");
  EXPECT_EQ(bag.all()[0].span.begin, 0u);
}

TEST(LexReportTest, ControlByteIsNamedInHex) {
  support::DiagBag bag;
  report("a\x01", bag);
  ASSERT_EQ(bag.size(), 1u);
  EXPECT_EQ(bag.all()[0].message, "unexpected byte 0x01");
}

TEST(LexReportTest, NonAsciiIsOneDiagnosticCoveringTheWholeCharacter) {
  support::DiagBag bag;
  report("a\xC3\xA9", bag);
  ASSERT_EQ(bag.size(), 1u);
  EXPECT_EQ(bag.all()[0].message,
            "unexpected non-ASCII character outside a comment or string literal");
  EXPECT_EQ(bag.all()[0].span.begin, 1u);
  EXPECT_EQ(bag.all()[0].span.end, 3u);
}

TEST(LexReportTest, NonAsciiInsideCommentsAndStringsIsFine) {
  support::DiagBag bag;
  EXPECT_EQ(report("// \xC3\xA9\nlet s = \"\xC3\xA9\";\n", bag), 0u);
  EXPECT_TRUE(bag.empty());
}

} // namespace
} // namespace minc::lex
