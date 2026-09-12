// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "lex/token_dump.h"
#include "lex/token_kind.h"
#include "lex/token_stream.h"
#include "support/line/line_col.h"
#include "support/line/line_table.h"
#include "support/term/terminal.h"

namespace minc::lex {
namespace {

std::string trim(std::string_view text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && text[begin] == ' ') {
    ++begin;
  }
  while (end > begin && text[end - 1] == ' ') {
    --end;
  }
  return std::string(text.substr(begin, end - begin));
}

// Removes SGR sequences so one parser can read plain and colored output.
std::string stripAnsi(std::string_view text) {
  std::string out;
  for (std::size_t i = 0; i < text.size();) {
    if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
      i += 2;
      while (i < text.size() && !(text[i] >= '@' && text[i] <= '~')) {
        ++i;
      }
      if (i < text.size()) {
        ++i; // the final byte of the sequence
      }
      continue;
    }
    out.push_back(text[i]);
    ++i;
  }
  return out;
}

bool isDigits(std::string_view text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
  });
}

// `line:col`, which is what the first column of a data row holds. The header and
// the histogram do not match this, so it is also the row filter.
bool looksLikePosition(std::string_view text) {
  const std::size_t colon = text.find(':');
  if (colon == std::string_view::npos) {
    return false;
  }
  return isDigits(text.substr(0, colon)) && isDigits(text.substr(colon + 1));
}

struct DumpRow {
  std::size_t posColumn = 0;
  std::size_t kindColumn = 0;
  std::size_t spellingColumn = 0;
  std::string pos;
  std::string offset;
  std::string length;
  std::string kind;
  std::string flags;
  std::string spelling;
};

// Parses the table by reading the column positions off the separator line
// first. That makes the test independent of how wide any field happens to be,
// so it keeps working when a longer kind name widens a column.
std::vector<DumpRow> parseRows(const std::string& raw) {
  const std::string text = stripAnsi(raw);
  std::vector<std::pair<std::size_t, std::size_t>> columns;

  for (std::size_t lineStart = 0; lineStart <= text.size();) {
    const std::size_t newline = text.find('\n', lineStart);
    const std::size_t lineEnd = newline == std::string::npos ? text.size() : newline;
    const std::string_view line(text.data() + lineStart, lineEnd - lineStart);
    lineStart = lineEnd + 1;
    if (line.size() > 2 && line[0] == ' ' && line[1] == ' ' && line[2] == '-') {
      columns.clear();
      for (std::size_t i = 0; i < line.size();) {
        if (line[i] == '-') {
          const std::size_t begin = i;
          while (i < line.size() && line[i] == '-') {
            ++i;
          }
          columns.emplace_back(begin, i);
        } else {
          ++i;
        }
      }
      break;
    }
  }

  std::vector<DumpRow> rows;
  if (columns.size() != 6) {
    return rows;
  }

  const auto field = [&text](std::size_t lineStart, std::size_t lineEnd,
                             std::pair<std::size_t, std::size_t> column) {
    const std::size_t begin = lineStart + column.first;
    if (begin >= lineEnd) {
      return std::string();
    }
    const std::size_t end = std::min(lineStart + column.second, lineEnd);
    return trim(std::string_view(text.data() + begin, end - begin));
  };

  for (std::size_t lineStart = 0; lineStart < text.size();) {
    const std::size_t newline = text.find('\n', lineStart);
    const std::size_t lineEnd = newline == std::string::npos ? text.size() : newline;
    const std::string_view line(text.data() + lineStart, lineEnd - lineStart);
    const std::size_t posBegin = std::min(columns[0].first, line.size());
    const std::size_t posEnd = std::min(columns[0].second, line.size());
    if (looksLikePosition(trim(line.substr(posBegin, posEnd - posBegin)))) {
      DumpRow row;
      row.posColumn = columns[0].first;
      row.kindColumn = columns[3].first;
      row.spellingColumn = columns[5].first;
      row.pos = field(lineStart, lineEnd, columns[0]);
      row.offset = field(lineStart, lineEnd, columns[1]);
      row.length = field(lineStart, lineEnd, columns[2]);
      row.kind = field(lineStart, lineEnd, columns[3]);
      row.flags = field(lineStart, lineEnd, columns[4]);
      // The spelling is the last column and may run past its dashes.
      const std::size_t spellingBegin = lineStart + columns[5].first;
      row.spelling =
          spellingBegin < lineEnd
              ? trim(std::string_view(text.data() + spellingBegin, lineEnd - spellingBegin))
              : std::string();
      rows.push_back(std::move(row));
    }
    lineStart = lineEnd + 1;
  }
  return rows;
}

