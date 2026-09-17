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
#include <string>

#include "support/typenames/type_name.h"
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
  // The `as` level, and where it sits is the whole of its grammar: **above every
  // binary operator and below the prefix ones**, which is Rust's precedence and
  // C's for a cast.
  //
  //   a as i64 * 2      is  (a as i64) * 2   -- the multiplication is one level up
  //   -a as i64         is  (-a) as i64      -- the prefix chain below took `-a`
  //   a as i32 as i64   is  (a as i32) as i64 -- left-associative, so chains read
  //                                             the way they were written
  //
  // The loop is iterative and the recursion is in `parsePrefix`, so a chain of
  // ten thousand casts costs no depth at all.
  CompletedMarker expr = parsePrefix();
  while (!bailedOut_ && at(lex::TokenKind::KwAs)) {
    Marker cast = expr.precede();
    bump(); // `as`
    parseCastType();
    expr = cast.complete(SyntaxKind::CastExpr);
  }
  return expr;
}

CompletedMarker Parser::parsePrefix() {
  DepthGuard depth(*this);
  if (!depth.ok()) {
    return recursionLimitError();
  }

  if (isPrefixOperator(current())) {
    Marker prefix = start();
    bump(); // the operator
    // Right-associative, so `--x`, `- -x` and `!!x` nest correctly -- and a
    // *prefix* level and not `parseUnary`, which is what makes `-a as i64` bind
    // the cast outside the negation.
    parsePrefix();
    return prefix.complete(SyntaxKind::PrefixExpr);
  }
  return parsePostfix();
}

void Parser::parseCastType() {
  Marker type = start();
  // Constructors first, whole groups, then the words -- the shape a type
  // position has (`typespec.h`). The difference from `parseType` is where it
  // stops: after the words, nothing more is part of the type. `a as i32 * 2` is
  // a multiplication, `a as *u8` is a pointer, and a run that kept going would
  // build `i32 *` -- which is a `*` after the words, the one spelling the type
  // reader refuses by name.
  while (!atEnd() && !bailedOut_) {
    if (at(lex::TokenKind::Star) || at(lex::TokenKind::Bang)) {
      bump();
      continue;
    }
    if (at(lex::TokenKind::LBracket)) {
      parseArrayCount();
      continue;
    }
    break;
  }
  if (!at(lex::TokenKind::Identifier)) {
    // `x as 1` and `x as`, both one mistake: no type was written.
    error("expected a type after `as`", ParseErrorCode::ExpectedType);
    type.complete(SyntaxKind::Type);
    return;
  }
  while (at(lex::TokenKind::Identifier) && !bailedOut_) {
    bump();
  }
  type.complete(SyntaxKind::Type);
}

bool Parser::atCastStart() const {
  // Precondition: the current token is `(`. The decision is two lexical
  // questions and **no symbol table**, which is the whole reason this form is in
  // the language (`casts.md`, decision 4):
  //
  //   1. is the run inside the parentheses a *complete type* -- constructors,
  //      then one or more **reserved type names**, then `)`;
  //   2. does the token after `)` start an expression.
  //
  // Question 1 asks `support::isTypeNameWord` rather than "is it an identifier",
  // and that is what makes `(x) + 1` for a variable `x` the parenthesised
  // expression it looks like: a word that is a type name can never be declared, so
  // it can never be a variable either.
  std::uint32_t i = 1;
  bool sawWord = false;
  while (true) {
    const lex::TokenKind kind = nth(i);
    if (!sawWord && (kind == lex::TokenKind::Star || kind == lex::TokenKind::Bang)) {
      ++i;
      continue;
    }
    if (kind == lex::TokenKind::Comma) {
      // `(i32, bool)`: a **product** type, which the scan accepts so the refusal
      // that names it can fire at the call site (`tuples.md`, decision 19). What
      // is not accepted is a cast to one -- the group is a complete type followed
      // by an expression, and the only thing missing from it is the fact that a
      // cast names one type.
      ++i;
      continue;
    }
    if (!sawWord && kind == lex::TokenKind::LBracket) {
      // A `[N]` or `[]` group, walked whole. The count is left to the type
      // reader: `[x]i32` is refused by the answer with a sentence, and a scan
      // that tried to judge it here would be a second copy of that rule.
      ++i;
      if (nth(i) == lex::TokenKind::IntegerLiteral || nth(i) == lex::TokenKind::Identifier) {
        ++i;
      }
      if (nth(i) == lex::TokenKind::RBracket) {
        ++i;
      }
      continue;
    }
    if (kind != lex::TokenKind::Identifier || !support::isTypeNameWord(text(i))) {
      break;
    }
    sawWord = true;
    ++i;
  }
  if (!sawWord || nth(i) != lex::TokenKind::RParen) {
    return false;
  }
  // `(i32)` with nothing after it that starts an expression is *not* a cast: it
  // is a parenthesised type name, and the sentence for it comes from the stage
  // that can say "a type is not a value; did you mean to cast?".
  return isExpressionStart(nth(i + 1));
}

