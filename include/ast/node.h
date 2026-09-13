// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The lowered node: what the analysis stages walk.
//
// Three differences from a green node, and each one exists to remove a cost the
// tree deliberately pays:
//
//   * **It has a position.** A green node is position-free (that is what makes
//     it shareable); this one carries two spans -- the origin, where the bytes
//     were *written*, and the unit-text range, which is what a dump reads. The
//     first is why a diagnostic in a macro expansion can point at the header.
//   * **Its children are a range.** `SyntaxNode::childOfKind` is a linear scan
//     of a homogeneous child list; here a node's children live in one contiguous
//     slice of a side array, so a consumer indexes instead of searching.
//   * **It carries the interned name.** A `Name` and a `PathExpr` both mean
//     \"this identifier\"; interning is done once, during lowering, so no later
//     stage compares spellings.
//
// What it deliberately does *not* carry: trivia (dropped), semantics (a
// `NameRef` lives in a parallel array, not in the node), a parent (a top-down
// walk knows one, and the IDE asks `source_to_def` instead), and a pointer to
// anything (identity is an index -- `parser.md` decision 13).
#pragma once

#include <cstdint>

#include "parse/syntax_kind.h"
#include "support/intern/sym_id.h"
#include "support/span/span.h"

namespace minc::ast {

// The lowered AST shares the syntax tag space rather than defining a second one.
// `SyntaxKind` already covers tokens and interior nodes with one `u16`, and a
// `NodeKind` of its own would be a mapping table to keep in sync for no gain.
using NodeKind = parse::SyntaxKind;

// Index into a `LoweredFile`'s node array. A `LoweredFile` is one translation
// unit, so the file half of \"an identity\" is the file node itself and the index
// is the whole id -- which is what keeps it a 4-byte value that can be stored in
// a hash map and compared without touching a tree. `0` is always the `File`
// node, so a valid id is never 0 by accident and the invalid one is the ceiling.
inline constexpr std::uint32_t kInvalidAst = 0xFFFFFFFFu;

struct AstId {
  std::uint32_t index = kInvalidAst;

  [[nodiscard]] constexpr bool valid() const {
    return index != kInvalidAst;
  }
  friend constexpr bool operator==(AstId, AstId) = default;
};

struct Node {
  NodeKind kind = NodeKind::Error;
  // Where the bytes were written. For a token that came out of a header or a
  // macro body this is *that* file, which is the whole point of provenance.
  support::Span origin;
  // The same token's range in the translation unit's own text. Equal to
  // `origin` for a file with no macros and no includes; what a dump prints from.
  support::Span unit;
  // Slice of `LoweredFile::children()`.
  std::uint32_t firstChild = 0;
  std::uint32_t childCount = 0;
  // The interned spelling of a name-bearing node and of an `Identifier` leaf.
  // `kInvalidSym` for everything else.
  //
  // A `Type` is deliberately *not* name-bearing: `unsigned long long int` is a
  // run of identifiers and has no single name, so the spelled text (which
  // `spellingOf` returns) is authoritative and no stage invents a name for it.
  support::SymId name = support::kInvalidSym;
  // True when this node is an `Error` or lies inside one. Computed once during
  // lowering -- it is the flag that keeps validation and resolution from
  // reporting a region the parser already reported. Inherited down the tree, so
  // a query is a load and not an ancestor walk.
  bool inError = false;

  // True for a token: a leaf. Distinct from "has no children", because an empty
  // *interior* node -- the zero-width `Error` the parser leaves where an
  // expression should have been, an empty parameter list -- has no children
  // either, and treating it as a token is how a region the parser reported would
  // slip past the `inError` guard and be reported twice.
  [[nodiscard]] constexpr bool isToken() const {
    return parse::isTokenKind(kind);
  }
  // True when the node has no children at all. Use `isToken()` to ask what kind
  // of node this is.
  [[nodiscard]] constexpr bool isLeaf() const {
    return childCount == 0;
  }
  [[nodiscard]] constexpr bool is(NodeKind other) const {
    return kind == other;
  }
};

} // namespace minc::ast
