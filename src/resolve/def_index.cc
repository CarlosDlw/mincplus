// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "resolve/def_index.h"

namespace minc::resolve {

DefIndex::DefIndex(const DefMap& map) : map_(map) {
  // References first. The reference array is already in source order, and a
  // duplicate offset cannot happen for a well-formed unit (a byte belongs to one
  // token); `emplace` keeps the first, so the answer stays a function of the
  // input if it ever did.
  refsByOffset_.reserve(map_.refs.size());
  for (std::size_t i = 0; i < map_.refs.size(); ++i) {
    refsByOffset_.emplace(unitOffsetKey(map_.refs[i].unitSpan), i);
  }

  // The declarations, in the other direction: from a declaration's name node to
  // the def the resolver created for it.
  //
  // A predefined name is skipped rather than parked at offset 0, where a real
  // declaration at the start of the unit would land. It has no declaration node
  // to look up in the first place -- that is what *predefined* means.
  //
  // The answer is the def's **identity** and not its own slot: a name declared
  // twice -- `extern fn i32 f();` above `fn i32 f() { }` -- is one function, so
  // both declaration sites have to answer with one `DefId`. Answering with the
  // site's own index would give one function two types and the lowering two
  // symbols.
  defsByOffset_.reserve(map_.defs.size());
  for (std::size_t i = 0; i < map_.defs.size(); ++i) {
    const Def& def = map_.defs[i];
    if (isPredefined(def.predefined)) {
      continue;
    }
    const DefId site = defIdOf(def, static_cast<std::uint32_t>(i));
    defsByOffset_.emplace(unitOffsetKey(def.unitSpan), canonicalOf(def, site));
  }
}

const NameRef* DefIndex::refAt(support::Span unit) const {
  const auto found = refsByOffset_.find(unitOffsetKey(unit));
  if (found == refsByOffset_.end()) {
    return nullptr;
  }
  return &map_.refs[found->second];
}

std::optional<DefId> DefIndex::targetAt(support::Span unit) const {
  const NameRef* ref = refAt(unit);
  if (ref == nullptr || !ref->resolved()) {
    return std::nullopt;
  }
  return ref->target;
}

std::optional<DefId> DefIndex::defAtUnitOffset(support::Span unit) const {
  // A node with no unit range is not an answer, and must not be allowed to
  // *find* one: an invalid span packs to the same key as offset 0 of an invalid
  // file, which is a key a declaration at the start of a unit could occupy.
  if (!unit.valid() || unit.empty()) {
    return std::nullopt;
  }
  const auto found = defsByOffset_.find(unitOffsetKey(unit));
  if (found == defsByOffset_.end()) {
    return std::nullopt;
  }
  return found->second;
}

std::optional<DefId> DefIndex::defInsideWritten(support::Span written) const {
  for (std::size_t i = 0; i < map_.defs.size(); ++i) {
    const Def& def = map_.defs[i];
    if (isPredefined(def.predefined) || def.nameSpan.file != written.file) {
      continue;
    }
    // Containment and not equality: the node is one the preprocessor or the
    // parser synthesised, so its written range is the whole construct it stands
    // for and the declaration's name sits inside it. The *first* such
    // declaration wins, so the answer stays a function of the input rather than
    // of the map's order.
    if (def.nameSpan.begin >= written.begin && def.nameSpan.end <= written.end) {
      const DefId site = defIdOf(def, static_cast<std::uint32_t>(i));
      return canonicalOf(def, site);
    }
  }
  return std::nullopt;
}

std::optional<DefId> DefIndex::defAtName(const ast::LoweredFile& file, ast::AstId nameNode) const {
  if (!nameNode.valid()) {
    return std::nullopt;
  }
  const ast::Node& node = file.at(nameNode);
  if (const std::optional<DefId> exact = defAtUnitOffset(node.unit); exact.has_value()) {
    return exact;
  }
  return defInsideWritten(node.origin);
}

} // namespace minc::resolve
