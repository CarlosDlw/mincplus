// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// What a token means to the parser, in one place.
//
// The lexer classifies tokens by what they *are*; this file classifies them by
// what the grammar does with them (prefix, infix, statement leader, expression
// leader) and holds the one operator precedence table. Precedence is data, not
// a call stack: one function per level would put the grammar in the call order,
// where a misplaced call is a silent misparse. Precedence climbing over this
// table is the same algorithm Pratt parsing describes.
#pragma once

#include <cstddef>
#include <cstdint>

#include "lex/token_kind.h"

namespace minc::parse {

struct BinaryOpInfo {
  lex::TokenKind kind;
  // Larger binds tighter. The values follow C, 3 (lowest binary) to 12.
  std::uint8_t precedence;
  bool rightAssociative;
};

inline constexpr BinaryOpInfo kBinaryOps[] = {
    {lex::TokenKind::PipePipe, 3, false},
    {lex::TokenKind::AmpAmp, 4, false},
    {lex::TokenKind::Pipe, 5, false},
    {lex::TokenKind::Caret, 6, false},
    {lex::TokenKind::Amp, 7, false},
    {lex::TokenKind::EqualEqual, 8, false},
    {lex::TokenKind::BangEqual, 8, false},
    {lex::TokenKind::Less, 9, false},
    {lex::TokenKind::LessEqual, 9, false},
    {lex::TokenKind::Greater, 9, false},
    {lex::TokenKind::GreaterEqual, 9, false},
    {lex::TokenKind::LessLess, 10, false},
    {lex::TokenKind::GreaterGreater, 10, false},
    {lex::TokenKind::Plus, 11, false},
    {lex::TokenKind::Minus, 11, false},
    {lex::TokenKind::Star, 12, false},
    {lex::TokenKind::Slash, 12, false},
    {lex::TokenKind::Percent, 12, false},
};

inline constexpr std::uint8_t kLowestBinaryPrecedence = 3;

inline constexpr std::size_t kBinaryOpCount = sizeof(kBinaryOps) / sizeof(kBinaryOps[0]);

[[nodiscard]] inline const BinaryOpInfo* binaryInfo(lex::TokenKind kind) {
  for (std::size_t i = 0; i < kBinaryOpCount; ++i) {
    if (kBinaryOps[i].kind == kind) {
      return &kBinaryOps[i];
    }
  }
  return nullptr;
}

// `=`, `+=`, and friends. Right-associative, lower precedence than `?:`.
[[nodiscard]] inline bool isAssignmentOperator(lex::TokenKind kind) {
  switch (kind) {
  case lex::TokenKind::Equal:
  case lex::TokenKind::PlusEqual:
  case lex::TokenKind::MinusEqual:
  case lex::TokenKind::StarEqual:
  case lex::TokenKind::SlashEqual:
  case lex::TokenKind::PercentEqual:
  case lex::TokenKind::AmpEqual:
  case lex::TokenKind::PipeEqual:
  case lex::TokenKind::CaretEqual:
  case lex::TokenKind::LessLessEqual:
  case lex::TokenKind::GreaterGreaterEqual:
    return true;
  default:
    return false;
  }
}

// Prefix operators that sit just below postfix in precedence. Pointer
// operators (`*`, `&`) are deliberately absent until their syntax is decided.
[[nodiscard]] inline bool isPrefixOperator(lex::TokenKind kind) {
  switch (kind) {
  case lex::TokenKind::Minus:
  case lex::TokenKind::Plus:
  case lex::TokenKind::Bang:
  case lex::TokenKind::Tilde:
  case lex::TokenKind::PlusPlus:
  case lex::TokenKind::MinusMinus:
    return true;
  default:
    return false;
  }
}

// A token an expression can begin with. Used to decide whether a statement
// starts an expression before committing to parsing one, which is what keeps a
// stray token from producing two errors instead of one.
[[nodiscard]] inline bool isExpressionStart(lex::TokenKind kind) {
  return lex::isLiteral(kind) || kind == lex::TokenKind::Identifier ||
         kind == lex::TokenKind::LParen || isPrefixOperator(kind);
}

[[nodiscard]] inline bool isStatementStart(lex::TokenKind kind) {
  // The control-flow keywords are one list, owned by the lexer, so a keyword
  // that heads a statement is classified there once and recognized here without
  // a second enumeration to keep in step.
  if (lex::isControlFlowKeyword(kind)) {
    return true;
  }
  switch (kind) {
  case lex::TokenKind::KwLet:
  case lex::TokenKind::KwConst:
  case lex::TokenKind::KwReturn:
  case lex::TokenKind::Semicolon:
  case lex::TokenKind::LBrace:
    return true;
  default:
    return isExpressionStart(kind);
  }
}

} // namespace minc::parse
