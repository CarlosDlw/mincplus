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
  case lex::TokenKind::KwIf:
    parseIfStmt();
    return;
  case lex::TokenKind::KwWhile:
    parseWhileStmt();
    return;
  case lex::TokenKind::KwFor:
    parseForStmt();
    return;
  case lex::TokenKind::KwBreak:
    parseJumpStmt(SyntaxKind::BreakStmt);
    return;
  case lex::TokenKind::KwContinue:
    parseJumpStmt(SyntaxKind::ContinueStmt);
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

// `if cond { ... } [else { ... } | else if ...]`
//
// There is no parenthesis in this grammar and none is needed. `(cond)` is a
// parenthesised *expression*, and a `{` cannot continue an expression, so the
// condition simply stops there and the block follows -- which is what makes
// both `if (x) { }` and `if x { }` parse, and `if (a) && b { }` parse as the
// reader expects. A language with struct literals has to add a rule forbidding
// one at the head of a condition; this one has nothing to forbid, because `{`
// is never an expression here.
void Parser::parseIfStmt() {
  Marker stmt = start();
  bump(); // `if`

  parseExpr();
  parseBlock();

  if (at(lex::TokenKind::KwElse)) {
    Marker clause = start();
    bump(); // `else`
    // `else if` is an `if` statement in the else arm, not a grammar of its own:
    // the chain is already a tree, and a second shape for it would need a second
    // traversal to answer "what does this branch do".
    if (at(lex::TokenKind::KwIf)) {
      parseIfStmt();
    } else {
      parseBlock();
    }
    clause.complete(SyntaxKind::ElseClause);
  }

  stmt.complete(SyntaxKind::IfStmt);
}

void Parser::parseWhileStmt() {
  Marker stmt = start();
  bump(); // `while`
  parseExpr();
  parseBlock();
  stmt.complete(SyntaxKind::WhileStmt);
}

// `for [init]; [cond]; [step] { body }`, with the clauses optionally wrapped in
// parentheses. The parentheses are consumed by the `for` itself rather than
// being part of an expression: the clauses are separated by `;`, so a `(` at
// that position can only be opening the group.
void Parser::parseForStmt() {
  Marker stmt = start();
  bump(); // `for`

  const bool parenthesized = at(lex::TokenKind::LParen);
  if (parenthesized) {
    bump();
  }

  parseForInit();
  parseForClause(SyntaxKind::ForCondition);
  expect(lex::TokenKind::Semicolon);
  parseForClause(SyntaxKind::ForStep);

  if (parenthesized) {
    expect(lex::TokenKind::RParen);
  }

  parseBlock();
  stmt.complete(SyntaxKind::ForStmt);
}

// The initializer is a statement: a new binding, an assignment, or nothing.
// Each case consumes the `;` that separates it from the condition, so the
// caller does not have to know which one it got.
void Parser::parseForInit() {
  if (at(lex::TokenKind::Semicolon)) {
    Marker empty = start();
    bump();
    empty.complete(SyntaxKind::EmptyStmt);
    return;
  }
  if (at(lex::TokenKind::KwLet) || at(lex::TokenKind::KwConst)) {
    const bool isConst = at(lex::TokenKind::KwConst);
    Marker binding = start();
    parseBinding();
    expect(lex::TokenKind::Semicolon);
    binding.complete(isConst ? SyntaxKind::ConstStmt : SyntaxKind::LetStmt);
    return;
  }
  Marker exprStmt = start();
  parseExpr();
  expect(lex::TokenKind::Semicolon);
  exprStmt.complete(SyntaxKind::ExprStmt);
}

void Parser::parseForClause(SyntaxKind wrapper) {
  Marker clause = start();
  // A clause is omitted by leaving it out, not by writing something: `for ;; {}`
  // is an infinite loop, and the empty slot is a node so that "is there a
  // condition?" is a child count and not a guess.
  if (!at(lex::TokenKind::Semicolon) && !at(lex::TokenKind::RParen) &&
      !at(lex::TokenKind::LBrace) && !atEnd() && !bailedOut_) {
    parseExpr();
  }
  clause.complete(wrapper);
}

void Parser::parseJumpStmt(SyntaxKind kind) {
  Marker stmt = start();
  bump(); // `break` or `continue`
  expect(lex::TokenKind::Semicolon);
  stmt.complete(kind);
}

void Parser::parseLetStmt(bool isConst) {
  Marker stmt = start();
  parseBinding();
  expect(lex::TokenKind::Semicolon);
  stmt.complete(isConst ? SyntaxKind::ConstStmt : SyntaxKind::LetStmt);
}

void Parser::parseBinding() {
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
    // A `{` here is the C habit of bracing the initializer -- `let a: [3]i32 =
    // {1, 2, 3};` -- and it is the one position where the reading is not
    // ambiguous: after `=` there is no block, so the token has exactly one
    // meaning and the sentence about which grouping is which can be said. (The
    // general expression position has no such case on purpose: `if { b(); }` is a
    // missing condition followed by the block, and answering it with a sentence
    // about braces would turn one mistake into four messages.)
    if (at(lex::TokenKind::LBrace)) {
      parseBraceGroup();
    } else {
      parseExpr();
    }
  }
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
