// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The parser's output, before it is a tree.
//
// The parser emits a flat list of events; a separate builder (in `syntax`)
// turns them into the green tree. The two are decoupled on purpose:
//
//   * the grammar is testable with a trivial sink -- no arena, no offsets, no
//     tree storage,
//   * the tree representation can change without touching the parser,
//   * an abandoned speculative parse is one tombstone, not an undo,
//   * a left-recursive construct is expressible in a left-to-right parse via
//     `forwardParent` (see below).
//
// A `Token` event carries no source index: it means "the next significant token
// from the source", and the builder -- which is the only component that sees
// trivia -- flushes the trivia in between before emitting it. That is what keeps
// the grammar free of whitespace rules while the tree stays lossless.
#pragma once

#include <cstdint>

#include "parse/syntax_kind.h"

namespace minc::parse {

enum class EventTag : std::uint8_t {
  // Reserved but never completed (or explicitly abandoned): ignored.
  Tombstone,
  // Opens a node. Everything until the matching Finish is its content.
  Start,
  // Closes the most recently opened node.
  Finish,
  // A leaf. See `missing`.
  Token,
};

struct Event {
  EventTag tag = EventTag::Tombstone;
  SyntaxKind kind = SyntaxKind::Error;

  // `Start` only: distance in events to this node's *forward parent*, or 0.
  //
  // A left-recursive construct emits its first node before it knows a parent
  // exists. Instead of moving the finished node into a new parent, the finished
  // node records where its parent will be, and the builder enters the parent
  // nodes before it when it reaches them:
  //
  //   a + b + c
  //   Start(BinaryExpr) Token(a) ...            <- 'a + b' finished here
  //   Start(BinaryExpr) forwardParent = 6       <- this will be its parent
  //   Token(+) Token(c) Finish
  //
  // The distance is always >= 1, because the parent is always later.
  std::uint32_t forwardParent = 0;

  // `Token` only: the token was inserted by error recovery, not read from the
  // source. It has zero width, and it exists so a node's shape does not depend
  // on what the user has typed yet -- every `;` slot is a `;` slot whether or
  // not the character is there.
  bool missing = false;

  [[nodiscard]] static Event start(SyntaxKind kind) {
    Event event;
    event.tag = EventTag::Start;
    event.kind = kind;
    return event;
  }
  [[nodiscard]] static Event finish() {
    Event event;
    event.tag = EventTag::Finish;
    return event;
  }
  [[nodiscard]] static Event token(SyntaxKind kind, bool missing) {
    Event event;
    event.tag = EventTag::Token;
    event.kind = kind;
    event.missing = missing;
    return event;
  }
};

} // namespace minc::parse
