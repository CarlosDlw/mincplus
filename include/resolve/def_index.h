// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The two indexed questions every stage below `resolve` asks of a `DefMap`.
//
// Both answers are "which `Def` does this node mean", and both are keyed on the
// same thing: the *unit* offset of the node's first token. A unit offset is one
// token each, while a written span is not -- a macro can give two names one
// written location (`Def::unitSpan`) -- so the unit offset is the only key that
// identifies a node.
//
// It lives here, and not in each consumer, because it was in each consumer: `sema`
// and `ir` carried the same three pieces -- the offset key, the declaration
// index, and the written-span fallback -- with a comment at each copy saying the
// other one had to change with it. That comment is the bug report. One rule, one
// implementation, and a stage that wants it links it.
#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>

#include "ast/ast.h"
#include "resolve/map.h"
#include "support/span/file_id.h"
#include "support/span/span.h"

namespace minc::resolve {

// `(file, unit offset)` packed into the one integer a map is keyed on. The file
// half is part of the key because a unit spans several files whose offsets
// restart, so an offset alone would collide across a `#include`.
[[nodiscard]] constexpr std::uint64_t unitOffsetKey(support::FileId file, std::uint32_t begin) {
  return (static_cast<std::uint64_t>(file) << 32U) | begin;
}

[[nodiscard]] constexpr std::uint64_t unitOffsetKey(support::Span unit) {
  return unitOffsetKey(unit.file, unit.begin);
}

// A `DefId` packed the same way, for the maps keyed on a declaration rather than
// on a position: storage, a function, a type.
[[nodiscard]] constexpr std::uint64_t defKey(DefId def) {
  return unitOffsetKey(def.file, def.index);
}

// Two indexes over one `DefMap`, built once and read for the rest of the stage.
//
// The map is borrowed, not copied: a `DefMap` is the biggest value in the
// pipeline and every consumer already holds it.
class DefIndex {
public:
  explicit DefIndex(const DefMap& map);

  // The reference written at this unit offset, or null. A name use that the
  // resolver recorded, whether or not it resolved.
  [[nodiscard]] const NameRef* refAt(support::Span unit) const;

  // The declaration that reference resolves to, or nullopt when there is no
  // reference there or the name did not resolve.
  [[nodiscard]] std::optional<DefId> targetAt(support::Span unit) const;

  // The declaration whose name token starts at this unit offset, or nullopt.
  // The unit half is exact whenever the token came from the unit; a token the
  // preprocessor *inserted* has no unit range, and that is what the written-span
  // fallback below is for.
  [[nodiscard]] std::optional<DefId> defAtUnitOffset(support::Span unit) const;

  // The first declaration whose written name lies inside `written`.
  //
  // The fallback for a synthetic name with no unit range, and only that: it is
  // exact when nothing was expanded, and it deliberately answers with the
  // *earliest* declaration so that the answer stays a function of the input
  // rather than of the map's iteration order.
  [[nodiscard]] std::optional<DefId> defInsideWritten(support::Span written) const;

  // Both of the above, for an AST `Name` node: the unit offset first, the
  // written span second. `file` is the lowered tree the node belongs to.
  [[nodiscard]] std::optional<DefId> defAtName(const ast::LoweredFile& file,
                                               ast::AstId nameNode) const;

private:
  const DefMap& map_;
  // Unit offset -> index into `map_.refs`, and -> `DefId`. Indexed rather than
  // scanned, because both questions are asked once per node and a scan would be
  // quadratic in the unit's size.
  std::unordered_map<std::uint64_t, std::size_t> refsByOffset_;
  std::unordered_map<std::uint64_t, DefId> defsByOffset_;
};

} // namespace minc::resolve
