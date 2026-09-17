// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The recursive-descent parser.
//
// It emits events and errors and nothing else: no tree, no diagnostics, no
// arena, no I/O, no printing. That is what lets the grammar be tested without a
// `Session`, and what keeps a change to the tree representation away from the
// grammar.
//
// Design record: `docs/architectures/parser.md`.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lex/token_kind.h"
#include "parse/event.h"
#include "parse/parse_error.h"
#include "parse/syntax_kind.h"
#include "parse/token_source.h"
#include "support/limits.h"
#include "support/span/span.h"

namespace minc::parse {

// The parser's two bounds -- `support::kMaxNestingDepth` and
// `support::kMaxParseErrors` -- live in `support/limits.h` with every other
// limit, so there is one place to look and one place to change.

struct ParseOutput {
  std::vector<Event> events;
  std::vector<ParseError> errors;
  // True when the parser gave up early. The tree still covers every byte; it
  // just has a trailing `Error` node holding the unparsed remainder.
  bool bailedOut = false;
};

// What a list of type arguments -- or of binders -- consumed *beyond* its own
// closing `>`.
//
// Lists are closed by `>`. They are also closed by the first half of a `>>`,
// which is one token to this language's lexer (maximal munch, so `.` and `..`
// are two tokens for the same reason) and two closers to the grammar, because
// `Grid<Grid<f64>>` is the spelling the market converged on and a reader should
// never have to write a space to nest a type. `>=` and `>>=` carry an `=` that
// belongs to the declaration around the list, not to the list.
//
// **A returned value, and not a parser flag.** Closing is observed deep inside a
// type run and paid for several frames up, and every frame in between is the
// grammar's own recursion -- which is exactly the traffic a return value can
// carry and a flag cannot. A flag would also survive the one thing that must
// clear it, a desynchronized token stream, and that is the failure this parser
// does not recover from (`generics.md`, decision 4).
struct ListClose {
  // `>>` or `>>=`: this token closed the enclosing list as well.
  bool closedParent = false;
  // `>=` or `>>=`: the token also carried the `=` a declaration is looking for.
  bool sawEqual = false;

  [[nodiscard]] bool any() const {
    return closedParent || sawEqual;
  }
};

class Parser;
class CompletedMarker;

// A node that has been opened but not yet closed. Points at a reserved slot in
// the event stream.
class Marker {
public:
  Marker() = default;

  // Closes the node. Emits the `Start` and pushes the `Finish`. Not nodiscard:
  // many nodes are built for their shape alone and never need the handle.
  CompletedMarker complete(SyntaxKind kind);
  // Drops the node: its future content belongs to the enclosing node instead.
  void abandon();

  [[nodiscard]] bool valid() const {
    return parser_ != nullptr;
  }

private:
  friend class Parser;
  friend class CompletedMarker;
  Marker(Parser* parser, std::uint32_t index) : parser_(parser), index_(index) {}

  Parser* parser_ = nullptr;
  std::uint32_t index_ = 0;
};

// A node that has been closed, and that a later node can adopt as a child.
class CompletedMarker {
public:
  CompletedMarker() = default;

  // Opens a new node that will *contain* this one, without moving anything.
  //
  // This is how a left-associative chain is built left to right: `a + b + c`
  // parses `a + b`, and then a new `BinaryExpr` adopts it and takes `+ c`. The
  // link is recorded as `forwardParent` on this node's `Start` event.
  Marker precede();

  [[nodiscard]] SyntaxKind kind() const;
  [[nodiscard]] bool valid() const {
    return parser_ != nullptr;
  }

private:
  friend class Parser;
  friend class Marker;
  CompletedMarker(Parser* parser, std::uint32_t index) : parser_(parser), index_(index) {}

  Parser* parser_ = nullptr;
  std::uint32_t index_ = 0;
};

class Parser {
public:
  explicit Parser(TokenSource& source);

  Parser(const Parser&) = delete;
  Parser& operator=(const Parser&) = delete;

  // Parses the whole source and returns the events and the errors.
  [[nodiscard]] ParseOutput run();

  // -- event stream (used by Marker / CompletedMarker) ----------------------
  [[nodiscard]] Marker start();
  void token(SyntaxKind kind, bool missing);
  // Emits the current token and advances. At the end it emits the end-of-file
  // leaf once, which is what makes the trailing trivia and the final byte part
  // of the tree.
  void bump();

  // -- diagnostics ----------------------------------------------------------
  void error(std::string message, ParseErrorCode code);
  // Consumes `kind` if it is next; otherwise reports it and inserts a
  // zero-width token of that kind so the node's shape stays stable.
  void expect(lex::TokenKind kind);

