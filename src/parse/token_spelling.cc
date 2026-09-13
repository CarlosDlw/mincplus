// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "parse/parser.h"

namespace minc::parse {

std::string_view tokenSpelling(lex::TokenKind kind) {
  // Only tokens with a fixed spelling need a row. Identifiers, literals, and
  // the two non-token kinds fall through to the kind's name, which is what a
  // message should say when there is nothing better to quote.
  switch (kind) {
  case lex::TokenKind::KwFn:
    return "fn";
  case lex::TokenKind::KwLet:
    return "let";
  case lex::TokenKind::KwConst:
    return "const";
  case lex::TokenKind::KwReturn:
    return "return";
  case lex::TokenKind::KwIf:
    return "if";
  case lex::TokenKind::KwElse:
    return "else";
  case lex::TokenKind::KwWhile:
    return "while";
  case lex::TokenKind::KwFor:
    return "for";
  case lex::TokenKind::KwBreak:
    return "break";
  case lex::TokenKind::KwContinue:
    return "continue";
  case lex::TokenKind::LParen:
    return "(";
  case lex::TokenKind::RParen:
    return ")";
  case lex::TokenKind::LBrace:
    return "{";
  case lex::TokenKind::RBrace:
    return "}";
  case lex::TokenKind::LBracket:
    return "[";
  case lex::TokenKind::RBracket:
    return "]";
  case lex::TokenKind::Semicolon:
    return ";";
  case lex::TokenKind::Comma:
    return ",";
  case lex::TokenKind::Colon:
    return ":";
  case lex::TokenKind::Question:
    return "?";
  case lex::TokenKind::Dot:
    return ".";
  case lex::TokenKind::Arrow:
    return "->";
  case lex::TokenKind::Equal:
    return "=";
  case lex::TokenKind::EqualEqual:
    return "==";
  case lex::TokenKind::Bang:
    return "!";
  case lex::TokenKind::BangEqual:
    return "!=";
  default:
    break;
  }
  return lex::toString(kind);
}

} // namespace minc::parse
