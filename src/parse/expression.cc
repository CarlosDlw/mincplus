// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Expressions, by precedence climbing over the one table in token_class.h.
//
// Left-associative chains are built left to right: `a - b - c` parses `a - b`
// first, then a new node adopts it and takes `- c`. The adoption is a link on
// the event stream (`CompletedMarker::precede`), so no node is ever moved and
// the tree is never re-walked.
//
// Every production that can call itself -- directly, or through a cycle that
// does not pass back through `parseExpr` -- takes a `DepthGuard`. Which ones
// those are is not guesswork: `-` 200000 times recurses `parseUnary`,
// `a = a = ...` recurses `parseAssign`, `a ? b : a ? b : ...` recurses
// `parseConditional`, and none of them goes through `parseExpr` on the way, so
// guarding only the entry point leaves an input that overflows the stack on
// Windows' 1 MiB thread stack. `parseBinary` needs no guard: its recursion is
// bounded by the number of precedence levels, which is a fixed 16.
#include "parse/parser.h"

#include <cstdint>

#include "token_class.h"

namespace minc::parse {

CompletedMarker Parser::recursionLimitError() {
  tooDeep();
  Marker bad = start();
  return bad.complete(SyntaxKind::Error);
}

void Parser::parseExpr() {
  DepthGuard depth(*this);
  if (!depth.ok()) {
    // This entry point has nothing to hand the node back to; the marker still
    // closes an `Error` node so the tree stays total.
    static_cast<void>(recursionLimitError());
    return;
  }
  parseAssign();
}

CompletedMarker Parser::parseAssign() {
  DepthGuard depth(*this);
  if (!depth.ok()) {
    return recursionLimitError();
  }

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
  DepthGuard depth(*this);
  if (!depth.ok()) {
    return recursionLimitError();
  }

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
  DepthGuard depth(*this);
  if (!depth.ok()) {
    return recursionLimitError();
  }

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
    } else if (at(lex::TokenKind::LBracket)) {
      // `a[i]`. Bracketed rather than a `PostfixExpr` because the index is a
      // full expression of its own, not the single operand an operator token
      // implies -- and because the brackets have to stay in the tree: `a[i]` and
      // `a i` are not the same program, and a dump that could not tell them
      // apart would not be a dump of the source.
      Marker index = expr.precede();
      bump(); // `[`
      parseExpr();
      // `a[1..2]` -- the range a future slice is taken with. Refused by name and
      // not left to the `expected ']'` two tokens later, which is what the `..`
      // used to produce: that message is about the *bracket* and says nothing
      // about the operator that is reserved. The second bound is read anyway, so
      // the subscript closes and the reader gets one sentence instead of two
      // (`arrays.md` step 10, decision 17).
      if (at(lex::TokenKind::Dot) && nth(1) == lex::TokenKind::Dot) {
        error("`..` is reserved for slices: `a[1..2]` has no value today, and `a[i]` is an "
              "element of the array",
              ParseErrorCode::ReservedRange);
        bump(); // `.`
        bump(); // `.`
        if (!at(lex::TokenKind::RBracket) && !atEnd() && !bailedOut_) {
          parseExpr();
        }
      }
      expect(lex::TokenKind::RBracket);
      expr = index.complete(SyntaxKind::IndexExpr);
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
  case lex::TokenKind::LBracket: {
    // `[` opens both literal forms, and the one thing that tells them apart is
    // what comes after the group (see `atTypedInitializer`). Decided here, once,
    // so nothing downstream has to guess which of the two it was handed.
    return atTypedInitializer() ? parseTypedInitializer() : parseArrayLiteral();
  }
  // **No `LBrace` case here, deliberately.** A `{` where a primary expression
  // would start is ambiguous and the common reading is not the mistaken one:
  // `if { b(); }` is a missing condition followed by the block. The sentence for
  // braces around a value therefore lives where a value is *required* and a block
  // cannot follow -- an element of an initializer (`parseElement`) and the value
  // of an annotated binding (`statement.cc`) -- and not at the general position,
  // where it would turn one missing condition into four messages.
  default: {
    error("expected an expression", ParseErrorCode::ExpectedExpression);
    // An empty `Error` node keeps the tree total without consuming anything;
    // the enclosing statement's progress rule takes care of moving on.
    Marker missing = start();
    return missing.complete(SyntaxKind::Error);
  }
  }
}

// The tokens a type is built from: a word, a `*`, or a `!`. `[N]` is the fourth
// and is a *group*, which is why the scan below walks it as one.
[[nodiscard]] static bool isTypeToken(lex::TokenKind kind) {
  return kind == lex::TokenKind::Identifier || kind == lex::TokenKind::Star ||
         kind == lex::TokenKind::Bang;
}

