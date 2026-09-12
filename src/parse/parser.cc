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
  if (errors_.size() >= kMaxParseErrors) {
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

void Parser::parseFile() {
  Marker file = start();
  while (!atEnd() && !bailedOut_) {
    if (at(lex::TokenKind::KwFn)) {
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

void Parser::parseItem() {
  if (bailedOut_) {
    return;
  }
  parseFnDecl();
}

void Parser::recoverItem() {
  Marker junk = start();
  while (!atEnd() && !at(lex::TokenKind::KwFn)) {
    bump();
  }
  junk.complete(SyntaxKind::Error);
}

} // namespace minc::parse
