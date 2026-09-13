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

// A parameter is a binding, and it is written exactly like one:
//
//   `x: i32`
//
// which is the same shape as `let x: i32`. One spelling for "a name with a
// type" in the whole language means one thing to learn, one node shape for
// every later stage to read, and one place for the diagnostic to point.
//
// The C order `i32 x` is deliberately *not* accepted, and the reason is not
// taste. A type is a run of identifiers and a name is an identifier, so
// `fn i32 f(unsigned long) { return long; }` -- a name forgotten -- is
// indistinguishable from `unsigned` named `long`: the type reader would quietly
// build `u32` and the parameter would be called `long`. The colon removes the
// guess. Everything between it and the `,`/`)` is the type, which is also what
// makes a future declarator (`x: *i32`, `buf: [8]u8`) delimited by the same two
// tokens instead of becoming a C declarator puzzle.
void Parser::parseParamList() {
  Marker params = start();
  while (!at(lex::TokenKind::RParen) && !atEnd() && !bailedOut_) {
    parseParam();
    if (!at(lex::TokenKind::Comma)) {
      break;
    }
    bump();
    // The loop condition runs again, which is how a trailing comma before `)`
    // ends the list instead of starting another parameter.
  }
  params.complete(SyntaxKind::ParamList);
}

void Parser::parseParam() {
  Marker param = start();
  // Progress is the loop's correctness condition, not a courtesy: a construct
  // this function does not consume would make `parseParamList` spin forever on
  // a token that cannot start a parameter. The span of the current token is the
  // witness -- if it did not move, the token is junk and is taken as such.
  const std::uint32_t before = currentSpan().begin;

  if (at(lex::TokenKind::Identifier) && nth(1) == lex::TokenKind::Colon) {
    Marker name = start();
    bump();
    name.complete(SyntaxKind::Name);
    bump(); // `:`
    parseType();
  } else {
    // Two ways to be here, and they are worth telling apart: one identifier run
    // with no colon is a type somebody forgot to name, while two or more is the
    // C argument order, which is a habit rather than a slip and is the one case
    // where the message can teach the syntax.
    const bool cOrder = at(lex::TokenKind::Identifier) && nth(1) == lex::TokenKind::Identifier;
    error(cOrder ? "parameters are written `name: type`, not `type name`"
                 : "expected a parameter written `name: type`",
          ParseErrorCode::ExpectedName);

    // Consumed as an `Error` node, not a `Type`: the run is not a type, and
    // spelling it as one would let the type reader report the same mistake a
    // second time. One mistake, one diagnostic, and the bytes still belong to
    // exactly one node.
    Marker junk = start();
    while (at(lex::TokenKind::Identifier)) {
      bump();
    }
    junk.complete(SyntaxKind::Error);
  }

  if (currentSpan().begin == before && !at(lex::TokenKind::Comma) && !at(lex::TokenKind::RParen) &&
      !atEnd() && !bailedOut_) {
    Marker junk = start();
    while (!at(lex::TokenKind::Comma) && !at(lex::TokenKind::RParen) && !atEnd()) {
      bump();
    }
    junk.complete(SyntaxKind::Error);
  }

  param.complete(SyntaxKind::Param);
}

// How many tokens a type run has, from the current position. A run is `*` and
// identifier tokens and nothing else, which is the same shape everything below
// reads and the same shape a `Type` node holds.
//
// The `*` is deliberately *not* given a meaning here. A pointer type is a
// sema question -- the parser does not know which words are type names and
// this stage is not allowed to know -- so the run is collected whole and the
// rules about where a `*` may sit live in `sema/typespec.cc`, which is the only
// place that can say "a pointer is written `*T`" and mean it.
[[nodiscard]] static std::uint32_t typeRunLength(const Parser& parser) {
  std::uint32_t tokens = 0;
  while (parser.nth(tokens) == lex::TokenKind::Star ||
         parser.nth(tokens) == lex::TokenKind::Identifier) {
    ++tokens;
  }
  return tokens;
}

void Parser::parseTypeAndName() {
  // The type and the name are both part of one run of `*` and identifiers, and
  // the only token that separates them from the rest of the declaration is `(`.
  // So the last identifier before `(` is the name and everything before it --
  // stars included -- is the type.
  const std::uint32_t tokens = typeRunLength(*this);
  std::uint32_t words = 0;
  std::uint32_t lastWord = 0;
  for (std::uint32_t i = 0; i < tokens; ++i) {
    if (nth(i) == lex::TokenKind::Identifier) {
      ++words;
      lastWord = i + 1; // one past the last identifier
    }
  }

  if (words == 1 && lastWord <= 1) {
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

  if (words == 0) {
    // Either nothing at all, or stars and no name (`fn *()`). Both are one
    // mistake: there is no function name. The stars still belong to a `Type`
    // node, because every byte of the source has to live somewhere and a
    // silently dropped `*` is how a tree stops being lossless.
    error(tokens == 0 ? "expected a return type and a function name" : "expected a function name",
          ParseErrorCode::ExpectedName);
    Marker type = start();
    while (!atEnd() && at(lex::TokenKind::Star)) {
      bump();
    }
    type.complete(SyntaxKind::Type);
    Marker name = start();
    name.complete(SyntaxKind::Name);
    return;
  }

  Marker type = start();
  for (std::uint32_t i = 0; i + 1 < lastWord; ++i) {
    bump();
  }
  type.complete(SyntaxKind::Type);

  Marker name = start();
  bump();
  name.complete(SyntaxKind::Name);
}

void Parser::parseType() {
  Marker type = start();
  if (typeRunLength(*this) == 0) {
    error("expected a type", ParseErrorCode::ExpectedType);
    type.complete(SyntaxKind::Type);
    return;
  }
  // In an annotation (`x: T`) there is no trailing name to separate, so the
  // whole run is the type.
  while (at(lex::TokenKind::Identifier) || at(lex::TokenKind::Star)) {
    bump();
  }
  type.complete(SyntaxKind::Type);
}

} // namespace minc::parse