  // True when the `[...]` group at the current token is the count of a typed
  // initializer and not the start of a list. Defined in expression.cc, where
  // the rule that decides it lives.
  [[nodiscard]] bool atTypedInitializer() const;

  // -- input ----------------------------------------------------------------
  [[nodiscard]] lex::TokenKind current() const {
    return source_.current();
  }
  [[nodiscard]] lex::TokenKind nth(std::uint32_t n) const {
    return source_.nth(n);
  }
  [[nodiscard]] bool at(lex::TokenKind kind) const {
    return current() == kind;
  }
  // The spelling of the token `nth(n)` looks at. Read only where the text *is*
  // the rule -- `_` as a count -- because a rule written against a spelling is a
  // rule that changes when the lexer does.
  [[nodiscard]] std::string_view text(std::uint32_t n) const {
    return source_.textOf(n);
  }
  [[nodiscard]] bool atEnd() const {
    return source_.atEnd();
  }
  [[nodiscard]] support::Span currentSpan() const {
    return source_.spanOfCurrent();
  }
  // The span of `nth(n)`. The grammar asks it for one thing: whether two tokens
  // were *written together* (`10z`), which no kind can answer.
  [[nodiscard]] support::Span spanOf(std::uint32_t n) const {
    return source_.spanOf(n);
  }
  [[nodiscard]] bool bailedOut() const {
    return bailedOut_;
  }

