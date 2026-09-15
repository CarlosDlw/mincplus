// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Parser core: markers, the event stream, diagnostics, and the file/item loop.
// The grammar itself is in expression.cc, statement.cc, and declaration.cc.
#include "parse/parser.h"

#include <string>
#include <string_view>
#include <utility>

namespace minc::parse {

// ---------------------------------------------------------------- markers ---

std::uint32_t Parser::reserveSlot() {
  events_.push_back(Event{});
  return static_cast<std::uint32_t>(events_.size() - 1);
}

CompletedMarker Marker::complete(SyntaxKind kind) {
  parser_->events_[index_] = Event::start(kind);
  parser_->events_.push_back(Event::finish());
  return CompletedMarker(parser_, index_);
}

void Marker::abandon() {
  if (parser_ != nullptr) {
    parser_->events_[index_] = Event{};
  }
}

Marker CompletedMarker::precede() {
  // The parent is a node that will be finished *after* this one, so its Start
  // is reserved at the end of the stream and this node points forward to it.
  const std::uint32_t parent = parser_->reserveSlot();
  parser_->events_[index_].forwardParent = parent - index_;
  return Marker(parser_, parent);
}

SyntaxKind CompletedMarker::kind() const {
  return parser_->events_[index_].kind;
}

// ---------------------------------------------------------------- parser ---

Parser::Parser(TokenSource& source) : source_(source) {}

ParseOutput Parser::run() {
  parseFile();

  ParseOutput out;
  out.events = std::move(events_);
  out.errors = std::move(errors_);
  out.bailedOut = bailedOut_;
  return out;
}

Marker Parser::start() {
  return Marker(this, reserveSlot());
}

void Parser::token(SyntaxKind kind, bool missing) {
  events_.push_back(Event::token(kind, missing));
}

void Parser::bump() {
  token(toSyntaxKind(source_.current()), /*missing=*/false);
  source_.bump();
}

// ------------------------------------------------------------ diagnostics ---

void Parser::error(std::string message, ParseErrorCode code) {
  if (bailedOut_) {
    return;
  }
  errors_.push_back(ParseError{currentSpan(), std::move(message), code});
  if (errors_.size() >= support::kMaxParseErrors) {
    bailOut("too many syntax errors");
  }
}

void Parser::expect(lex::TokenKind kind) {
  if (at(kind)) {
    bump();
    return;
  }
  error("expected '" + std::string(tokenSpelling(kind)) + "'", ParseErrorCode::ExpectedToken);
  // Insert the token the grammar needs instead of leaving a hole: a `;` slot
  // must be a `;` slot whether or not the user has typed it, or every accessor
  // and every tree walk would have to cope with a node whose shape depends on
  // what is missing.
  token(toSyntaxKind(kind), /*missing=*/true);
}

void Parser::bailOut(std::string message) {
  if (bailedOut_) {
    return;
  }
  bailedOut_ = true;
  errors_.push_back(ParseError{currentSpan(), std::move(message), ParseErrorCode::Aborted});
}

void Parser::tooDeep() {
  if (depthReported_) {
    return;
  }
  depthReported_ = true;
  bailOut("expression or block nests too deeply");
}

// ------------------------------------------------------------------ file ---

// The tokens that can begin a file-scope item. One predicate rather than the
// same list of comparisons in the loop and in the recovery, so a form added here
// (`struct`, `import`) is added in one place and the loop and the recovery
// cannot come to different answers.
//
// A *binding* is an item: `let`/`const` at the top of a unit is the same
// production a block-scope binding uses, and `static` is a prefix on either
// form, which is why the prefix is here beside the two words that follow it.
[[nodiscard]] static bool isItemStart(const Parser& parser) {
  return parser.at(lex::TokenKind::KwFn) || parser.at(lex::TokenKind::KwExtern) ||
         parser.at(lex::TokenKind::KwStatic) || parser.at(lex::TokenKind::KwLet) ||
         parser.at(lex::TokenKind::KwConst);
}

void Parser::parseFile() {
  Marker file = start();
  while (!atEnd() && !bailedOut_) {
    if (isItemStart(*this)) {
      parseItem();
    } else {
      error("expected a declaration", ParseErrorCode::ExpectedItem);
      recoverItem();
    }
  }

  if (bailedOut_) {
    // Everything left is still source and must be preserved, so it becomes one
    // `Error` node rather than being dropped.
    Marker junk = start();
    while (!atEnd()) {
      bump();
    }
    junk.complete(SyntaxKind::Error);
  }

  // The end-of-file leaf: this is what puts the trailing trivia and the final
  // byte inside the file node, so concatenating every leaf reproduces the
  // source exactly. `bump()` here is safe at the end and a no-op afterwards.
  bump();
  file.complete(SyntaxKind::File);
}

// One file-scope item. `static` is a prefix on both forms -- a function and a
// binding -- so a single lookahead past it is what says which declaration is
// being read; the rest of the shape is the same production the block scope uses.
void Parser::parseItem() {
  if (bailedOut_) {
    return;
  }

  const bool isStatic = at(lex::TokenKind::KwStatic);
  const lex::TokenKind leader = isStatic ? nth(1) : current();

  // `extern` on a binding. Refused by name rather than misread: without this the
  // `let` would be reported as a function declaration missing its `fn`, which is
  // true of the shape and says nothing about the word the reader meant.
  if (leader == lex::TokenKind::KwExtern &&
      (nth(1) == lex::TokenKind::KwLet || nth(1) == lex::TokenKind::KwConst)) {
    error("`extern` on a binding is not implemented yet: a file-scope binding is defined in this "
          "unit, and `extern fn` is the declaration form for a function",
          ParseErrorCode::ExternBinding);
    recoverItem();
    return;
  }

  // `static` in front of something that is not a declaration. Reported here, and
  // the recovery consumes the word, so the file loop always makes progress.
  if (isStatic && leader != lex::TokenKind::KwFn && leader != lex::TokenKind::KwExtern &&
      leader != lex::TokenKind::KwLet && leader != lex::TokenKind::KwConst) {
    error("`static` makes a declaration internal to this unit; it belongs before `fn`, `let` or "
          "`const`",
          ParseErrorCode::StaticPosition);
    recoverItem();
    return;
  }

  switch (leader) {
  case lex::TokenKind::KwFn:
    parseFnDecl(/*isExtern=*/false, isStatic);
    return;
  case lex::TokenKind::KwExtern:
    parseFnDecl(/*isExtern=*/true, isStatic);
    return;
  case lex::TokenKind::KwLet:
  case lex::TokenKind::KwConst:
    parseFileBinding(/*isConst=*/leader == lex::TokenKind::KwConst, isStatic);
    return;
  default:
    break;
  }

  // Unreachable from `parseFile`, which only calls this on an item start; kept
  // total so a future caller cannot make the file loop spin.
  error("expected a declaration", ParseErrorCode::ExpectedItem);
  recoverItem();
}

// A file-scope binding, which is the block-scope one plus the `;` and an
// optional `static`. One node kind, so a consumer reads a binding once wherever
// it was written -- and so `lower` summarises one shape into an `Item` instead
// of two.
void Parser::parseFileBinding(bool isConst, bool isStatic) {
  Marker stmt = start();
  if (isStatic) {
    bump(); // `static`
  }
  parseBinding();
  expect(lex::TokenKind::Semicolon);
  stmt.complete(isConst ? SyntaxKind::ConstStmt : SyntaxKind::LetStmt);
}

// Always consumes at least one token. The caller may have already decided that
// the *current* token is wrong -- and the current token is then an item start by
// construction -- so a loop that only advances to the next item start would
// return without moving and the file loop would spin on it forever.
void Parser::recoverItem() {
  Marker junk = start();
  if (!atEnd()) {
    bump();
  }
  while (!atEnd() && !isItemStart(*this)) {
    bump();
  }
  junk.complete(SyntaxKind::Error);
}

} // namespace minc::parse
