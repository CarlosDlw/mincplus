// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Expressions, by precedence climbing over the one table in token_class.h.
//
// Left-associative chains are built left to right: `a - b - c` parses `a - b`
// first, then a new node adopts it and takes `- c`. The adoption is a link on
// the event stream (`CompletedMarker::precede`), so no node is ever moved and
// the tree is never re-walked.
#include "parse/parser.h"

#include <cstdint>

#include "token_class.h"

namespace minc::parse {

void Parser::parseExpr() {
  DepthGuard depth(*this);
  if (!depth.ok()) {
    tooDeep();
    Marker bad = start();
    bad.complete(SyntaxKind::Error);
    return;
  }
  parseAssign();
}

CompletedMarker Parser::parseAssign() {
  CompletedMarker lhs = parseConditional();
  if (isAssignmentOperator(current())) {
    Marker assign = lhs.precede();
    bump();        // the operator
    parseAssign(); // right-associative
    return assign.complete(SyntaxKind::AssignExpr);
  }
  return lhs;
}

CompletedMarker Parser::parseConditional() {
  CompletedMarker condition = parseBinary(kLowestBinaryPrecedence);
  if (!at(lex::TokenKind::Question)) {
    return condition;
  }
  Marker conditional = condition.precede();
  bump();      // `?`
  parseExpr(); // the middle admits assignment, as in C
  expect(lex::TokenKind::Colon);
  parseConditional(); // right-associative
  return conditional.complete(SyntaxKind::ConditionalExpr);
}

CompletedMarker Parser::parseBinary(std::uint8_t minPrecedence) {
  CompletedMarker lhs = parseUnary();
  while (!bailedOut_) {
    const BinaryOpInfo* info = binaryInfo(current());
    if (info == nullptr || info->precedence < minPrecedence) {
      break;
    }
    Marker binary = lhs.precede();
    bump(); // the operator
    // Left-associative: the right side must bind strictly tighter, which is
    // what makes `a - b - c` a left-nested chain.
    const auto next = static_cast<std::uint8_t>(
        info->rightAssociative ? info->precedence
                               : static_cast<std::uint8_t>(info->precedence + 1));
    parseBinary(next);
    lhs = binary.complete(SyntaxKind::BinaryExpr);
  }
  return lhs;
}

CompletedMarker Parser::parseUnary() {
  if (isPrefixOperator(current())) {
    Marker prefix = start();
    bump();       // the operator
    parseUnary(); // right-associative, so `--x` and `- -x` nest correctly
    return prefix.complete(SyntaxKind::PrefixExpr);
  }
  return parsePostfix();
}

CompletedMarker Parser::parsePostfix() {
  CompletedMarker expr = parsePrimary();
  while (!bailedOut_) {
    if (at(lex::TokenKind::LParen)) {
      Marker call = expr.precede();
      bump(); // `(`
      parseArgList();
      expect(lex::TokenKind::RParen);
      expr = call.complete(SyntaxKind::CallExpr);
    } else if (at(lex::TokenKind::PlusPlus) || at(lex::TokenKind::MinusMinus)) {
      Marker postfix = expr.precede();
      bump();
      expr = postfix.complete(SyntaxKind::PostfixExpr);
    } else {
      break;
    }
  }
  return expr;
}

CompletedMarker Parser::parsePrimary() {
  switch (current()) {
  case lex::TokenKind::IntegerLiteral:
  case lex::TokenKind::FloatLiteral:
  case lex::TokenKind::CharLiteral:
  case lex::TokenKind::StringLiteral: {
    Marker literal = start();
    bump();
    return literal.complete(SyntaxKind::LiteralExpr);
  }
  case lex::TokenKind::Identifier: {
    // `true`, `false`, and every type name land here: none of them is a
    // keyword yet, and the parser does not need them to be.
    Marker path = start();
    bump();
    return path.complete(SyntaxKind::PathExpr);
  }
  case lex::TokenKind::LParen: {
    Marker paren = start();
    bump(); // `(`
    parseExpr();
    expect(lex::TokenKind::RParen);
    return paren.complete(SyntaxKind::ParenExpr);
  }
  default: {
    error("expected an expression", ParseErrorCode::ExpectedExpression);
    // An empty `Error` node keeps the tree total without consuming anything;
    // the enclosing statement's progress rule takes care of moving on.
    Marker missing = start();
    return missing.complete(SyntaxKind::Error);
  }
  }
}

void Parser::parseArgList() {
  if (at(lex::TokenKind::RParen)) {
    return;
  }
  Marker args = start();
  parseExpr();
  while (at(lex::TokenKind::Comma) && !bailedOut_) {
    bump();
    parseExpr();
  }
  args.complete(SyntaxKind::ArgList);
}

} // namespace minc::parse