bool Parser::atProductCastStart() const {
  // Precondition: `atCastStart()` is true, so what is ahead is a complete type run
  // in parentheses and an expression after it. This asks the one further question
  // -- is the type a *product* -- by looking for a comma at the group's own level.
  // A comma inside a member's nested group is that member's business, which is
  // what the depth counter is for.
  std::uint32_t depth = 0;
  for (std::uint32_t i = 0;; ++i) {
    const lex::TokenKind kind = nth(i);
    if (kind == lex::TokenKind::EndOfFile) {
      return false;
    }
    if (kind == lex::TokenKind::LParen) {
      ++depth;
      continue;
    }
    if (kind == lex::TokenKind::RParen) {
      --depth;
      if (depth == 0) {
        return false;
      }
      continue;
    }
    if (kind == lex::TokenKind::Comma && depth == 1) {
      return true;
    }
  }
}

CompletedMarker Parser::parseProductCastRefusal() {
  // `(i32, bool)x`: one diagnostic, at the group, and a tree that holds the whole
  // expression -- the group's tokens and the operand -- so the recovery does not
  // hand the next rule a token that belongs to this one. The parser is the stage
  // that can see the cast *shape*; the sentence names what to write instead
  // (`tuples.md`, decision 19).
  Marker cast = start();
  error("a cast names one type, and `(T, U)` is a product: take the value whole, or cast each "
        "member where it is used",
        ParseErrorCode::CastToProduct);
  std::uint32_t depth = 0;
  while (!atEnd() && !bailedOut_) {
    const lex::TokenKind kind = current();
    bump();
    if (kind == lex::TokenKind::LParen) {
      ++depth;
      continue;
    }
    if (kind == lex::TokenKind::RParen) {
      --depth;
      if (depth == 0) {
        break;
      }
    }
  }
  parsePrefix();
  return cast.complete(SyntaxKind::CastExpr);
}

CompletedMarker Parser::parseTupleLiteral(Marker group) {
  // Precondition: `(` and the first element have been read; the current token is
  // `,`. The commas stay in the tree -- they are what tells a product from a
  // group, and a reader of the tree has to be able to see where an element ended.
  std::uint32_t elements = 1;
  while (at(lex::TokenKind::Comma)) {
    bump(); // `,`
    if (at(lex::TokenKind::RParen)) {
      // `(a,)`: the trailing comma, which is the writing of a one-member product.
      // Kept in the tree and refused below, because both halves -- the shape and
      // the sentence -- belong to the stage that can see the comma (`tuples.md`,
      // decision 2).
      break;
    }
    if (!isExpressionStart(current())) {
      error("expected the value of this member: a product is written `(a, b)`",
            ParseErrorCode::ExpectedExpression);
      break;
    }
    parseExpr();
    ++elements;
  }
  expect(lex::TokenKind::RParen);
  if (elements < 2) {
    error("a product of one value is that value: write the value, or give the product a second "
          "member",
          ParseErrorCode::ExpectedExpression);
  }
  return group.complete(SyntaxKind::TupleExpr);
}

CompletedMarker Parser::parseCastPrefix() {
  Marker cast = start();
  bump(); // `(`
  parseCastType();
  expect(lex::TokenKind::RParen);
  // The operand is a *prefix* expression, so the `as` level stays outside the
  // cast: `(i32)a as i64` is `((i32)a) as i64`.
  parsePrefix();
  return cast.complete(SyntaxKind::CastExpr);
}