std::string histogramSection(const std::string& dump) {
  const std::size_t at = dump.find("tokens by kind:");
  return at == std::string::npos ? std::string() : dump.substr(at);
}

TEST(TokenDumpTest, HeaderCountsBytesAndTokens) {
  const std::string_view text = "let x = 1;\n"; // 11 bytes
  const TokenStream stream = TokenStream::lex(0, text);
  const std::string dump = dumpTokens(stream, "t.mx");

  EXPECT_NE(dump.find("== t.mx"), std::string::npos);
  EXPECT_NE(dump.find("(11 bytes, 10 tokens: 6 significant, 4 trivia)"), std::string::npos);
  EXPECT_EQ(dump.find("does NOT cover"), std::string::npos);
}

TEST(TokenDumpTest, EveryRowMatchesItsToken) {
  const std::string_view text = "// c\nlet x: i32 = 0x1F; /* b */\n";
  const TokenStream stream = TokenStream::lex(0, text);
  const std::vector<DumpRow> rows = parseRows(dumpTokens(stream, "t.mx"));

  ASSERT_EQ(rows.size(), stream.size());
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const Token& token = stream.tokens()[i];
    EXPECT_EQ(rows[i].offset, std::to_string(token.offset)) << i;
    EXPECT_EQ(rows[i].length, std::to_string(token.length)) << i;
    EXPECT_EQ(rows[i].kind, toString(token.kind)) << i;
    // `-` is exactly the "no flags" marker, in both directions.
    EXPECT_EQ(rows[i].flags == "-", !token.hasAnyFlag()) << i;
  }
}

TEST(TokenDumpTest, TotalRowsAreAligned) {
  const std::string_view text = "fn i32 main() {\n  let s = \"a\";\n  return 0;\n}\n";
  const std::vector<DumpRow> rows = parseRows(dumpTokens(TokenStream::lex(0, text), "a.mx"));

  ASSERT_GT(rows.size(), 5u);
  for (const DumpRow& row : rows) {
    EXPECT_EQ(row.posColumn, rows[0].posColumn);
    EXPECT_EQ(row.kindColumn, rows[0].kindColumn);
    EXPECT_EQ(row.spellingColumn, rows[0].spellingColumn);
  }
}

// The dump prints its own `line:col` column by walking the stream. This is the
// cross-check that the walk and the line table agree, including across a CRLF
// line ending and a block comment that spans a newline.
TEST(TokenDumpTest, PositionsMatchTheLineTable) {
  const std::string_view text = "let a = 1;\r\nlet b = 2;\n/* x\ny */ let c = 3;\n";
  const TokenStream stream = TokenStream::lex(0, text);
  const support::LineTable lines(text);
  const auto size = static_cast<std::uint32_t>(text.size());
  const std::vector<DumpRow> rows = parseRows(dumpTokens(stream, "p.mx"));

  ASSERT_EQ(rows.size(), stream.size());
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const support::LineCol pos = lines.lookup(stream.tokens()[i].offset, size);
    const std::string expected = std::to_string(pos.line) + ":" + std::to_string(pos.col);
    EXPECT_EQ(rows[i].pos, expected) << i;
  }
}

TEST(TokenDumpTest, NonPrintableSpellingsAreEscaped) {
  const std::string_view text = "\t\n";
  const std::vector<DumpRow> rows = parseRows(dumpTokens(TokenStream::lex(0, text), "e.mx"));

  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(rows[0].kind, "Whitespace");
  EXPECT_EQ(rows[0].spelling, R"(\t)");
  EXPECT_EQ(rows[1].kind, "Newline");
  EXPECT_EQ(rows[1].spelling, R"(\n)");
  EXPECT_EQ(rows[2].kind, "EndOfFile");
  EXPECT_EQ(rows[2].spelling, "");
}

// Raw string literals keep the backslashes in this test unambiguous: the input
// holds one backslash and the dump shows two.
TEST(TokenDumpTest, BackslashesAreEscapedInTheSpelling) {
  const std::string_view text = R"(// a\b)";
  const std::vector<DumpRow> rows = parseRows(dumpTokens(TokenStream::lex(0, text), "b.mx"));

  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].kind, "LineComment");
  EXPECT_EQ(rows[0].spelling, R"(// a\\b)");
}