  // -- grammar --------------------------------------------------------------
  // Defined across parser.cc (file and items), declaration.cc (functions and
  // types), statement.cc, and expression.cc, so each file holds one part of the
  // grammar.
  void parseFile();
  void parseItem();
  // The two linkage words are parameters rather than something this function
  // sniffs for, because which form is being parsed is already decided by
  // `parseItem` and the forms differ in the body and nowhere else. Both are
  // *prefixes* on the one `FnDecl` node, so the node holds the words a reader
  // wrote and the tree stays lossless.
  void parseFnDecl(bool isExtern, bool isStatic);
  // A file-scope binding. It builds the **same** `LetStmt`/`ConstStmt` node a
  // block-scope one does -- one production, two positions -- with `static` as a
  // child token when it was written, which is where `resolve` reads the linkage
  // from.
  void parseFileBinding(bool isConst, bool isStatic);
  // `type Name = T;`. One caller today: `parseItem`, because a type name is a
  // *unit-level* name and a block-scope one is refused where the block is read
  // (`statement.cc`). Type parameters have a slot between the name and the `=`
  // when the language grows them, which is why the name is read as its own node
  // and not folded into the `=`'s lookahead.
  void parseTypeAlias();
  // `<T, K>`: the binders of a declaration, after the name being declared and
  // before its parameter list or its `=`. One production for `fn` and for `type`,
  // because the position is one position (`generics.md`, decision 1).
  //
  // Returns what its closing token consumed beyond itself, because the last
  // binder of `type Pair<T>= (T, K);` leaves the `=` of the alias already read.
  [[nodiscard]] ListClose parseGenericParams();
  // `<i32, bool>`: the arguments of a use, inside a type run or behind the `::`
  // of an explicit call. Precondition: the current token is `<`.
  //
  // This and `parseGenericParams` are two functions over *one* list shape and one
  // closer, differing in what the list holds -- binders are names, arguments are
  // types -- which is why the closing rule is read from one place for both.
  [[nodiscard]] ListClose parseTypeArgList();
  // The `>` that closes a list, splitting a compound token when the source wrote
  // one: `>>` is two closures, and `>=`/`>>=` carry the `=` a declaration owns.
  [[nodiscard]] ListClose closeList();
  // Is the current token a closer? `>`, `>=`, `>>`, `>>=` -- one question with
  // four spellings, answered in one place, so no reader of the grammar has to
  // hold all four (`closeList` is where the four are told apart).
  [[nodiscard]] bool atListCloser() const {
    return at(lex::TokenKind::Greater) || at(lex::TokenKind::GreaterEqual) ||
           at(lex::TokenKind::GreaterGreater) || at(lex::TokenKind::GreaterGreaterEqual);
  }
  // A closing token that no frame can own, reported beside the character to
  // delete. Every type position and every `::` calls it with what its reader
  // returned, so one sentence covers all of them and none of them stays silent.
  void reportUnusedListClose(ListClose close);
  // The part after the closing `)`: a block for a definition, `;` for an
  // `extern` declaration, and a diagnostic for either of the two wrong
  // combinations.
  void parseFunctionTail(bool isExtern);
  // `allowVariadic` is the enclosing declaration's form, which is what decides
  // whether `...` is legal: only a declaration may be variadic, because reading
  // the arguments needs `va_start`, which the language does not have.
  void parseParamList(bool allowVariadic);
  void parseParam();
  // The `...` of a parameter list, which ends it. `hasParameter` is whether a
  // parameter was written before the marker.
  void parseVariadicMarker(bool allowVariadic, bool hasParameter);
  void parseBlock();
  void parseStmt();
  void parseLetStmt(bool isConst);
  // The binding itself -- `let x: T = e` -- with no terminator. `parseLetStmt`
  // is this plus the `;`, and the `for` initializer is this plus the `;` that
  // separates the clauses. One implementation, so the two spellings of a
  // binding cannot drift apart.
  void parseBinding();
  // `(a, b)`: the left-hand side of a destructuring binding. One `Name` child per
  // position, and `_` for a member that is skipped -- which is a `Name` too, so
  // every later stage reads it with the code it already had (`tuples.md`,
  // decision 6).
  void parseBindingPattern();
  void parseReturnStmt();
  void parseExprStmt();
  void parseIfStmt();
  void parseWhileStmt();
  // `for init; cond; step { body }`. The parentheses around the clauses are
  // optional, as in every other condition in the language.
  void parseForStmt();
  void parseForInit();
  // The condition or the step of a `for`, wrapped in its own kind so a consumer
  // asks for it by name instead of by position. An omitted clause is a
  // zero-width node of that kind.
  void parseForClause(SyntaxKind wrapper);
  void parseJumpStmt(SyntaxKind kind);
  // A type-only position (after `:`).
  //
  // Returns what the type's closing token consumed beyond itself. Only the
  // caller knows whether an `=` belongs there -- `let p: Pair<i32, bool>= t;`
  // wants it, a parameter does not -- so the reader reports it and the caller
  // decides. A `>>` that lands with no enclosing list is reported here, where
  // the absence is known.
  [[nodiscard]] ListClose parseType();
  // One run of a type position: the constructors, the words, the `(T, U)`
  // groups, and the `<...>` argument lists, stopping at the first token that
  // cannot continue a type. Shared by a whole position and by one member of a
  // product, which is why it is not folded into `parseType` (`tuples.md`,
  // decision 15).
  //
  // A run is a pure conduit for a compound closer: what a list inside it reports
  // travels up unchanged, and stops the run, because the frame the report is
  // about encloses the run.
  [[nodiscard]] ListClose parseTypeRun();
  // `(T, U)`: a product. Its members are runs, so this is the one place the type
  // grammar is recursive, and it is guarded like the expression grammar is.
  void parseTypeGroup();
  void parseTypeAndName(); // `fn` return type followed by the function name
  // The type of a cast, in either spelling. Not `parseType`: a cast's type is
  // followed by an *expression*, so a `*` after the type's last word is the
  // multiplication it looks like -- `a as i32 * 2` is `(a as i32) * 2`, and a
  // reader that ran the declaration grammar here would swallow the `*` and build
  // a type nothing can spell (`casts.md`, decision 3).
  void parseCastType();
  // One `[`, count, `]` group of a type position, always consumed whole: the
  // group is the unit the count belongs to, and a group the parser leaves half
  // read is a group the next construct re-reads as something else.
  void parseArrayCount();

  // The two literal forms. `parseInitializerElements` is the body both share --
  // the comma-separated list with its trailing comma, or `value ; count` --
  // because the only thing that separates a list from a fill is one `;`, and two
  // implementations would be two places for that to drift.
  CompletedMarker parseArrayLiteral();
  CompletedMarker parseTypedInitializer();
  // One element of an initializer, in either grouping (`parseInitializerElements`
  // is the loop over these). A separate function because of the `{` recovery:
  // one place reads "what starts an element", and one place owns the sentence.
  void parseElement();
  // A `{...}` where a type was expected to come first: refused by name, and read
  // as the `ArrayLiteral` the reader meant, so one mistake stays one message.
  // Called from the two positions a stray brace can appear in -- an element, and
  // a whole value -- which is what keeps the sentence in one place.
  CompletedMarker parseBraceGroup();
  void parseInitializerElements(lex::TokenKind closer);

