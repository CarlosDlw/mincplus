// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include <cstddef>
#include <set>
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
  // Reported in table order, so the result is deterministic: `\x` with no digits
  // is `lex-escape-digits`, and the quote that never came is the other flag.
  EXPECT_EQ(bag.all()[0].code, "lex-unterminated-string");
  EXPECT_EQ(bag.all()[1].code, "lex-escape-digits");
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

// A flag no input can produce is a diagnostic the user will never see. This is
// the lexer's half of the same guarantee the parser's error-code test makes:
// every row of the table is reachable, and from the input that should produce
// it. `lex-invalid-character` is the exception -- it is not a flag, so it is
// checked here by name.
TEST(LexReportTest, EveryFlagIsReachableFromSomeInput) {
  struct Case {
    const char* label;
    std::string_view source;
    const char* code;
  };
  // One tiny input per code, so a failure is its own reproduction.
  const Case cases[] = {
      {"unterminated string", "\"abc\n", "lex-unterminated-string"},
      {"unterminated char", "'a\n", "lex-unterminated-char"},
      {"unterminated comment", "/* x", "lex-unterminated-comment"},
      {"unknown escape", "\"\\q\"", "lex-unknown-escape"},
      {"surrogate escape", "\"\\uD800\"", "lex-escape-out-of-range"},
      {"escape past the last scalar", "\"\\U00110000\"", "lex-escape-out-of-range"},
      {"empty char literal", "''", "lex-empty-char"},
      {"hex with no digits", "0x", "lex-missing-digits"},
      {"separator with no digit on one side", "let x = 1000_;\n", "lex-misplaced-separator"},
      {"separator touching a suffix", "let x = 10_u8;\n", "lex-misplaced-separator"},
      {"escape with no digits", "\"\\x\"", "lex-escape-digits"},
      {"short universal escape", "\"\\u12\"", "lex-escape-digits"},
      {"hex escape above a byte", "\"\\x1FF\"", "lex-escape-too-wide"},
      {"octal escape above a byte", "\"\\400\"", "lex-escape-too-wide"},
      {"delimited escape above a byte", "\"\\x{1F600}\"", "lex-escape-too-wide"},
      {"named escape", "\"\\N{GREEK SMALL LETTER ALPHA}\"", "lex-named-escape"},
      {"byte outside the alphabet", "`", "lex-invalid-character"},
  };

  std::set<std::string> seen;
  for (const Case& testCase : cases) {
    support::DiagBag bag;
    report(testCase.source, bag);
    std::set<std::string> codes;
    for (const support::Diagnostic& diagnostic : bag.all()) {
      codes.insert(diagnostic.code);
    }
    EXPECT_EQ(codes.count(std::string(testCase.code)), 1u)
        << testCase.label << ": expected " << testCase.code;
    seen.merge(codes);
  }

  for (const FlagInfo& info : flagInfos()) {
    EXPECT_EQ(seen.count(std::string(info.code)), 1u)
        << "no input produces " << info.code << "; add one to the table above";
  }
  EXPECT_EQ(seen.count("lex-invalid-character"), 1u);
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
  report("let x = `;\n", bag);
  ASSERT_EQ(bag.size(), 1u);
  EXPECT_EQ(bag.all()[0].code, "lex-invalid-character");
  EXPECT_EQ(bag.all()[0].message, "unexpected character '`'");
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
