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
  void parseType();        // type-only position (after `:`)
  void parseTypeAndName(); // `fn` return type followed by the function name
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
  CompletedMarker parseUnary();
  CompletedMarker parsePostfix();
  CompletedMarker parsePrimary();
  void parseArgList();

  // -- recovery -------------------------------------------------------------
  void recoverStatement();
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
