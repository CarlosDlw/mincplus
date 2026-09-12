// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Statements and blocks.
#include "parse/parser.h"

#include "token_class.h"

namespace minc::parse {

void Parser::parseBlock() {
  Marker block = start();

  DepthGuard depth(*this);
  if (!depth.ok()) {
    // Refusing to descend is what turns deep nesting into a diagnostic instead
    // of a stack overflow. The rest of the input becomes one `Error` node.
    tooDeep();
    block.complete(SyntaxKind::Block);
    return;
  }

  expect(lex::TokenKind::LBrace);
  while (!at(lex::TokenKind::RBrace) && !atEnd() && !bailedOut_) {
    if (isStatementStart(current())) {
      parseStmt();
    } else {
      error("expected a statement", ParseErrorCode::ExpectedStatement);
      recoverStatement();
    }
  }
  expect(lex::TokenKind::RBrace);
  block.complete(SyntaxKind::Block);
}

void Parser::parseStmt() {
  if (bailedOut_) {
    return;
  }

  switch (current()) {
  case lex::TokenKind::KwLet:
    parseLetStmt(false);
    return;
  case lex::TokenKind::KwConst:
    parseLetStmt(true);
    return;
  case lex::TokenKind::KwReturn:
    parseReturnStmt();
    return;
  case lex::TokenKind::Semicolon: {
    Marker empty = start();
    bump();
    empty.complete(SyntaxKind::EmptyStmt);
    return;
  }
  case lex::TokenKind::LBrace:
    parseBlock();
    return;
  default:
    break;
  }

  parseExprStmt();
}

void Parser::parseLetStmt(bool isConst) {
  Marker stmt = start();
  bump(); // `let` or `const`

  Marker name = start();
  if (at(lex::TokenKind::Identifier)) {
    bump();
  } else {
    error("expected a variable name", ParseErrorCode::ExpectedName);
  }
  name.complete(SyntaxKind::Name);

  if (at(lex::TokenKind::Colon)) {
    bump();
    parseType();
  }
  if (at(lex::TokenKind::Equal)) {
    bump();
    parseExpr();
  }

  expect(lex::TokenKind::Semicolon);
  stmt.complete(isConst ? SyntaxKind::ConstStmt : SyntaxKind::LetStmt);
}

void Parser::parseReturnStmt() {
  Marker stmt = start();
  bump(); // `return`
  if (!at(lex::TokenKind::Semicolon) && !atEnd() && !bailedOut_) {
    parseExpr();
  }
  expect(lex::TokenKind::Semicolon);
  stmt.complete(SyntaxKind::ReturnStmt);
}

void Parser::parseExprStmt() {
  Marker stmt = start();
  parseExpr();
  expect(lex::TokenKind::Semicolon);
  stmt.complete(SyntaxKind::ExprStmt);
}

void Parser::recoverStatement() {
  // One `Error` node for the whole skipped run, so a burst of junk is one error
  // rather than one per token. The caller only reaches this when the current
  // token cannot start a statement, so at least one token is consumed and the
  // block loop always makes progress.
  Marker junk = start();
  while (!atEnd() && !at(lex::TokenKind::RBrace) && !isStatementStart(current())) {
    bump();
  }
  junk.complete(SyntaxKind::Error);
}

} // namespace minc::parse
