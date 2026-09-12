// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "lex/token_dump.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "lex/token_kind.h"
#include "support/limits.h"

#include "byte_class.h"

namespace minc::lex {
namespace {

constexpr const char* kReset = "\x1b[0m";
constexpr const char* kDim = "\x1b[2m";
constexpr const char* kInvalidColor = "\x1b[1;31m";
constexpr const char* kKeywordColor = "\x1b[1;34m";
constexpr const char* kLiteralColor = "\x1b[32m";
constexpr const char* kOperatorColor = "\x1b[33m";
constexpr const char* kIdentifierColor = "\x1b[36m";
constexpr const char* kEndColor = "\x1b[1;37m";
constexpr const char* kFlagColor = "\x1b[1;31m";
constexpr const char* kEllipsis = "...";

[[nodiscard]] char hexDigit(unsigned value) {
  return static_cast<char>(value < 10U ? ('0' + value) : ('A' + (value - 10U)));
}

// One category per token, so the dump reads as syntax highlighting and the
// colors are decided in exactly one place.
[[nodiscard]] const char* kindColor(TokenKind kind) {
  if (isTrivia(kind)) {
    return kDim;
  }
  if (isKeyword(kind)) {
    return kKeywordColor;
  }
  if (isLiteral(kind)) {
    return kLiteralColor;
  }
  if (isOperator(kind) || isPunctuation(kind)) {
    return kOperatorColor;
  }
  if (kind == TokenKind::Identifier) {
    return kIdentifierColor;
  }
  if (kind == TokenKind::Invalid) {
    return kInvalidColor;
  }
  return kEndColor;
}

[[nodiscard]] std::string colored(std::string_view text, const char* color, bool enabled) {
  if (!enabled) {
    return std::string(text);
  }
  std::string out(color);
  out.append(text);
  out.append(kReset);
  return out;
}

// "1 byte" but "0 bytes": the header is read by people, so it reads like
// English instead of like a template.
[[nodiscard]] std::string countLabel(std::size_t count, const char* singular, const char* plural) {
  return std::to_string(count) + (count == 1 ? singular : plural);
}

[[nodiscard]] std::string padLeft(std::string_view text, std::size_t width) {
  std::string out;
  if (text.size() < width) {
    out.append(width - text.size(), ' ');
  }
  out.append(text);
  return out;
}

[[nodiscard]] std::string padRight(std::string_view text, std::size_t width) {
  std::string out(text);
  if (out.size() < width) {
    out.append(width - out.size(), ' ');
  }
  return out;
}

// Non-printable bytes become their escape, so a Newline token shows as `\n`
// instead of breaking the table in half.
[[nodiscard]] std::string escapeSpelling(std::string_view lexeme) {
  std::string out;
  out.reserve(lexeme.size() + 8);
  for (const char raw : lexeme) {
    const auto byte = static_cast<unsigned char>(raw);
    switch (byte) {
    case '\n':
      out += "\\n";
      continue;
    case '\r':
      out += "\\r";
      continue;
    case '\t':
      out += "\\t";
      continue;
    case '\v':
      out += "\\v";
      continue;
    case '\f':
      out += "\\f";
      continue;
    case '\\':
      out += "\\\\";
      continue;
    default:
      break;
    }
    if (byte < 0x20U || byte == 0x7FU) {
      out += "\\x";
      out.push_back(hexDigit(byte >> 4U));
      out.push_back(hexDigit(byte & 0x0FU));
      continue;
    }
    out.push_back(raw);
  }
  return out;
}

// Truncates on the input rather than on the escaped output, and never in the
// middle of a UTF-8 sequence.
[[nodiscard]] std::string spellingOf(std::string_view lexeme, std::size_t maxBytes) {
  if (lexeme.size() <= maxBytes) {
    return escapeSpelling(lexeme);
  }
  std::size_t cut = maxBytes;
  while (cut > 0 && isContinuationByte(static_cast<Byte>(lexeme[cut]))) {
    --cut;
  }
  std::string out = escapeSpelling(lexeme.substr(0, cut));
  out += kEllipsis;
  return out;
}

[[nodiscard]] std::string flagNames(TokenFlags flags) {
  if (flags == 0) {
    return "-";
  }
  std::string out;
  for (const FlagInfo& info : flagInfos()) {
    if (!hasFlag(flags, info.flag)) {
      continue;
    }
    if (!out.empty()) {
      out.push_back(',');
    }
    out += info.name;
  }
  return out.empty() ? "-" : out;
}

// Position cursor advanced over the token stream itself. Counting terminators
// with the same rule as LineTable (LF, CRLF, lone CR) makes the printed
// `pos` column agree with what a diagnostic would say, without the dump having
// to be handed a line table.
struct Cursor {
  std::uint32_t line = 1;
  std::uint32_t col = 1;