  void parseExpr();
  CompletedMarker parseAssign();
  CompletedMarker parseConditional();
  CompletedMarker parseBinary(std::uint8_t minPrecedence);
  // The `as` level: a postfix operator above every binary operator and below the
  // prefix ones, so `-a as i64` is `(-a) as i64` and `a as i64 * 2` is
  // `(a as i64) * 2`. Chains are left-associative.
  CompletedMarker parseUnary();
  // The operand of a cast, an operand of `as`, or the expression a prefix
  // operator applies to. Split out of `parseUnary` because `as` sits *between*
  // the two: a prefix operator and `as` do not nest in the same direction.
  CompletedMarker parsePrefix();
  // `(T)x`, which is a cast only when the run inside the parentheses is a
  // complete type of reserved type names and an expression follows the `)`; that
  // is what `atCastStart` decides, and this builds the node.
  CompletedMarker parseCastPrefix();
  [[nodiscard]] bool atCastStart() const;
  // The same scan asked one more question: the group is a complete type run and
  // the type is a **product** (a comma at the group's own level). `(i32, bool)x`
  // is the one cast shape the language refuses by name (`tuples.md`).
  [[nodiscard]] bool atProductCastStart() const;
  // That refusal: one diagnostic at the group, and a `CastExpr` holding the whole
  // expression, so the operand is not read as a second mistake.
  CompletedMarker parseProductCastRefusal();
  // A literal written against an identifier: `10z`. Reported and consumed as an
  // `Error` child so one slip costs one diagnostic instead of a cascade from
  // whatever expected the expression to end.
  [[nodiscard]] bool atLiteralSuffixRun() const;
  // `.0` / `.field`, the one postfix that reads a component of a value
  // (`tuples.md`). Decided by a token of lookahead -- a digit is a position of a
  // product, a name is a field of a `struct` when one lands -- and the two are the
  // same node, because they are the same question.
  CompletedMarker parseFieldPostfix(CompletedMarker base);
  // `(a, b)`, the product literal. Decided by the comma and nothing else: the
  // first expression is parsed either way, and the `,` that follows it is what
  // makes the group a product rather than the parenthesised value it has always
  // been (`tuples.md`, decision 7). The `(` and the first element are already
  // read when this is called, which is what makes the decision free.
  CompletedMarker parseTupleLiteral(Marker group);
  CompletedMarker parsePostfix();
  CompletedMarker parsePrimary();
  void parseArgList();

  // -- recovery -------------------------------------------------------------
  void recoverStatement();
  // One token the grammar could not use, wrapped in an `Error` node and consumed.
  // The node is what makes the *rest* of the construct parse on: a token left in
  // the stream is re-read by whatever comes next, which turns one mistake into a
  // second sentence about a shape nobody wrote (`ast` skips the region, and every
  // reader below it answers the poison silently). `recoverStatement` is the whole
  // statement's version of the same idea.
  void consumeAsError();
  // Does the current token end the expression rather than continue it? A `.` at the
  // end of one is followed by `;`, `)`, `]`, `,` or `}` -- and there the honest
  // sentence belongs to the construct that wanted the expression to end, which is
  // why nothing is consumed.
  [[nodiscard]] bool atExpressionEnd() const;
  void recoverItem();
  void bailOut(std::string message);

  // -- depth ----------------------------------------------------------------
  // Reports the nest-too-deep error once and gives up, so the rest of the input
  // becomes one `Error` node instead of a stack overflow.
  void tooDeep();

private:
  friend class Marker;
  friend class CompletedMarker;
  friend class DepthGuard;

  [[nodiscard]] std::uint32_t reserveSlot();
  // Reports the depth limit once and returns the empty `Error` node that keeps
  // the tree total while the parser unwinds. One implementation, so every
  // guarded entry point fails the same way.
  [[nodiscard]] CompletedMarker recursionLimitError();

  TokenSource& source_;
  std::vector<Event> events_;
  std::vector<ParseError> errors_;
  std::uint32_t depth_ = 0;
  bool bailedOut_ = false;
  bool depthReported_ = false;
};

// RAII for the recursion limit. Construction succeeds only while there is room:
// a failed guard must be checked, and the caller must not descend. The counter
// is decremented only if it was incremented, so the guard cannot underflow.
class DepthGuard {
public:
  explicit DepthGuard(Parser& parser)
      : parser_(parser), entered_(parser.depth_ < support::kMaxNestingDepth) {
    if (entered_) {
      ++parser_.depth_;
    }
  }
  ~DepthGuard() {
    if (entered_) {
      --parser_.depth_;
    }
  }

  DepthGuard(const DepthGuard&) = delete;
  DepthGuard& operator=(const DepthGuard&) = delete;

  [[nodiscard]] bool ok() const {
    return entered_;
  }

private:
  Parser& parser_;
  bool entered_;
};

// The spelling a token would have in source, for "expected ';'" messages.
// Falls back to the kind's name for tokens with no fixed spelling.
[[nodiscard]] std::string_view tokenSpelling(lex::TokenKind kind);

} // namespace minc::parse
