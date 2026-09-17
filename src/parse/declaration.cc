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

#include "token_class.h"

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
void Parser::parseFnDecl(bool isExtern, bool isStatic) {
  Marker decl = start();
  if (isStatic) {
    bump(); // `static`
  }
  if (isExtern) {
    bump(); // `extern`
  }
  // Both words answer one question -- who may see this name -- and they answer
  // it oppositely: `static` is this unit only, `extern` is defined elsewhere. A
  // declaration that says both is refused here, where both words are in hand,
  // rather than resolved to a winner one stage down.
  if (isStatic && isExtern) {
    error("`static` and `extern` are opposite: one says this unit only, the other says the "
          "definition is elsewhere",
          ParseErrorCode::ConflictingLinkage);
  }
  expect(lex::TokenKind::KwFn);
  parseTypeAndName();
  // `fn T identity<T>(v: T)`: the binders, after the name and before the
  // parameter list. The scan above stopped at the `<` on purpose and left it in
  // the stream, because the list is its own node and not part of the name.
  reportUnusedListClose(parseGenericParams());
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
    reportUnusedListClose(parseType());
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

// How many tokens a type run has, from the current position. A run is `*`,
// `[N]` and identifier tokens, which is the same shape everything below reads and
// the same shape a `Type` node holds.
//
// None of the three is given a meaning here. A pointer or an array type is a
// sema question -- the parser does not know which words are type names and this
// stage is not allowed to know -- so the run is collected whole and the rules
// about where a `*` or a `[N]` may sit live in `sema/typespec.cc`, which is the
// only place that can say "a pointer is written `*T`" and mean it.
//
// A *malformed* group -- `[` with no count, or with no `]` -- still joins the run
// as one token. The alternative, ending the run there, would leave the count and
// the bracket outside the `Type` node, so the group the parser reports on would
// not be the group the reader wrote and the tokens after it would be read as the
// element type. A tree that does not hold the source is a tree that cannot be
// diagnosed from.
// What the scan of one type run found. It answers the two questions the
// declaration reader has -- how many tokens the run occupies, and where its last
// *word* ends -- in one walk, because a second walk is a second rule about what a
// word is (`tuples.md`).
struct TypeRunScan {
  // Tokens the run occupies, from here.
  std::uint32_t tokens = 0;
  // Words at the *top level* of the run: an identifier, or a whole `(T, U)`
  // group. The identifiers **inside** a group are not words of this run, which is
  // what keeps `fn (i32, bool) f()` from reading `bool` (or `f`) as the wrong
  // word -- the name of a function is the last word of the run, and a product is
  // one word.
  std::uint32_t words = 0;
  // The index of the last word's **first token** -- the name of the declaration.
  // Everything before it is the type, and it is deliberately not "one past the
  // word": `identity<T>` is *one* word whose name is `identity`, and the list
  // that follows the identifier is the declaration's, not the word's.
  std::uint32_t nameStart = 0;
  // That last word was a `(T, U)` group and not an identifier: a run that ends
  // this way has no name in it, and the reader has to say so.
  bool lastWordIsGroup = false;
};

// One past the `>` that closes the argument list whose `<` is at `open`, walking
// the four spellings a closer has.
//
// The *rule* of closing -- which tokens close one list, and which close two -- is
// `Parser::closeList`'s, and this is the lookahead's copy of it, because a scan
// cannot parse. The two are held together by a test that runs every spelling
// through both: a drift here reads the name of a declaration from the wrong
// token, which is the one mistake this scan exists to prevent.
[[nodiscard]] static std::uint32_t skipTypeArgList(const Parser& parser, std::uint32_t open) {
  // `open` indexes the `<`, so the first iteration is what depth 1 means.
  std::uint32_t i = open;
  std::int32_t depth = 0;
  while (true) {
    const lex::TokenKind kind = parser.nth(i);
    if (kind == lex::TokenKind::EndOfFile) {
      // Unterminated. The reader below is the one that reports it, with the span
      // of the `<` to point at; the scan only has to stop somewhere.
      return i;
    }
    std::int32_t closed = 0;
    switch (kind) {
    case lex::TokenKind::Less:
      ++depth;
      break;
    case lex::TokenKind::Greater:
    case lex::TokenKind::GreaterEqual:
      closed = 1;
      break;
    case lex::TokenKind::GreaterGreater:
    case lex::TokenKind::GreaterGreaterEqual:
      closed = 2;
      break;
    default:
      break;
    }
    if (closed != 0) {
      depth -= closed;
      if (depth <= 0) {
        return i + 1;
      }
    }
    ++i;
  }
}

[[nodiscard]] static TypeRunScan scanTypeRun(const Parser& parser) {
  TypeRunScan run;
  std::uint32_t tokens = 0;
  while (true) {
    const lex::TokenKind kind = parser.nth(tokens);
    if (kind == lex::TokenKind::Star || kind == lex::TokenKind::Identifier ||
        // `!`, the bottom type. It is a type *token* rather than a word, which
        // is why it is listed beside the two the grammar already had: a run is
        // still what a type position holds, and `!` takes part in it exactly
        // where a word would -- `fn ! f()` is a return type and a name, and the
        // reader below splits the run the same way it splits `fn i32 f()`.
        kind == lex::TokenKind::Bang) {
      if (kind == lex::TokenKind::Identifier) {
        // The one place a *word* is counted, so "how many words" and "where the
        // name starts" cannot disagree.
        run.words += 1;
        run.nameStart = tokens;
        run.lastWordIsGroup = false;
        ++tokens;
        // `<...>`, the list attached to this word: **part of the word**, and the
        // reason `fn Vec<i32> f()` does not read `i32` as the name. In a
        // declaration a `<` after a word has no other reading -- the run ends at
        // the `(` of the parameter list, so there is no expression for it to be
        // a comparison of -- which is what lets the scan take it without asking
        // what is inside.
        if (parser.nth(tokens) == lex::TokenKind::Less) {
          tokens = skipTypeArgList(parser, tokens);
        }
        continue;
      }
      ++tokens;
      continue;
    }
    // A `(T, U)` product, walked as one balanced group: **one word**, whose
    // closing `)` is found by counting. Nothing inside is judged here -- the type
    // reader one stage down is the only place that decides what a member may be,
    // and a scan that tried to would be a second copy of that rule
    // (`tuples.md`, decision 15).
    //
    // Two conditions, and both are about `fn`, which is the only thing this scan
    // is for. A `(` continues the run **only at its start** (`run.words == 0`),
    // because after a word the `(` is the *parameter list*: `fn i32 main(` has its
    // type and its name behind it, and reading the list as a product would make
    // `main` the return type. And the group must be non-empty, so `fn *(` is still
    // the missing name it was before a product existed rather than an empty
    // group that reads as a type.
    if (kind == lex::TokenKind::LParen && run.words == 0 &&
        parser.nth(tokens + 1) != lex::TokenKind::RParen) {
      std::uint32_t depth = 0;
      while (true) {
        const lex::TokenKind in = parser.nth(tokens);
        // Past the end, `nth` answers the end-of-file token (the source clamps),
        // so an unterminated group ends the run here and the group's own reader
        // reports the missing `)`.
        if (in == lex::TokenKind::EndOfFile) {
          break;
        }
        ++tokens;
        if (in == lex::TokenKind::LParen) {
          ++depth;
          continue;
        }
        if (in == lex::TokenKind::RParen) {
          --depth;
          if (depth == 0) {
            break;
          }
        }
      }
      run.words += 1;
      // The group is one word and this is the token after it. When nothing
      // follows, `lastWordIsGroup` is true and the reader reports the missing
      // name instead of reading this index.
      run.nameStart = tokens;
      run.lastWordIsGroup = true;
      continue;
    }
    // `[N]`, `[]`, or the bracket alone.
    if (parser.nth(tokens) == lex::TokenKind::LBracket) {
      const lex::TokenKind counted = parser.nth(tokens + 1);
      if ((counted == lex::TokenKind::IntegerLiteral ||
           (counted == lex::TokenKind::Identifier && parser.text(tokens + 1) == kInferredCount)) &&
          parser.nth(tokens + 2) == lex::TokenKind::RBracket) {
        tokens += 3;
        continue;
      }
      // `[]T`, the slice: **two** tokens, and a complete type like the counted
      // group above. The run is what a `Type` node holds, so a run that stopped
      // at the `[` would put the `]` outside it -- and in a *declaration* that is
      // not a cosmetic difference: the name is the last identifier of the run, so
      // `fn []i32 f()` would split `]` as the name and report a function called
      // `]`. One token pair here is the whole fix, and it is the same statement
      // the type reader makes one stage down: `[]` is a type.
      if (counted == lex::TokenKind::RBracket) {
        tokens += 2;
        continue;
      }
      ++tokens;
      continue;
    }
    break;
  }
  run.tokens = tokens;
  return run;
}

ListClose Parser::parseBoundTypeRun(std::uint32_t count) {
  const TokenBound bound(*this, count);
  ListClose close = parseTypeRun();
  // The reader can stop **before** the bound -- `fn i32 10 f()` hands the run a
  // token no type grammar holds -- and the tail still belongs to the `Type` node,
  // because the node covers the run the scan found. A token left outside would be
  // read by whatever comes next, which is how one mistake becomes a sentence
  // about a shape nobody wrote. The reader has already said what is wrong with
  // it, which is why this loop adds no diagnostic of its own.
  while (!source_.atEnd() && source_.position() < bound.end()) {
    bump();
  }
  // A run in a declaration has no list around it -- the run *is* the whole type
  // position -- so a compound closer that reports an enclosing list reports a
  // character with nothing to close, which is what `parseType` says for the same
  // slip. The `=` travels on, because which declaration owns one is the caller's
  // business: `type Pair<T>= (T, K);` wants it, a return type does not.
  if (close.closedParent) {
    reportUnusedListClose(close);
    close.closedParent = false;
  }
  return close;
}

void Parser::parseTypeAndName() {
  // The type and the name are both part of one run of `*` and identifiers, and
  // the only token that separates them from the rest of the declaration is `(`.
  // So the last identifier before `(` is the name and everything before it --
  // stars included -- is the type.
  //
  // A `<...>` list attached to a word is part of that word, and the *name* is the
  // word's first token: `fn Vec<i32> f<T>()` is the type `Vec<i32>` and the name
  // `f`, with `<T>` left in the stream for `parseGenericParams`. Nothing after
  // the name is consumed here, because the binders are their own node.
  const TypeRunScan run = scanTypeRun(*this);
  const std::uint32_t tokens = run.tokens;
  const std::uint32_t words = run.words;
  const std::uint32_t nameStart = run.nameStart;

  if (run.lastWordIsGroup) {
    // The run ends in a `(T, U)` and holds no identifier after it, so there is no
    // name: `fn (i32, bool) (` has the type and not the name. The whole run is the
    // `Type` and the `Name` is empty, because a tree that dropped the group here
    // is a tree that lost the return type a reader wrote.
    error(words == 1 ? "expected a return type before the function name"
                     : "expected a function name",
          words == 1 ? ParseErrorCode::ExpectedType : ParseErrorCode::ExpectedName);
    Marker type = start();
    // The run, read as a type. A group is the only way to be here without a name
    // after it, so what this keeps is the `(T, U)` a reader wrote -- shaped, so a
    // `Pair<i32, bool>` written as a product member has the same subtree it has
    // in every other type position.
    static_cast<void>(parseBoundTypeRun(tokens));
    type.complete(SyntaxKind::Type);
    Marker name = start();
    name.complete(SyntaxKind::Name);
    return;
  }

  if (words == 1 && nameStart == 0) {
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
  // The return type, read by the **type reader** and bounded to the run the scan
  // found. Bumping the run's tokens raw is what this used to do, and it is what
  // `fn Pair<T, K> make<T, K>()` cannot have: with the words left flat, the
  // `<...>` never becomes the `TypeArgList` every other type position builds, and
  // a reader below sees the words `Pair T K` and answers with a sentence about a
  // name for a type combined with something else.
  const ListClose close = parseBoundTypeRun(nameStart);
  type.complete(SyntaxKind::Type);
  // `>=` where only `>` belongs: the `=` was swallowed by the list, and a
  // function declaration has no `=` for it to belong to -- so it is named here
  // rather than left to a `parseFunctionTail` that would report the `{` it never
  // reached.
  reportUnusedListClose(close);

  Marker name = start();
  bump();
  name.complete(SyntaxKind::Name);
}

void Parser::parseArrayCount() {
  // Precondition: the current token is `[`.
  bump();
  // A number, or `_` -- the count that comes from the elements of a typed
  // initializer (`arrays.md`). Both are *accepted as the count token* here and
  // judged by the reader, which is the stage that knows what encloses the type:
  // `let a: [_]i32` is an error, `let a = [_]i32{1, 2}` is not, and only the
  // position tells them apart. Any other name is still the parse error below --
  // the two spellings are one token kind apart, and the difference is worth
  // keeping in the stage that can see the bracket.
  const bool count = at(lex::TokenKind::IntegerLiteral) ||
                     (at(lex::TokenKind::Identifier) && text(0) == kInferredCount);
  if (count) {
    bump();
  } else if (!at(lex::TokenKind::RBracket)) {
    // Not a count, and not the `[]` of a slice either. The count is a literal
    // number today (`arrays.md` decision 19), so a reader who wrote an
    // expression has one thing to fix, and the message says which. The
    // offending token is consumed so the `]` below is the one that belongs to
    // this group: leaving it would make one mistake cost two sentences.
    error("the count of an array type is a literal number, not an expression",
          ParseErrorCode::ExpectedArrayCount);
    bump();
  }
  if (at(lex::TokenKind::RBracket)) {
    bump();
    return;
  }
  error("expected `]` to close the array count", ParseErrorCode::ExpectedArrayCountClose);
}

// `type Name = T;` -- a name for a type that already exists (`type_alias.md`).
//
// The right-hand side is `parseType`, which is the *only* reader of a type in
// this grammar: every type form the language has (a primitive, a C spelling, a
// pointer, an array, a slice, and every composition of them) is already spelled
// here, so an alias adds no type syntax and cannot accept a type no signature
// accepts. The `=` is required and so is the `;`, both by `expect`, because the
// shape is the whole declaration -- `type A i32;` has no reading that means
// something else.
void Parser::parseTypeAlias() {
  Marker decl = start();
  bump(); // `type`

  Marker name = start();
  if (at(lex::TokenKind::Identifier)) {
    bump();
  } else {
    error("expected a name for the type", ParseErrorCode::ExpectedName);
  }
  name.complete(SyntaxKind::Name);

  // `type Pair<T, K> = (T, K);`: the binders, between the name and the `=`. The
  // one token that can stand here is `<`, so this asks for the list and gets an
  // empty answer when the alias is not generic.
  const ListClose binders = parseGenericParams();
  if (!binders.sawEqual) {
    // `type Pair<T>= (T, K);` writes the `>` and the `=` as one token, and the
    // list has already consumed it as the `=` of the alias.
    expect(lex::TokenKind::Equal);
  }
  reportUnusedListClose(ListClose{.closedParent = binders.closedParent});
  // The type is read even when the `=` was missing: one mistake, one sentence,
  // and the tree keeps the shape a reader wrote so nothing below has to guess at
  // what was meant.
  reportUnusedListClose(parseType());
  expect(lex::TokenKind::Semicolon);
  decl.complete(SyntaxKind::TypeAliasDecl);
}

// `<T, K>`: the binders of a declaration (`generics.md`).
//
// The list holds **names** and nothing else today. A constraint -- `T: Ordered`
// -- takes the `:` of every other binding, and its slot is deliberately not read
// here yet: syntax that parses and is then ignored would let a declaration claim
// a constraint the checker does not enforce, and an unenforced constraint is
// worse than a missing one. The constraint arrives with its enforcement.
ListClose Parser::parseGenericParams() {
  // A `<` in this position is a binder list and nothing else, which is the
  // grammar's doing rather than a convention: the *type* is what precedes the
  // name, so `fn i32 f<T>(` cannot read the `<` as part of a return type. When
  // the token is not `<` nothing is read, which is what lets `fn` and `type` call
  // this unconditionally.
  if (!at(lex::TokenKind::Less)) {
    return {};
  }
  Marker list = start();
  bump(); // `<`
  while (!bailedOut_ && !atEnd()) {
    if (!at(lex::TokenKind::Identifier)) {
      // Two readers land here, and which one it is changes the sentence. `<>` is
      // a list that binds nothing: a generic declaration with no parameter is the
      // declaration it already was, and the empty list cannot be told from the
      // *closer* by shape, so it is named here rather than left to "expected a
      // name" at a `>` that is in fact the right character. Anything else is a
      // slot with a name missing from it.
      error(atListCloser() ? "a binder list holds at least one name: write `<T>`"
                           : "expected the name of a binder",
            ParseErrorCode::ExpectedName);
      break;
    }
    Marker name = start();
    bump();
    name.complete(SyntaxKind::Name);
    if (at(lex::TokenKind::Colon)) {
      // `T: Num`: the constraint, read into the tree rather than checked here.
      //
      // The parser decides **the shape** and nothing else: one name, and that is
      // the whole of a constraint's syntax today. Whether the word is one of the
      // classes is `sema`'s question, and it is asked there for the same reason
      // every other name is -- the grammar has no table of names, and the one
      // stage that does is the one that can also explain what the alternatives
      // are (`generics.md`, § 6).
      Marker constraint = start();
      bump(); // `:`
      if (at(lex::TokenKind::Identifier)) {
        // A `Name` and not a `Type`: a class is a word of the language, and the
        // `Name` kind is what tells `resolve` exactly that -- it reports an
        // unknown name for a `PathExpr` and is silent about a `Name`, which is
        // what keeps `Number` out of the definitions the resolver looks up.
        Marker klass = start();
        bump();
        klass.complete(SyntaxKind::Name);
      } else {
        // A `:` with nothing after it, or with something that cannot be a class
        // name. The sentence names the slot's shape, and the token is left where
        // it is so the list's own closer reports the *other* mistake if there is
        // one -- `T: (,)` is two slips and gets two sentences.
        error(atListCloser() ? "a constraint needs a class name after `:`"
                             : "expected the name of a constraint class after `:`",
              ParseErrorCode::ExpectedName);
      }
      constraint.complete(SyntaxKind::Constraint);
    }
    if (at(lex::TokenKind::Comma)) {
      bump();
      continue;
    }
    break;
  }
  const ListClose close = closeList();
  list.complete(SyntaxKind::GenericParams);
  return close;
}

// `<i32, bool>`: the arguments of a use, in a type run or behind `::`.
ListClose Parser::parseTypeArgList() {
  // Precondition: the current token is `<`.
  Marker list = start();
  bump(); // `<`
  bool first = true;
  while (!bailedOut_ && !atEnd() && isTypeStart(current())) {
    // The argument's run is read by the same function a whole type position uses,
    // so an argument cannot accept a type the rest of the language refuses, and
    // `<>` cannot nest deeper than a type can. It is wrapped in a `Type` node,
    // because an argument is a type *written in a type position* and every reader
    // below asks for one by kind (`generics.md`).
    Marker argument = start();
    const ListClose close = parseTypeRun();
    argument.complete(SyntaxKind::Type);
    first = false;
    if (close.closedParent) {
      // The list inside the argument was closed by a **compound** closer, so this
      // list is closed as well and there is no closer left to read. The bit is
      // consumed here, because this is the frame it is about; the `=` travels on
      // to whoever owns it.
      list.complete(SyntaxKind::TypeArgList);
      return ListClose{.sawEqual = close.sawEqual};
    }
    if (at(lex::TokenKind::Comma)) {
      bump();
      continue;
    }
    break;
  }
  if (first) {
    // `<>`, and the same shape with a non-type token after the `<`. One sentence
    // for both, naming the written form, because the mistake is the same one: the
    // list has a slot with nothing in it.
    error("expected the type of an argument: a list is written `<i32, bool>`",
          ParseErrorCode::ExpectedType);
  }
  const ListClose close = closeList();
  list.complete(SyntaxKind::TypeArgList);
  return close;
}

// The `>` that closes a list -- and the two characters a compound token carries
// beyond it (`generics.md`, decision 4).
ListClose Parser::closeList() {
  switch (current()) {
  case lex::TokenKind::Greater:
    bump();
    return {};
  case lex::TokenKind::GreaterEqual:
    // `>=`: one list, and the `=` some declaration is asking for.
    // `let p: Pair<i32, bool>= t;` is the spelling this exists for.
    bump();
    return ListClose{.sawEqual = true};
  case lex::TokenKind::GreaterGreater:
    // `>>`: `Grid<Grid<f64>>`, one token and two lists. The second belongs to the
    // enclosing list, which is why this is a return value and not a flag: the
    // frame that owns it is several calls up, and the calls in between are the
    // type grammar's own recursion.
    bump();
    return ListClose{.closedParent = true};
  case lex::TokenKind::GreaterGreaterEqual:
    // `>>=`: two lists and the `=`. Spelled together, and never by accident:
    // `A<B<C>>= t` is exactly what the reader means.
    bump();
    return ListClose{.closedParent = true, .sawEqual = true};
  default:
    error("expected `>` to close this list of type arguments",
          ParseErrorCode::ExpectedTypeArgClose);
    // A zero-width `>` so the node's shape does not depend on whether the reader
    // wrote the closer -- the same answer `expect` gives a missing token.
    token(toSyntaxKind(lex::TokenKind::Greater), /*missing=*/true);
    return {};
  }
}

void Parser::reportUnusedListClose(ListClose close) {
  if (close.closedParent) {
    // `A<B>>` where one `>` was enough: the extra character is the mistake, and
    // the sentence says how many a list needs.
    error("one `>` closes each list of type arguments: the extra one written here closes "
          "nothing",
          ParseErrorCode::StrayTypeArgClose);
    return;
  }
  if (close.sawEqual) {
    // `>=` typed where only `>` belongs. Named because the two characters are one
    // token, so "expected `>`" would point at a `>` the reader did write, and the
    // fix is to separate them.
    error("this `=` is not part of the type: `>=` is `>` and then `=`",
          ParseErrorCode::StrayTypeArgClose);
  }
}

ListClose Parser::parseType() {
  Marker type = start();
  if (!isTypeStart(current())) {
    error("expected a type", ParseErrorCode::ExpectedType);
    type.complete(SyntaxKind::Type);
    return {};
  }
  // In an annotation (`x: T`) there is no trailing name to separate, so the
  // whole run is the type. `!` is accepted here too, and refused one stage later
  // where the position is known: this stage answers "what shape is written", and
  // `let x: !` is a shape -- a wrong one, with a sentence about why, produced by
  // the only stage that knows an object cannot have that type (`never.md`).
  ListClose close = parseTypeRun();
  type.complete(SyntaxKind::Type);
  // A `>>` here closed a list this position does not have: `A<B>>` is one `>`
  // too many, and this is the frame that knows there is no enclosing list. What
  // survives is the `=`, which *does* belong to the declaration around the type
  // (`let p: Pair<i32, bool>= t;`) and which only that frame can consume.
  if (close.closedParent) {
    reportUnusedListClose(close);
    close.closedParent = false;
  }
  return close;
}

ListClose Parser::parseTypeRun() {
  // One run, from the current token: the constructors, the words, the groups, and
  // the `<...>` lists. It stops at the first token that cannot continue a type,
  // which is what makes it reusable both for a whole position and for one member
  // of a product -- the member's run ends at a `,` or a `)` and needs no second
  // loop.
  while (!bailedOut_ && !atEnd()) {
    if (at(lex::TokenKind::Identifier) || at(lex::TokenKind::Star) || at(lex::TokenKind::Bang)) {
      bump();
      continue;
    }
    if (at(lex::TokenKind::LBracket)) {
      parseArrayCount();
      continue;
    }
    if (at(lex::TokenKind::LParen)) {
      parseTypeGroup();
      continue;
    }
    if (at(lex::TokenKind::Less)) {
      // `<i32, bool>`: the argument list of the word just read. A type position
      // has no expression in it, so a `<` after a word has exactly one meaning
      // here -- which is the whole reason the ambiguity that costs C++ a space in
      // `>>=` does not exist in this position.
      //
      // A run is a **conduit**: what a list reports is handed up unchanged, and
      // the run stops, because the list that compound closer closed encloses this
      // run. Consuming the bit here would lose it for the frame that needs it.
      const ListClose close = parseTypeArgList();
      if (close.any()) {
        return close;
      }
      continue;
    }
    break;
  }
  return {};
}

void Parser::parseTypeGroup() {
  // Precondition: the current token is `(`.
  //
  // `(T, U)`, a product. The parser's job here is the *shape*: the group is
  // closed, its members are runs, and a member with nothing written in it is one
  // mistake. What a member may be -- an object, not `void`, not `!` -- is the type
  // reader's question, one stage down, and it answers with a sentence about the
  // member it refused (`tuples.md`, decision 15).
  DepthGuard depth(*this);
  if (!depth.ok()) {
    tooDeep();
    return;
  }
  bump(); // `(`
  if (at(lex::TokenKind::RParen)) {
    // `()`: an empty group, named here because the two things it could have been
    // are a parameter list (which is not a type position) and a product (which
    // has at least two members). `void` is the type with no value, and the
    // sentence says so (`tuples.md`, decision 2).
    error("a product has at least two members, `(T, U)`; for the type with no value write `void`",
          ParseErrorCode::ExpectedType);
    bump(); // `)`
    return;
  }
  if (!at(lex::TokenKind::RParen)) {
    while (!bailedOut_ && !atEnd()) {
      if (!isTypeStart(current())) {
        error("expected the type of this member: a product is written `(T, U)`",
              ParseErrorCode::ExpectedType);
        break;
      }
      // A member is a run, so it can carry a list of arguments -- and a compound
      // closer inside it would be closing a list that this group is not, which is
      // reported beside the extra character.
      reportUnusedListClose(parseTypeRun());
      if (at(lex::TokenKind::Comma)) {
        bump();
        if (at(lex::TokenKind::RParen)) {
          // `(T,)`: the trailing comma is kept in the tree and the reader refuses
          // the one-member product by name -- the same sentence for `(T)` in a type
          // position, because it is the same mistake (`tuples.md`, decision 2).
          break;
        }
        continue;
      }
      break;
    }
  }
  if (at(lex::TokenKind::RParen)) {
    bump();
    return;
  }
  error("expected `)` to close this product", ParseErrorCode::ExpectedTypeGroupClose);
}

} // namespace minc::parse
