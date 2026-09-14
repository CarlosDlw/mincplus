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

// `fn Type Name(...) Block`, and the declaration form `extern fn Type
// Name(...);`.
//
// **One node kind, not two.** The two forms have the same shape and differ in
// one word and in whether a body follows, so `FnDecl` covers both exactly as
// `VariableStmt` covers `let` and `const`: a consumer asks `bodyOf` and gets an
// empty answer for a declaration instead of trying two casts. The `extern` token
// stays a child of the node, so the tree is still lossless and the word a reader
// wrote is still in it.
void Parser::parseFnDecl(bool isExtern) {
  Marker decl = start();
  if (isExtern) {
    bump(); // `extern`
  }
  expect(lex::TokenKind::KwFn);
  parseTypeAndName();
  expect(lex::TokenKind::LParen);
  parseParamList(/*allowVariadic=*/isExtern);
  expect(lex::TokenKind::RParen);
  parseFunctionTail(isExtern);
  decl.complete(SyntaxKind::FnDecl);
}

// The body, or the `;` that stands in for it.
//
// `extern` is what decides which one is required, and both directions of getting
// it wrong are reported *here*, because both are grammar and not semantics: the
// parser is the stage that has the word and the token. The wrong form is still
// built into the tree -- a body after `extern` is parsed as a body, a missing
// body leaves the declaration without one -- so one mistake produces one
// diagnostic rather than a cascade from every stage below.
void Parser::parseFunctionTail(bool isExtern) {
  if (isExtern) {
    if (!at(lex::TokenKind::LBrace)) {
      expect(lex::TokenKind::Semicolon);
      return;
    }
    error("`extern` declares a function that is defined elsewhere, so it has no body",
          ParseErrorCode::ExternWithBody);
    parseBlock();
    return;
  }

  if (at(lex::TokenKind::LBrace)) {
    parseBlock();
    return;
  }

  // No body and no `extern`. The `;` is consumed when it is there, so the slip
  // that produced this -- a declaration written without the word that makes it
  // one -- costs one diagnostic and not a second "expected ';'" on top of it.
  error("a function with no body is a declaration; write `extern fn`",
        ParseErrorCode::MissingExtern);
  if (at(lex::TokenKind::Semicolon)) {
    bump();
  }
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
void Parser::parseParamList(bool allowVariadic) {
  Marker params = start();
  bool hasParameter = false;
  while (!at(lex::TokenKind::RParen) && !atEnd() && !bailedOut_) {
    if (at(lex::TokenKind::Ellipsis)) {
      parseVariadicMarker(allowVariadic, hasParameter);
      // `...` *ends* the list by definition, so the loop stops here whether or
      // not the reader wrote more after it. Anything that follows was already
      // reported as the marker being in the wrong place, and the marker's own
      // recovery consumed it, which is what keeps `)` from adding a second
      // diagnostic about the same slip.
      break;
    }
    parseParam();
    hasParameter = true;
    if (!at(lex::TokenKind::Comma)) {
      break;
    }
    bump();
    // The loop condition runs again, which is how a trailing comma before `)`
    // ends the list instead of starting another parameter.
  }
  params.complete(SyntaxKind::ParamList);
}

// `...`, the variadic marker.
//
// Three rules, all of them grammar, and all of them reported here because this
// is the stage holding the token and the word:
//
//   * only a declaration may be variadic (`extern fn`), because *reading* the
//     arguments needs `va_start`, which the language does not have;
//   * at least one parameter has to come before it, so a call has something to
//     check its arguments against;
//   * it comes last, because it is what makes the arguments after the named ones
//     un-specified.
//
// The marker is built whatever happened, so the tree says the list is variadic
// even when the words around it were wrong: one mistake, one diagnostic, and
// every byte still in a node.
void Parser::parseVariadicMarker(bool allowVariadic, bool hasParameter) {
  Marker marker = start();
  bump(); // `...`

  if (!allowVariadic) {
    error("`...` needs a declaration: reading a variadic argument needs `va_start`, so only "
          "`extern fn` can be variadic",
          ParseErrorCode::VariadicDefinition);
  } else if (!hasParameter) {
    error("`...` needs a parameter before it, so a call has something to check its arguments "
          "against",
          ParseErrorCode::VariadicPosition);
  } else if (at(lex::TokenKind::Comma)) {
    error("`...` must be the last thing in the parameter list", ParseErrorCode::VariadicPosition);
  }

  if (at(lex::TokenKind::Comma)) {
    // The tail belongs to no parameter, and leaving it for `)` would report one
    // slip twice. It is consumed as junk for that reason and not for tidiness.
    Marker junk = start();
    while (!at(lex::TokenKind::RParen) && !atEnd() && !bailedOut_) {
      bump();
    }
    junk.complete(SyntaxKind::Error);
  }

  marker.complete(SyntaxKind::VariadicParam);
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
         parser.nth(tokens) == lex::TokenKind::Identifier ||
         // `!`, the bottom type. It is a type *token* rather than a word, which
         // is why it is listed beside the two the grammar already had: a run is
         // still what a type position holds, and `!` takes part in it exactly
         // where a word would -- `fn ! f()` is a return type and a name, and the
         // reader below splits the run the same way it splits `fn i32 f()`.
         parser.nth(tokens) == lex::TokenKind::Bang) {
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
    // Everything the run held goes into the `Type` node, `!` included: the run
    // is what the reader below would have read, and dropping a token here is how
    // a tree stops being lossless.
    while (!atEnd() && (at(lex::TokenKind::Star) || at(lex::TokenKind::Bang))) {
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
  // whole run is the type. `!` is accepted here too, and refused one stage later
  // where the position is known: this stage answers "what shape is written", and
  // `let x: !` is a shape -- a wrong one, with a sentence about why, produced by
  // the only stage that knows an object cannot have that type (`never.md`).
  while (at(lex::TokenKind::Identifier) || at(lex::TokenKind::Star) || at(lex::TokenKind::Bang)) {
    bump();
  }
  type.complete(SyntaxKind::Type);
}

} // namespace minc::parse
