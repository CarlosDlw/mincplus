// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "lex/token_kind.h"

#include <array>
#include <cstddef>

namespace minc::lex {
namespace {

// The keyword set, in one place. Adding a keyword is a one-line change here
// plus an enumerator in token_kind.h; the lexer, the dump, and the tests all
// read this table instead of repeating the list.
//
// Deliberately tiny: only what the examples in `examples/` actually use.
// Primitive type names (`i32`, `u8`, and the C spellings) and `true`/`false`
// are *not* keywords yet. They are reserved words in the language design, but
// making them lexical keywords is a decision that follows the type system, and
// until it is taken they lex as plain identifiers.
//
// Sorted by spelling so the table can be binary-searched and so that a diff
// adding a keyword is obviously in the right place.
constexpr std::array<Keyword, 4> kKeywords{{
    {"const", TokenKind::KwConst},
    {"fn", TokenKind::KwFn},
    {"let", TokenKind::KwLet},
    {"return", TokenKind::KwReturn},
}};

} // namespace

std::span<const Keyword> keywords() {
  return kKeywords;
}

std::optional<TokenKind> keywordFromText(std::string_view text) {
  // Length is compared first: it rejects almost every identifier before the
  // first character is ever examined, and it keeps the lookup cheap on the hot
  // path where every identifier is tested.
  for (const Keyword& keyword : kKeywords) {
    if (keyword.text.size() == text.size() && keyword.text == text) {
      return keyword.kind;
    }
  }
  return std::nullopt;
}

const char* toString(TokenKind kind) {
  // No `default:` on purpose: adding an enumerator without a name here must
  // fail the build (`-Wswitch`), not silently print a placeholder.
  switch (kind) {
  case TokenKind::EndOfFile:
    return "EndOfFile";
  case TokenKind::Invalid:
    return "Invalid";
  case TokenKind::Whitespace:
    return "Whitespace";
  case TokenKind::Newline:
    return "Newline";
  case TokenKind::LineComment:
    return "LineComment";
  case TokenKind::BlockComment:
    return "BlockComment";
  case TokenKind::IntegerLiteral:
    return "IntegerLiteral";
  case TokenKind::FloatLiteral:
    return "FloatLiteral";
  case TokenKind::CharLiteral:
    return "CharLiteral";
  case TokenKind::StringLiteral:
    return "StringLiteral";
  case TokenKind::Identifier:
    return "Identifier";
  case TokenKind::KwFn:
    return "KwFn";
  case TokenKind::KwLet:
    return "KwLet";
  case TokenKind::KwConst:
    return "KwConst";
  case TokenKind::KwReturn:
    return "KwReturn";
  case TokenKind::LParen:
    return "LParen";
  case TokenKind::RParen:
    return "RParen";
  case TokenKind::LBrace:
    return "LBrace";
  case TokenKind::RBrace:
    return "RBrace";
  case TokenKind::LBracket:
    return "LBracket";
  case TokenKind::RBracket:
    return "RBracket";
  case TokenKind::Semicolon:
    return "Semicolon";
  case TokenKind::Comma:
    return "Comma";
  case TokenKind::Colon:
    return "Colon";
  case TokenKind::Question:
    return "Question";
  case TokenKind::Dot:
    return "Dot";
  case TokenKind::Arrow:
    return "Arrow";
  case TokenKind::Plus:
    return "Plus";
  case TokenKind::Minus:
    return "Minus";
  case TokenKind::Star:
    return "Star";
  case TokenKind::Slash:
    return "Slash";
  case TokenKind::Percent:
    return "Percent";
  case TokenKind::PlusPlus:
    return "PlusPlus";
  case TokenKind::MinusMinus:
    return "MinusMinus";
  case TokenKind::Amp:
    return "Amp";
  case TokenKind::Pipe:
    return "Pipe";
  case TokenKind::Caret:
    return "Caret";
  case TokenKind::Tilde:
    return "Tilde";
  case TokenKind::LessLess:
    return "LessLess";
  case TokenKind::GreaterGreater:
    return "GreaterGreater";
  case TokenKind::AmpAmp:
    return "AmpAmp";
  case TokenKind::PipePipe:
    return "PipePipe";
  case TokenKind::Bang:
    return "Bang";
  case TokenKind::EqualEqual:
    return "EqualEqual";
  case TokenKind::BangEqual:
    return "BangEqual";
  case TokenKind::Less:
    return "Less";
  case TokenKind::LessEqual:
    return "LessEqual";
  case TokenKind::Greater:
    return "Greater";
  case TokenKind::GreaterEqual:
    return "GreaterEqual";
  case TokenKind::Equal:
    return "Equal";
  case TokenKind::PlusEqual:
    return "PlusEqual";
  case TokenKind::MinusEqual:
    return "MinusEqual";
  case TokenKind::StarEqual:
    return "StarEqual";
  case TokenKind::SlashEqual:
    return "SlashEqual";
  case TokenKind::PercentEqual:
    return "PercentEqual";
  case TokenKind::AmpEqual:
    return "AmpEqual";
  case TokenKind::PipeEqual:
    return "PipeEqual";
  case TokenKind::CaretEqual:
    return "CaretEqual";
  case TokenKind::LessLessEqual:
    return "LessLessEqual";
  case TokenKind::GreaterGreaterEqual:
    return "GreaterGreaterEqual";
  case TokenKind::Last:
    // The sentinel names no token, so it has no name. Listed explicitly because
    // the switch has no `default:` on purpose.
    return "Unknown";
  }
  return "Unknown";
}

} // namespace minc::lex
