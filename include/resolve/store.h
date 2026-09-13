// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The revision-keyed cache of resolved units.
//
// One store per compilation, for the same reason `TreeStore` is one per
// compilation: a cache that lived per command would be a cache for nobody. It
// keys on `(FileId, revision)`, because byte offsets do not survive an edit, and
// it additionally compares the **item tree** -- by hash to find candidates and
// by `==` to confirm -- because that is what makes the editor invariant pay off:
//
//   edit a function body
//     -> that file's lowered AST changes, its item tree does not
//        -> the cached def map is still correct and is returned untouched
//
// A *signature* edit changes the item tree, so the unit is resolved again --
// which is correct, because the set of visible names really did change. A hash
// alone would be a correctness bug waiting for a collision, and this structure
// decides whether a stale scope can be handed to the editor, so it is not the
// place for a probabilistic answer.
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "ast/ast.h"
#include "resolve/map.h"
#include "resolve/resolve.h"
#include "support/intern/interner.h"
#include "support/span/file_id.h"

namespace minc::resolve {

class ResolveStore {
public:
  ResolveStore() = default;
  ResolveStore(const ResolveStore&) = delete;
  ResolveStore& operator=(const ResolveStore&) = delete;

  struct Stats {
    std::size_t hits = 0;   // answered from the cache
    std::size_t misses = 0; // resolved, then stored
  };

  // The unit's resolution, reusing the cached one when the revision and the
  // item tree are both unchanged.
  //
  // The returned pointer is stable until this file is dropped or the store is
  // cleared: the entries live in an `unordered_map`'s nodes, so adding another
  // file never moves one.
  [[nodiscard]] const ResolveOutput* outputFor(support::FileId file, std::uint32_t revision,
                                               const ast::LoweredFile& lowered,
                                               support::Interner& symbols,
                                               ResolveOptions options = {});

  [[nodiscard]] const ResolveOutput* find(support::FileId file) const;

  // Drops one file's entry. False when there was none, so a caller cannot think
  // it invalidated something that was never there.
  bool drop(support::FileId file);
  void clear();

  [[nodiscard]] const Stats& stats() const {
    return stats_;
  }
  [[nodiscard]] std::size_t size() const {
    return entries_.size();
  }

private:
  struct Entry {
    std::uint32_t revision = 0;
    ast::ItemTree items;
    ResolveOutput output;
  };

  std::unordered_map<support::FileId, Entry> entries_;
  Stats stats_;
};

} // namespace minc::resolve
