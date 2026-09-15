// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "lex/lexer.h"

#include <cstddef>
#include <optional>

#include "lex/token_kind.h"

#include "byte_class.h"
#include "literal_scanner.h"

namespace minc::lex {
namespace {

[[nodiscard]] Token punct(std::uint32_t offset, std::uint32_t length, TokenKind kind) {
  return Token{offset, length, kind, 0};
}

// Longest match over the punctuator set.
//
// Direct-coded instead of table-driven: it is the same handful of comparisons
// either way, and it keeps the whole punctuator grammar in one readable block
// rather than split between a table and the code that indexes it. `tests/unit/lex`
// pins every spelling in this block to its kind, so a missing row fails loudly.
//
// `#` and `##` are here because they are punctuators of the lexical grammar,
// not because the preprocessor wants them: the tokenizer's job is to say what
// the bytes *are*, and only the preprocessor's job is to say what a `#` at the
// start of a line *means*. Longest match applies to `##` as it does everywhere
// else, so `# #` (two spellings) and `##` (one) stay distinguishable -- which
// is exactly the distinction the preprocessor has to make.
[[nodiscard]] std::optional<Token> scanPunctuator(std::string_view text, std::uint32_t offset) {
  const std::size_t size = text.size();
  const char c0 = text[offset];
  const char c1 = offset + 1 < size ? text[offset + 1] : '\0';
  const char c2 = offset + 2 < size ? text[offset + 2] : '\0';

  switch (c0) {
  case '<':
    if (c1 == '<') {
      return c2 == '=' ? punct(offset, 3, TokenKind::LessLessEqual)
                       : punct(offset, 2, TokenKind::LessLess);
    }
    return c1 == '=' ? punct(offset, 2, TokenKind::LessEqual) : punct(offset, 1, TokenKind::Less);
  case '>':
    if (c1 == '>') {
      return c2 == '=' ? punct(offset, 3, TokenKind::GreaterGreaterEqual)
                       : punct(offset, 2, TokenKind::GreaterGreater);
    }
    return c1 == '=' ? punct(offset, 2, TokenKind::GreaterEqual)
                     : punct(offset, 1, TokenKind::Greater);
  case '=':
    return c1 == '=' ? punct(offset, 2, TokenKind::EqualEqual) : punct(offset, 1, TokenKind::Equal);
  case '!':
    return c1 == '=' ? punct(offset, 2, TokenKind::BangEqual) : punct(offset, 1, TokenKind::Bang);
  case '&':
    if (c1 == '&') {
      return punct(offset, 2, TokenKind::AmpAmp);
    }
    return c1 == '=' ? punct(offset, 2, TokenKind::AmpEqual) : punct(offset, 1, TokenKind::Amp);
  case '|':
    if (c1 == '|') {
      return punct(offset, 2, TokenKind::PipePipe);
    }
    return c1 == '=' ? punct(offset, 2, TokenKind::PipeEqual) : punct(offset, 1, TokenKind::Pipe);
  case '^':
    return c1 == '=' ? punct(offset, 2, TokenKind::CaretEqual) : punct(offset, 1, TokenKind::Caret);
  case '+':
    if (c1 == '+') {
      return punct(offset, 2, TokenKind::PlusPlus);
    }
    return c1 == '=' ? punct(offset, 2, TokenKind::PlusEqual) : punct(offset, 1, TokenKind::Plus);
  case '-':
    if (c1 == '-') {
      return punct(offset, 2, TokenKind::MinusMinus);
    }
    if (c1 == '>') {
      return punct(offset, 2, TokenKind::Arrow);
    }
    return c1 == '=' ? punct(offset, 2, TokenKind::MinusEqual) : punct(offset, 1, TokenKind::Minus);
  case '*':
    return c1 == '=' ? punct(offset, 2, TokenKind::StarEqual) : punct(offset, 1, TokenKind::Star);
  case '/':
    return c1 == '=' ? punct(offset, 2, TokenKind::SlashEqual) : punct(offset, 1, TokenKind::Slash);
  case '%':
    return c1 == '=' ? punct(offset, 2, TokenKind::PercentEqual)
                     : punct(offset, 1, TokenKind::Percent);
  case '~':
    return punct(offset, 1, TokenKind::Tilde);
  case '(':
    return punct(offset, 1, TokenKind::LParen);
  case ')':
    return punct(offset, 1, TokenKind::RParen);
  case '{':
    return punct(offset, 1, TokenKind::LBrace);
  case '}':
    return punct(offset, 1, TokenKind::RBrace);
  case '[':
    return punct(offset, 1, TokenKind::LBracket);
  case ']':
    return punct(offset, 1, TokenKind::RBracket);
  case ';':
    return punct(offset, 1, TokenKind::Semicolon);
  case ',':
    return punct(offset, 1, TokenKind::Comma);
  case ':':
    return punct(offset, 1, TokenKind::Colon);
  case '?':
    return punct(offset, 1, TokenKind::Question);
  case '.':
    // Longest match, and the reason `....` is `...` then `.`: three dots are one
    // token, and a fourth is another one. A float literal cannot reach here --
    // `.5` is scanned as a number before the punctuator table is consulted -- so
    // there is no ambiguity between the two readings of a leading dot.
    return c1 == '.' && c2 == '.' ? punct(offset, 3, TokenKind::Ellipsis)
                                  : punct(offset, 1, TokenKind::Dot);
  case '#':
    return c1 == '#' ? punct(offset, 2, TokenKind::HashHash) : punct(offset, 1, TokenKind::Hash);
  default:
    return std::nullopt;
  }
}

// Space, tab, vertical tab, and form feed. CR and LF are their own Newline
// token so the stream tells a formatter (and the highlighter) exactly where
// lines end.
[[nodiscard]] Token scanWhitespace(std::string_view text, std::uint32_t offset) {
  std::size_t i = offset;
  while (i < text.size()) {
    const char c = text[i];
    if (c != ' ' && c != '\t' && c != '\v' && c != '\f') {
      break;
    }
    ++i;
  }
  return Token{offset, static_cast<std::uint32_t>(i - offset), TokenKind::Whitespace, 0};
}

// Runs to -- but not including -- the line terminator, so the Newline token
// owns every byte of the terminator and nothing leaks into a comment's text.
[[nodiscard]] Token scanLineComment(std::string_view text, std::uint32_t offset) {
  std::size_t i = offset + 2;
  while (i < text.size() && text[i] != '\n' && text[i] != '\r') {
    ++i;
  }
  return Token{offset, static_cast<std::uint32_t>(i - offset), TokenKind::LineComment, 0};
}

// Block comments do not nest, which is the C rule and the one every editor in
// the ecosystem already assumes.
[[nodiscard]] Token scanBlockComment(std::string_view text, std::uint32_t offset) {
  const std::size_t size = text.size();
  std::size_t i = offset + 2;
  bool closed = false;
  while (i < size) {
    if (text[i] == '*' && i + 1 < size && text[i + 1] == '/') {
      i += 2;
      closed = true;
      break;
    }
    ++i;
  }
  TokenFlags flags = 0;
  if (!closed) {
    flags = flagOf(TokenFlag::UnterminatedBlockComment);
  }
  return Token{offset, static_cast<std::uint32_t>(i - offset), TokenKind::BlockComment, flags};
}

[[nodiscard]] Token scanIdentifier(std::string_view text, std::uint32_t offset) {
  std::size_t i = offset + 1;
  while (i < text.size() && isIdentifierContinue(static_cast<Byte>(text[i]))) {
    ++i;
  }
  const auto length = static_cast<std::uint32_t>(i - offset);
  const std::optional<TokenKind> keyword = keywordFromText(text.substr(offset, length));
  return Token{offset, length, keyword.value_or(TokenKind::Identifier), 0};
}

} // namespace

Token lexOne(std::string_view text, std::uint32_t offset) {
  if (offset >= text.size()) {
    return Token{offset, 0, TokenKind::EndOfFile, 0};
  }

  const char c = text[offset];
  const auto byte = static_cast<Byte>(c);

  if (c == '\n' || c == '\r') {
    return Token{offset, newlineLengthAt(text, offset), TokenKind::Newline, 0};
  }
  if (c == ' ' || c == '\t' || c == '\v' || c == '\f') {
    return scanWhitespace(text, offset);
  }
  // Tested before the punctuator table so `//` and `/*` beat a lone `/`.
  if (c == '/' && offset + 1 < text.size()) {
    if (text[offset + 1] == '/') {
      return scanLineComment(text, offset);
    }
    if (text[offset + 1] == '*') {
      return scanBlockComment(text, offset);
    }
  }
  if (c == '"') {
    return detail::scanString(text, offset);
  }
  if (c == '\'') {
    return detail::scanChar(text, offset);
  }
  if (isAsciiDigit(byte)) {
    return detail::scanNumber(text, offset);
  }
  // `.5`, and **not** the second dot of a `..`: the one character of lookbehind
  // is what keeps `..` readable as two punctuators. Without it `a[1..2]` lexes as
  // `1` `.` `.2`, and the range operator the language reserves for slices would
  // be unreachable under any spelling -- a `.` in front of a digit is a number
  // only when the character before it is not a `.` (so `.5`, `1.5` and `a.5` are
  // untouched, and `1..2` is `1`, `.`, `.`, `2`).
  if (c == '.' && offset + 1 < text.size() && isAsciiDigit(static_cast<Byte>(text[offset + 1])) &&
      (offset == 0 || text[offset - 1] != '.')) {
    return detail::scanNumber(text, offset);
  }
  if (isIdentifierStart(byte)) {
    return scanIdentifier(text, offset);
  }
  if (const std::optional<Token> punctuator = scanPunctuator(text, offset);
      punctuator.has_value()) {
    return *punctuator;
  }
  // Nothing claims this byte. A byte >= 0x80 can only reach here outside a
  // comment or a literal, which means it is not part of the language.
  //
  // A lead byte is grouped with the continuation bytes that follow so one
  // character becomes one token and the caret underlines all of it. Anything
  // else stays one byte per token, which keeps the stream lossless even for
  // input that is not valid UTF-8 at all (a fuzz harness, a raw buffer).
  std::uint32_t length = 1;
  if (byte >= static_cast<Byte>(0x80U)) {
    while (offset + length < text.size() &&
           isContinuationByte(static_cast<Byte>(text[offset + length]))) {
      ++length;
    }
  }
  return Token{offset, length, TokenKind::Invalid, 0};
}

} // namespace minc::lex