bool Parser::atTypedInitializer() const {
  // Precondition: the current token is `[`.
  //
  // `[...]` is the *count* of a typed initializer when the group is exactly
  // `[N]` (or `[_]`) and what follows is a type run and then `{`. That is a scan
  // to the `{` rather than a fixed amount of lookahead, because a type run is as
  // long as its words: `[2][3]unsigned long long int{...}` is twelve tokens of
  // type before the brace. The scan is bounded by the run, and a run ends at the
  // first token that cannot be part of a type -- which in a real program is a
  // handful. Nothing else in this grammar puts `{` after type tokens, so the two
  // readings of a `[...]` group can never both be valid (`arrays.md`).
  if (nth(0) != lex::TokenKind::LBracket) {
    return false;
  }
  const lex::TokenKind counted = nth(1);
  if ((counted != lex::TokenKind::IntegerLiteral && counted != lex::TokenKind::Identifier) ||
      nth(2) != lex::TokenKind::RBracket) {
    return false;
  }
  std::uint32_t i = 3;
  while (true) {
    if (isTypeToken(nth(i))) {
      ++i;
      continue;
    }
    if (nth(i) != lex::TokenKind::LBracket) {
      break;
    }
    // A `[N]` group, walked as **one** step of the run. Stepping token by token
    // would stop on the count's `IntegerLiteral` -- not a type token -- and so
    // would fail to see the `{` past `[2][3]i32`, which is exactly the shape the
    // nested typed initializer is written with. Malformed groups are walked as
    // far as they hold together: this is a scan to the `{`, and the *reader* is
    // what refuses the group (`sema/typespec.cc`).
    ++i; // `[`
    if (nth(i) == lex::TokenKind::IntegerLiteral ||
        (nth(i) == lex::TokenKind::Identifier && text(i) == kInferredCount)) {
      ++i;
    }
    if (nth(i) == lex::TokenKind::RBracket) {
      ++i;
    }
  }
  return nth(i) == lex::TokenKind::LBrace;
}

void Parser::parseInitializerElements(lex::TokenKind closer) {
  // The elements of `{...}` or `[...]`: a list separated by `,`, with a trailing
  // comma allowed, or `value ; count`, the fill. The `;` is the only thing that
  // tells the two apart, so both are read here and a one-element list is not a
  // special case. An empty group is left empty for the reader to refuse: the
  // parser answers "what shape is written", and what a shape *means* -- that a
  // zero-element array has no spelling -- is a rule with a sentence.
  if (at(closer)) {
    return;
  }
  parseElement();
  if (at(lex::TokenKind::Semicolon)) {
    bump();
    parseExpr();
    return;
  }
  while (at(lex::TokenKind::Comma) && !bailedOut_) {
    bump();
    if (at(closer)) {
      break; // the trailing comma
    }
    parseElement();
  }
}

void Parser::parseElement() {
  // One element of an initializer, in either grouping.
  //
  // A `{` here is C's nested-braces spelling, and it is not this language's: the
  // braces belong to a *type* (`[3]i32{1, 2, 3}`) and a group of elements is a
  // value, written with the same brackets its type is (`[1, 2, 3]`). Leaving it to
  // `parseExpr` would answer with "expected an expression" at a token that starts
  // an expression everywhere else, and the reader would be looking for a missing
  // operand rather than at the two characters to swap.
  //
  // The group is read anyway, and as the `ArrayLiteral` it was meant to be: one
  // refusal, and a tree whose shape still says what the reader wrote, so the
  // checker does not add a second complaint about an element it cannot type.
  if (!at(lex::TokenKind::LBrace)) {
    parseExpr();
    return;
  }
  parseBraceGroup();
}

CompletedMarker Parser::parseBraceGroup() {
  // One sentence for both positions a stray `{` can appear in -- an element, and
  // a whole value -- because it is one mistake: the braces of an initializer
  // belong to its *type* (`[3]i32{1, 2, 3}`), and a group of elements is a value,
  // written with the brackets its type is (`[1, 2, 3]`).
  error("`{` comes after a type, as in `[3]i32{1, 2, 3}`: a group of elements is a value, and "
        "it is written with brackets -- `[1, 2, 3]` -- or the binding gets the type and the "
        "group needs none",
        ParseErrorCode::BraceWithoutType);
  // Read as the `ArrayLiteral` it was meant to be. One refusal, and a tree whose
  // shape still says what the reader wrote: an annotated binding then types it,
  // and the checker has nothing to add about an element it cannot name.
  Marker group = start();
  bump(); // `{`
  parseInitializerElements(lex::TokenKind::RBrace);
  expect(lex::TokenKind::RBrace);
  return group.complete(SyntaxKind::ArrayLiteral);
}

CompletedMarker Parser::parseArrayLiteral() {
  Marker literal = start();
  bump(); // `[`
  parseInitializerElements(lex::TokenKind::RBracket);
  expect(lex::TokenKind::RBracket);
  return literal.complete(SyntaxKind::ArrayLiteral);
}

CompletedMarker Parser::parseTypedInitializer() {
  Marker initializer = start();
  // `[N]T` is one `Type` node, count and all -- the same `parseType` a binding
  // annotation uses, so a typed initializer cannot spell a type differently from
  // the rest of the language.
  parseType();
  bump(); // `{`, guaranteed by `atTypedInitializer`
  parseInitializerElements(lex::TokenKind::RBrace);
  expect(lex::TokenKind::RBrace);
  return initializer.complete(SyntaxKind::TypedInitializer);
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
