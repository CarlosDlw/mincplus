// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Token classification.
//
// Tokens are classified by *what they are*, never by what a later stage will
// do with them: there is no "type name" kind or "declaration keyword" kind,
// because that would be semantic information. The kinds below are the whole
// lexical grammar, and keyword classification lives here too (see below).
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace minc::lex {

enum class TokenKind : std::uint8_t {
  EndOfFile,
  // A byte that starts no token we know. Emitted alone so the lossless
  // invariant still holds: every byte belongs to exactly one token.
  Invalid,

  // Trivia. Always produced, never skipped: the token stream is what the
  // formatter, the language server, and the lossless check consume.
  Whitespace,
  Newline,
  LineComment,
  BlockComment,

  // Literals. The raw spelling is the source text; interpreting it (value,
  // overflow, UTF-8 encoding) is a later stage's job.
  IntegerLiteral,
  FloatLiteral,
  CharLiteral,
  StringLiteral,

  Identifier,

  // Keywords.
  KwFn,
  KwLet,
  KwConst,
  KwReturn,

  // Punctuation.
  LParen,
  RParen,
  LBrace,
  RBrace,
  LBracket,
  RBracket,
  Semicolon,
  Comma,
  Colon,
  Question,
  Dot,
  Arrow,

  // Arithmetic.
  Plus,
  Minus,
  Star,
  Slash,
  Percent,
  PlusPlus,
  MinusMinus,

  // Bitwise.
  Amp,
  Pipe,
  Caret,
  Tilde,
  LessLess,
  GreaterGreater,

  // Logical.
  AmpAmp,
  PipePipe,
  Bang,

  // Comparison.
  EqualEqual,
  BangEqual,
  Less,
  LessEqual,
  Greater,
  GreaterEqual,

  // Assignment.
  Equal,
  PlusEqual,
  MinusEqual,
  StarEqual,
  SlashEqual,
  PercentEqual,
  AmpEqual,
  PipeEqual,
  CaretEqual,
  LessLessEqual,
  GreaterGreaterEqual,
};

[[nodiscard]] constexpr bool isTrivia(TokenKind kind) {
  switch (kind) {
  case TokenKind::Whitespace:
  case TokenKind::Newline:
  case TokenKind::LineComment:
  case TokenKind::BlockComment:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] constexpr bool isKeyword(TokenKind kind) {
  switch (kind) {
  case TokenKind::KwFn:
  case TokenKind::KwLet:
  case TokenKind::KwConst:
  case TokenKind::KwReturn:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] constexpr bool isLiteral(TokenKind kind) {
  switch (kind) {
  case TokenKind::IntegerLiteral:
  case TokenKind::FloatLiteral:
  case TokenKind::CharLiteral:
  case TokenKind::StringLiteral:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] constexpr bool isOperator(TokenKind kind) {
  switch (kind) {
  case TokenKind::Plus:
  case TokenKind::Minus:
  case TokenKind::Star:
  case TokenKind::Slash:
  case TokenKind::Percent:
  case TokenKind::PlusPlus:
  case TokenKind::MinusMinus:
  case TokenKind::Amp:
  case TokenKind::Pipe:
  case TokenKind::Caret:
  case TokenKind::Tilde:
  case TokenKind::LessLess:
  case TokenKind::GreaterGreater:
  case TokenKind::AmpAmp:
  case TokenKind::PipePipe:
  case TokenKind::Bang:
  case TokenKind::EqualEqual:
  case TokenKind::BangEqual:
  case TokenKind::Less:
  case TokenKind::LessEqual:
  case TokenKind::Greater:
  case TokenKind::GreaterEqual:
  case TokenKind::Equal:
  case TokenKind::PlusEqual:
  case TokenKind::MinusEqual:
  case TokenKind::StarEqual:
  case TokenKind::SlashEqual:
  case TokenKind::PercentEqual:
  case TokenKind::AmpEqual:
  case TokenKind::PipeEqual:
  case TokenKind::CaretEqual:
  case TokenKind::LessLessEqual:
  case TokenKind::GreaterGreaterEqual:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] constexpr bool isPunctuation(TokenKind kind) {
  switch (kind) {
  case TokenKind::LParen:
  case TokenKind::RParen:
  case TokenKind::LBrace:
  case TokenKind::RBrace:
  case TokenKind::LBracket:
  case TokenKind::RBracket:
  case TokenKind::Semicolon:
  case TokenKind::Comma:
  case TokenKind::Colon:
  case TokenKind::Question:
  case TokenKind::Dot:
  case TokenKind::Arrow:
    return true;
  default:
    return false;
  }
}

// Stable name for tooling, tests, and the token dump. Never localized, never
// abbreviated, and independent of the source spelling.
[[nodiscard]] const char* toString(TokenKind kind);

struct Keyword {
  std::string_view text;
  TokenKind kind;
};

// The keyword table, in one place. The parser, the dump, and error messages
// all consult this instead of repeating the list.
//
// Primitive type names (`i32`, `u8`, ...) and the C spellings (`int`, `long`)
// are deliberately *not* here yet: they are reserved words in the language
// design but the decision to make them lexical keywords is still open, and
// until it is settled they lex as identifiers.
[[nodiscard]] std::span<const Keyword> keywords();

[[nodiscard]] std::optional<TokenKind> keywordFromText(std::string_view text);

} // namespace minc::lex
