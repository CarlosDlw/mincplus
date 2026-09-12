// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The keyed store of parsed trees.
//
// One store per compilation, the way `SourceManager` is one per session. It
// owns the node cache -- which is what makes sharing real rather than
// per-tree -- and it keys each tree by `(FileId, revision)`, for the same
// reason spans are revision-scoped: byte offsets do not survive an edit.
//
// Why this lives above `syntax` instead of in `support`: `support` is
// syntax-free by contract (see the layering rules in `docs/architecture.md`),
// so the component that owns parsed trees cannot live below the thing it
// stores. The arena it builds into is still the session's, so there is one
// arena per compilation rather than one per module.
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "lex/token_stream.h"
#include "support/mem/arena.h"
#include "support/span/file_id.h"
#include "syntax/green.h"
#include "syntax/tree.h"

namespace minc::syntax {

class TreeStore {
public:
  explicit TreeStore(support::Arena& arena) : cache_(arena) {}

  TreeStore(const TreeStore&) = delete;
  TreeStore& operator=(const TreeStore&) = delete;

  // Parses `stream` and keeps the result. A newer revision of a file replaces
  // the old tree instead of holding both, because the old one's offsets no
  // longer refer to anything in the file the editor has.
  //
  // Returns nullptr only when the arena is exhausted. The pointer stays valid
  // until the file is re-parsed, dropped, or cleared -- which is what lets a
  // caller hold it across a whole command line.
  [[nodiscard]] const SyntaxTree* parse(const lex::TokenStream& stream, std::uint32_t revision);

  [[nodiscard]] const SyntaxTree* find(support::FileId file) const;

  // Drops one file's tree. False when there was none, so a caller cannot think
  // it invalidated something that was never there.
  bool drop(support::FileId file);

  void clear();

  [[nodiscard]] std::size_t size() const {
    return trees_.size();
  }

  // Exposed so a caller can see how much sharing is happening, and so a test
  // can assert it instead of trusting the comment above.
  [[nodiscard]] const GreenCache& cache() const {
    return cache_;
  }

private:
  GreenCache cache_;
  std::unordered_map<support::FileId, SyntaxTree> trees_;
};

} // namespace minc::syntax