TEST(TokenDumpTest, LongLexemesAreTruncated) {
  const DumpOptions defaults;
  const std::vector<DumpRow> rows =
      parseRows(dumpTokens(TokenStream::lex(0, std::string(200, 'a')), "l.mx"));

  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].spelling, std::string(defaults.maxSpellingBytes, 'a') + "...");
}

// Truncation works on the input, so a cut can never land inside a UTF-8
// sequence and print a broken character.
TEST(TokenDumpTest, TruncationNeverSplitsAUtf8Sequence) {
  // One string literal holding three two-byte characters. Bounding at byte 4
  // would cut the third character in half, so the cut backs off to byte 3.
  const std::string text = "\"\xC3\xA9\xC3\xA9\xC3\xA9\"";
  DumpOptions options;
  options.maxSpellingBytes = 4;
  const std::vector<DumpRow> rows =
      parseRows(dumpTokens(TokenStream::lex(0, text), "u.mx", options));

  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].kind, "StringLiteral");
  EXPECT_EQ(rows[0].spelling, "\"\xC3\xA9...");
}

TEST(TokenDumpTest, FlaggedTokensShowTheirFlags) {
  const std::string_view text = "let s = \"abc\n";
  const std::vector<DumpRow> rows = parseRows(dumpTokens(TokenStream::lex(0, text), "f.mx"));

  bool found = false;
  for (const DumpRow& row : rows) {
    if (row.kind == "StringLiteral") {
      found = true;
      EXPECT_EQ(row.flags, "unterminated-string");
    }
  }
  EXPECT_TRUE(found);
}

TEST(TokenDumpTest, PlainModeHasNoEscapeSequences) {
  const std::string dump = dumpTokens(TokenStream::lex(0, "let x = 1;\n"), "c.mx");
  EXPECT_EQ(dump.find('\x1b'), std::string::npos);
}

TEST(TokenDumpTest, ColorNeverShiftsAColumn) {
  const std::string_view text = "fn i32 main() {\n  let s = 1;\n}\n";
  const TokenStream stream = TokenStream::lex(0, text);
  const std::string plain = dumpTokens(stream, "c.mx");
  const std::string colored = dumpTokens(stream, "c.mx", DumpOptions{support::ColorMode::Ansi});

  EXPECT_NE(colored.find('\x1b'), std::string::npos);
  EXPECT_EQ(stripAnsi(colored), plain);

  const std::vector<DumpRow> plainRows = parseRows(plain);
  const std::vector<DumpRow> coloredRows = parseRows(colored);
  ASSERT_EQ(plainRows.size(), coloredRows.size());
  for (std::size_t i = 0; i < plainRows.size(); ++i) {
    EXPECT_EQ(coloredRows[i].kindColumn, plainRows[i].kindColumn) << i;
    EXPECT_EQ(coloredRows[i].spelling, plainRows[i].spelling) << i;
  }
}

TEST(TokenDumpTest, HistogramAccountsForEveryToken) {
  const std::string_view text = "// c\nlet x = \"s\";\n";
  const TokenStream stream = TokenStream::lex(0, text);
  const std::string dump = dumpTokens(stream, "h.mx");

  const std::string section = histogramSection(dump);
  ASSERT_FALSE(section.empty());

  std::size_t total = 0;
  for (std::size_t i = 0; i < section.size();) {
    if (std::isdigit(static_cast<unsigned char>(section[i])) != 0) {
      std::size_t value = 0;
      while (i < section.size() && std::isdigit(static_cast<unsigned char>(section[i])) != 0) {
        value = value * 10U + static_cast<std::size_t>(section[i] - '0');
        ++i;
      }
      total += value;
      continue;
    }
    ++i;
  }
  EXPECT_EQ(total, stream.size());
}

TEST(TokenDumpTest, HistogramCanBeTurnedOff) {
  DumpOptions options;
  options.showHistogram = false;
  const std::string dump = dumpTokens(TokenStream::lex(0, "let x = 1;"), "n.mx", options);
  EXPECT_EQ(dump.find("tokens by kind:"), std::string::npos);
}

} // namespace
} // namespace minc::lex
