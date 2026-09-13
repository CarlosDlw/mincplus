// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "resolve/store.h"

#include <cstdint>
#include <unordered_map>
#include <utility>

#include "ast/ast.h"
#include "resolve/map.h"
#include "resolve/resolve.h"
#include "support/intern/interner.h"
#include "support/span/file_id.h"

namespace minc::resolve {

const ResolveOutput* ResolveStore::outputFor(support::FileId file, std::uint32_t revision,
                                             const ast::LoweredFile& lowered,
                                             support::Interner& symbols, ResolveOptions options) {
  const auto it = entries_.find(file);
  if (it != entries_.end() && it->second.revision == revision &&
      it->second.items == lowered.items()) {
    // The revision matches and the signatures are identical: whatever changed in
    // that file, it was not the set of visible names, so the scopes still stand.
    ++stats_.hits;
    return &it->second.output;
  }

  ++stats_.misses;
  Entry entry;
  entry.revision = revision;
  entry.items = lowered.items();
  entry.output = resolveUnit(lowered, symbols, options);

  // In place when the key exists (the node, and therefore the pointer handed out
  // before, stays put), inserted otherwise.
  const auto [slot, inserted] = entries_.insert_or_assign(file, std::move(entry));
  (void)inserted;
  return &slot->second.output;
}

const ResolveOutput* ResolveStore::find(support::FileId file) const {
  const auto it = entries_.find(file);
  return it == entries_.end() ? nullptr : &it->second.output;
}

bool ResolveStore::drop(support::FileId file) {
  return entries_.erase(file) != 0;
}

void ResolveStore::clear() {
  entries_.clear();
  stats_ = Stats{};
}

} // namespace minc::resolve
