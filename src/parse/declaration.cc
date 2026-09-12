// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Declarations: functions, and the type/name runs they are made of.
//
// Types are a *position*, not a token kind. `i32`, `int`, and the multi-word C
// spellings (`unsigned long long int`) all lex as identifiers, so a type is the
// run of identifiers that appears where a type is expected. That is the whole
// reason `.mx` needs no lexer feedback and no symbol table in the parser -- and
// it is why the run has to be delimited by position here, once, rather than
// guessed.
#include "parse/parser.h"

#include <cstdint>
#include <string>

namespace minc::parse {

void Parser::parseFnDecl() {
  Marker decl = start();
  expect(lex::TokenKind::KwFn);
  parseTypeAndName();
  expect(lex::TokenKind::LParen);
  parseParamList();
  expect(lex::TokenKind::RParen);
  parseBlock();
  decl.complete(SyntaxKind::FnDecl);
}

void Parser::parseParamList() {
  Marker params = start();
  // Parameter syntax is not decided yet (the annotation form `name: T` and the
  // C form `T name` are both plausible), so the parser accepts the empty list
  // and says so plainly rather than guessing one of them into the grammar.
  if (!at(lex::TokenKind::RParen) && !atEnd()) {
    error("function parameters are not supported yet", ParseErrorCode::UnsupportedParameters);
    Marker junk = start();
    while (!at(lex::TokenKind::RParen) && !atEnd()) {
      bump();
    }
    junk.complete(SyntaxKind::Error);
  }
  params.complete(SyntaxKind::ParamList);
}

void Parser::parseTypeAndName() {
  // The type and the name are both identifier runs, and the only token that
  // separates them from the rest of the declaration is `(`. So the last
  // identifier before `(` is the name and everything before it is the type.
  std::uint32_t words = 0;
  while (nth(words) == lex::TokenKind::Identifier) {
    ++words;
  }

  if (words == 0) {
    error("expected a return type and a function name", ParseErrorCode::ExpectedName);
    Marker type = start();
    type.complete(SyntaxKind::Type);
    Marker name = start();
    name.complete(SyntaxKind::Name);
    return;
  }

  if (words == 1) {
    // `fn name()` -- a name with no return type. Report the type, not the name,
    // because the name is the part that is definitely there.
    error("expected a return type before the function name", ParseErrorCode::ExpectedType);
    Marker type = start();
    type.complete(SyntaxKind::Type);
    Marker name = start();
    bump();
    name.complete(SyntaxKind::Name);
    return;
  }

  Marker type = start();
  for (std::uint32_t i = 0; i + 1 < words; ++i) {
    bump();
  }
  type.complete(SyntaxKind::Type);

  Marker name = start();
  bump();
  name.complete(SyntaxKind::Name);
}

void Parser::parseType() {
  Marker type = start();
  if (!at(lex::TokenKind::Identifier)) {
    error("expected a type", ParseErrorCode::ExpectedType);
    type.complete(SyntaxKind::Type);
    return;
  }
  // In an annotation (`x: T`) there is no trailing name to separate, so the
  // whole identifier run is the type.
  while (at(lex::TokenKind::Identifier)) {
    bump();
  }
  type.complete(SyntaxKind::Type);
}

} // namespace minc::parse
