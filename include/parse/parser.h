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
#include "support/span/span.h"

namespace minc::parse {

// How deeply recursive-descent parsing may nest before it refuses to go on.
//
// This is a stack-safety limit, not a language limit: 256 nested constructs is
// far more than real code has, and it is low enough that the deepest case still
// fits a Windows thread's 1 MiB stack *with sanitizers on* (which use larger
// frames). Clang's `-fbracket-depth` default is the same number.
inline constexpr std::uint32_t kMaxNestingDepth = 256;

// After this many errors the parser stops trying and consumes the rest of the
// input into one `Error` node. Pathological input then costs bounded work
// instead of a quadratic error cascade.
inline constexpr std::size_t kMaxParseErrors = 4096;

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
  void parseFnDecl();
  void parseParamList();
  void parseBlock();
  void parseStmt();
  void parseLetStmt(bool isConst);
  void parseReturnStmt();
  void parseExprStmt();
  void parseType();        // type-only position (after `:`)
  void parseTypeAndName(); // `fn` return type followed by the function name

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
      : parser_(parser), entered_(parser.depth_ < kMaxNestingDepth) {
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