  void advance(std::string_view lexeme) {
    std::size_t i = 0;
    while (i < lexeme.size()) {
      const std::uint32_t newline = newlineLengthAt(lexeme, i);
      if (newline != 0) {
        ++line;
        col = 1;
        i += newline;
        continue;
      }
      ++col;
      ++i;
    }
  }
};

struct Row {
  std::string pos;
  std::string offset;
  std::string length;
  TokenKind kind = TokenKind::EndOfFile;
  std::string flags;
  std::string spelling;
};

} // namespace

std::string dumpTokens(const TokenStream& stream, std::string_view path, DumpOptions options) {
  const bool color = options.color == support::ColorMode::Ansi;
  const std::size_t maxSpelling = std::max<std::size_t>(options.maxSpellingBytes, 4);

  std::vector<Row> rows;
  rows.reserve(stream.size());

  // Grow-on-demand rather than a fixed table: a new token kind must not be able
  // to walk off the end of a hand-sized array.
  std::vector<std::size_t> kindCounts;

  Cursor cursor;

  for (const Token& token : stream.tokens()) {
    const std::string_view lexeme = stream.text().substr(token.offset, token.length);

    Row row;
    row.pos = std::to_string(cursor.line) + ":" + std::to_string(cursor.col);
    row.offset = std::to_string(token.offset);
    row.length = std::to_string(token.length);
    row.kind = token.kind;
    row.flags = flagNames(token.flags);
    row.spelling = spellingOf(lexeme, maxSpelling);
    rows.push_back(std::move(row));

    const auto index = static_cast<std::size_t>(token.kind);
    if (index >= kindCounts.size()) {
      kindCounts.resize(index + 1, 0);
    }
    ++kindCounts[index];

    cursor.advance(lexeme);
  }

  // Column widths come from the data, so the table is aligned whatever the file
  // holds. The header labels take part so a two-line file still lines up.
  std::size_t posWidth = 3;
  std::size_t offsetWidth = 6;
  std::size_t lengthWidth = 3;
  std::size_t kindWidth = 4;
  std::size_t flagsWidth = 5;
  for (const Row& row : rows) {
    posWidth = std::max(posWidth, row.pos.size());
    offsetWidth = std::max(offsetWidth, row.offset.size());
    lengthWidth = std::max(lengthWidth, row.length.size());
    kindWidth = std::max(kindWidth, std::string_view(toString(row.kind)).size());
    flagsWidth = std::max(flagsWidth, row.flags.size());
  }

  std::string out;
  out.reserve(rows.size() * 64U + 256U);

  out += "== ";
  out += path;
  out += "  (";
  out += countLabel(stream.text().size(), " byte", " bytes");
  out += ", ";
  out += countLabel(stream.size(), " token: ", " tokens: ");
  out += std::to_string(stream.significantCount());
  out += " significant, ";
  out += std::to_string(stream.triviaCount());
  out += " trivia)\n";

  if (!stream.lossless()) {
    // Should be unreachable; if it ever happens it is the one thing worth
    // shouting about, because every offset downstream depends on it.
    out += "   !! token stream does NOT cover the input byte for byte\n";
  }
  out += '\n';

  out += "  ";
  out += padLeft("pos", posWidth);
  out += "  ";
  out += padLeft("offset", offsetWidth);
  out += "  ";
  out += padLeft("len", lengthWidth);
  out += "  ";
  out += padRight("kind", kindWidth);
  out += "  ";
  out += padRight("flags", flagsWidth);
  out += "  spelling\n";

  out += "  ";
  out += std::string(posWidth, '-');
  out += "  ";
  out += std::string(offsetWidth, '-');
  out += "  ";
  out += std::string(lengthWidth, '-');
  out += "  ";
  out += std::string(kindWidth, '-');
  out += "  ";
  out += std::string(flagsWidth, '-');
  out += "  ";
  out += std::string(std::min<std::size_t>(maxSpelling, 32) + 8, '-');
  out += '\n';

  for (const Row& row : rows) {
    const TokenKind kind = row.kind;
    const bool flagged = row.flags != "-";

    out += "  ";
    out += padLeft(row.pos, posWidth);
    out += "  ";
    out += padLeft(row.offset, offsetWidth);
    out += "  ";
    out += padLeft(row.length, lengthWidth);
    out += "  ";
    out += colored(padRight(toString(kind), kindWidth), kindColor(kind), color);
    out += "  ";
    out += colored(padRight(row.flags, flagsWidth), flagged ? kFlagColor : kDim, color);
    out += "  ";
    // Trivia is dimmed: it makes the code tokens stand out without hiding what
    // the whitespace and comments actually are.
    out += isTrivia(kind) ? colored(row.spelling, kDim, color) : row.spelling;
    out += '\n';
  }

  if (!options.showHistogram || rows.empty()) {
    return out;
  }

  std::size_t nameWidth = 0;
  for (std::size_t i = 0; i < kindCounts.size(); ++i) {
    if (kindCounts[i] != 0) {
      nameWidth = std::max(nameWidth, std::string_view(toString(static_cast<TokenKind>(i))).size());
    }
  }

  out += '\n';
  out += "  tokens by kind:\n";
  for (std::size_t i = 0; i < kindCounts.size(); ++i) {
    if (kindCounts[i] == 0) {
      continue;
    }
    const auto kind = static_cast<TokenKind>(i);
    out += "    ";
    out += colored(padRight(toString(kind), nameWidth), kindColor(kind), color);
    out += "  ";
    out += padLeft(std::to_string(kindCounts[i]), 8);
    out += '\n';
  }

  return out;
}

} // namespace minc::lex