bool Parser::atLiteralSuffixRun() const {
  if (!lex::isLiteral(current()) || nth(1) != lex::TokenKind::Identifier) {
    return false;
  }
  const support::Span here = spanOf(0);
  const support::Span next = spanOf(1);
  return here.file == next.file && here.end == next.begin;
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
      // `a[i]` and `a[1..2]` -- one bracket, two expressions, and the `..`
      // decides which. Bracketed rather than a `PostfixExpr` because the index
      // is a full expression of its own, not the single operand an operator
      // token implies -- and because the brackets have to stay in the tree:
      // `a[i]` and `a i` are not the same program, and a dump that could not
      // tell them apart would not be a dump of the source.
      Marker index = expr.precede();
      bump(); // `[`
      // Three cases, and each one is a *complete* tree: the operand before the
      // `..`, the operand after it, or the index that makes this an `IndexExpr`.
      // Nothing is left half-consumed, so the single `]` below closes whichever
      // of the three was opened (`slices.md` decision 8).
      if (at(lex::TokenKind::Dot) && nth(1) == lex::TokenKind::Dot) {
        // `a[..r]` and `a[..]`: no first bound.
        bump(); // `.`
        bump(); // `.`
        if (!at(lex::TokenKind::RBracket) && !atEnd() && !bailedOut_) {
          parseExpr();
        }
        expect(lex::TokenKind::RBracket);
        expr = index.complete(SyntaxKind::SliceExpr);
      } else {
        parseExpr();
        if (at(lex::TokenKind::Dot) && nth(1) == lex::TokenKind::Dot) {
          bump(); // `.`
          bump(); // `.`
          // `a[l..r]` and `a[l..]`: the second bound is optional, and its absence
          // is written, not inferred -- `a[l..]` and `a[l]` are different
          // programmes and the `..` is what says which one was typed.
          if (!at(lex::TokenKind::RBracket) && !atEnd() && !bailedOut_) {
            parseExpr();
          }
          expect(lex::TokenKind::RBracket);
          expr = index.complete(SyntaxKind::SliceExpr);
        } else {
          expect(lex::TokenKind::RBracket);
          expr = index.complete(SyntaxKind::IndexExpr);
        }
      }
    } else if (at(lex::TokenKind::Dot) && nth(1) != lex::TokenKind::Dot) {
      // `t.0`, and `s.field` when `struct` lands: one node for a component of a
      // value (`tuples.md`, decision 16). A `.` **followed by another `.`** is not
      // this node: inside brackets, `a[1..2]` is the range the slice reads, and the
      // index reader that owns it asks for the two dots by hand.
      Marker field = expr.precede();
      bump(); // `.`
      if (at(lex::TokenKind::IntegerLiteral) || at(lex::TokenKind::Identifier)) {
        bump();
      } else if (at(lex::TokenKind::FloatLiteral) && text(0).find('.') != 0) {
        // **A member chain that the scanner glued into one number.** `t.0.1` is two
        // member reads, and the scanner sees `0.1` -- a fraction, because a `.`
        // followed by a digit belongs to the number before it. The two readings are
        // indistinguishable *as a token*, and this is the one place the language
        // pays for spelling a member with a dot: the chain has to be separated by
        // something the scanner can see. Named here with both fixes rather than left
        // as "expected a member", which teaches nothing about the character to add.
        error("`" + std::string(text(0)) +
                  "` is one number, so this is not two member reads: write the second member "
                  "apart from the first, as `t.0 .1` or `(t.0).1`",
              ParseErrorCode::ExpectedName);
        consumeAsError();
      } else if (!bailedOut_) {
        // A `.` with nothing to read: the member is the one token the node exists
        // for, so it is named here rather than left to whatever expected the
        // expression to end. A token that ends the expression instead of starting a
        // member (`;`, `)` ...) is *not* consumed: the statement's own "expected `;`"
        // is the true sentence for a dot written at the end of one.
        error("expected a member after `.`: a position (`t.0`) or a field name",
              ParseErrorCode::ExpectedName);
        if (!atExpressionEnd()) {
          consumeAsError();
        }
      }
      expr = field.complete(SyntaxKind::FieldExpr);
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
    // `10z`, `1.5x`, `'a'u8`: a literal with a name written against it. The
    // scanner claims a trailing run only when the run is a suffix the language
    // knows, so what is left here is the literal and the name as two tokens --
    // and two tokens with nothing between them are one mistake, not a missing
    // operator. The run is consumed inside the literal's node so the statement
    // after it still ends where the reader ended it.
    if (atLiteralSuffixRun()) {
      const std::string spelling(text(0));
      const std::string run(text(1));
      const bool numeric = at(lex::TokenKind::IntegerLiteral) || at(lex::TokenKind::FloatLiteral);
      error(numeric ? "`" + spelling + run +
                          "`: a literal and a name may not be written together, and `" + run +
                          "` is not a suffix this language has -- write the conversion (`" +
                          spelling + " as i64`), or a space if two tokens were meant"
                    : "a suffix on a literal says nothing here: `" + spelling +
                          "` has one type already, and the conversion is written `" + spelling +
                          " as ` followed by the type",
            ParseErrorCode::InvalidLiteralSuffix);
      Marker junk = start();
      bump();
      junk.complete(SyntaxKind::Error);
    }
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
    // `(i32)x` and `(x) + 1` are one token apart and two different programs, and
    // the difference is decided here, once, by `atCastStart`. A `(T, U)` group is
    // a *third* reading, and the one refusal it earns is named here rather than
    // left to the cast path (`tuples.md`, decision 19).
    if (atCastStart()) {
      return atProductCastStart() ? parseProductCastRefusal() : parseCastPrefix();
    }
    // A group, a product, or nothing at all. The first expression is parsed either
    // way and the `,` that follows it is what decides -- a comma is not an
    // expression token in this grammar, so nothing is looked at twice and nothing
    // is backtracked (`tuples.md`, decision 7).
    if (nth(1) == lex::TokenKind::RParen) {
      Marker empty = start();
      bump(); // `(`
      bump(); // `)`
      error("`()` is not a value: a call with no arguments and an empty parameter list are both "
            "written `()`, and a function that returns nothing says `void`",
            ParseErrorCode::ExpectedExpression);
      return empty.complete(SyntaxKind::TupleExpr);
    }
    Marker group = start();
    bump(); // `(`
    parseExpr();
    if (at(lex::TokenKind::Comma)) {
      return parseTupleLiteral(group);
    }
    expect(lex::TokenKind::RParen);
    return group.complete(SyntaxKind::ParenExpr);
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
  case lex::TokenKind::Dot: {
    // `.5`: a number whose token began with the point. The lexer stopped reading
    // one when member access arrived -- a `.` after a value is `t.0` (`tuples.md`)
    // -- so this is the sentence for the spelling that used to be a float, and it
    // is here rather than in the scanner because it names the fix a reader of a
    // *program* needs. The number is consumed, so one slip is one diagnostic.
    Marker number = start();
    bump(); // `.`
    if (at(lex::TokenKind::IntegerLiteral)) {
      bump();
    }
    error("a number begins with a digit: `.5` is written `0.5` -- a `.` after a value reads a "
          "member of it (`t.0`)",
          ParseErrorCode::LeadingPointNumber);
    return number.complete(SyntaxKind::Error);
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
  // Three ways to open the group, and the third is the empty one: `[N]`, `[_]`,
  // and `[]`. `[]i32{...}` is a *slice literal*, which the language does not have
  // -- and reading it here is what makes that a sentence instead of a parse
  // error: the constructor is built, and the type reader one stage down is the
  // one that can say why a view has no literal (`slices.md` decision 7). A `[`
  // followed by anything else is *not* this path, and stays the array reader's
  // problem.
  std::uint32_t i = 0;
  if (counted == lex::TokenKind::RBracket) {
    i = 2; // `[]`, closed as soon as it opened
  } else if (counted == lex::TokenKind::IntegerLiteral || counted == lex::TokenKind::Identifier) {
    if (nth(2) != lex::TokenKind::RBracket) {
      return false;
    }
    i = 3;
  } else {
    return false;
  }
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
