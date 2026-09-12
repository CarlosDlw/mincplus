// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "pp/expansion_table.h"

#include <cstdint>
#include <utility>

#include "support/limits.h"

namespace minc::pp {

std::size_t ExpansionTable::FrameKeyHash::operator()(const FrameKey& key) const {
  // A small FNV-style mix. The fields are already integers, so this only has to
  // spread them; it does not have to be cryptographic.
  std::uint64_t hash = 1469598103934665603ULL;
  const auto mix = [&hash](std::uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ULL;
  };
  mix(static_cast<std::uint64_t>(key.macro));
  mix(key.invocation.file);
  mix(key.invocation.offset);
  mix(key.invocation.length);
  mix(key.parent);
  mix(key.define.file);
  mix(key.define.offset);
  return static_cast<std::size_t>(hash);
}

ExpansionTable::ExpansionTable() {
  // The root: "written here, not expanded". It exists so that `at` and `chain`
  // are total and no caller has to special-case "no expansion".
  frames_.push_back(ExpansionFrame{});
}

ExpansionId ExpansionTable::intern(const ExpansionFrame& frame) {
  const FrameKey key{frame.macro, frame.invocation, frame.parent, frame.define};
  const auto existing = index_.find(key);
  if (existing != index_.end()) {
    return existing->second;
  }

  // `frames_` is a deque, so pushing never invalidates the returned references
  // that the expander holds for the frames it is currently expanding.
  const auto id = static_cast<ExpansionId>(frames_.size());
  frames_.push_back(frame);
  index_.emplace(key, id);
  return id;
}

const ExpansionFrame& ExpansionTable::at(ExpansionId id) const {
  // The root for anything that is not a real frame: a corrupted or stale id
  // yields "no expansion", which renders as "written here" rather than reading
  // past the end of the table.
  return id < frames_.size() ? frames_[id] : frames_[0];
}

std::uint32_t ExpansionTable::depth(ExpansionId id) const {
  std::uint32_t count = 1;
  while (id != kNoExpansion && count <= support::kMaxExpansionDepth) {
    id = at(id).parent;
    ++count;
  }
  return count;
}

void ExpansionTable::chain(ExpansionId id, std::vector<ExpansionId>& out) const {
  // Two independent stops: the parent link is strictly decreasing (so the walk
  // is finite), and the depth cap means a table that was somehow fed a cycle
  // costs a bounded amount of work instead of hanging the diagnostic that is
  // trying to explain another failure.
  std::uint32_t steps = 0;
  while (id != kNoExpansion && steps < support::kMaxExpansionDepth) {
    out.push_back(id);
    id = at(id).parent;
    ++steps;
  }
}

void ExpansionTable::clear() {
  frames_.clear();
  index_.clear();
  frames_.push_back(ExpansionFrame{});
}

} // namespace minc::pp
