// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The chain of macro invocations a token came out of.
//
// One frame per *invocation site*, not per expansion: a macro invoked inside a
// loop in a header is one frame, and its four hundred expansions share it. That
// is why the table is hash-consed on `(macro, invocation, parent, define)` --
// the same structural-sharing idea as the syntax tree's green nodes, applied to
// provenance instead of syntax.
//
// Frame 0 is the root (`kNoExpansion`): "written directly in the file being
// read". It exists so that every question about a chain is total -- there is no
// null case to special-case at each call site.
//
// `parent` is always a *smaller* id, because a frame is only ever created while
// its parent exists. That single property is what makes a walk terminate
// without a visited set.
//
// Design record: `docs/architectures/preprocessor.md`.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

#include "pp/pp_token.h"
#include "support/intern/sym_id.h"

namespace minc::pp {

struct ExpansionFrame {
  // The macro being expanded. `kInvalidSym` on the root frame.
  support::SymId macro = support::kInvalidSym;
  // Where the invocation starts: the caret for "invoked here".
  SourceLoc invocation;
  // The frame this invocation happened inside, or `kNoExpansion` for the file.
  ExpansionId parent = kNoExpansion;
  // Where the macro was defined: the caret for "defined here".
  SourceLoc define;
};

class ExpansionTable {
public:
  ExpansionTable();

  // Returns the id for `frame`, creating it if it is new. Identical frames get
  // one id, so the table is proportional to distinct invocation sites.
  [[nodiscard]] ExpansionId intern(const ExpansionFrame& frame);

  // Total for every id: `kNoExpansion` is the root frame, and an out-of-range
  // id returns the root rather than reading out of bounds. A caller that needs
  // to know whether an id exists asks `valid`.
  [[nodiscard]] const ExpansionFrame& at(ExpansionId id) const;

  [[nodiscard]] bool valid(ExpansionId id) const {
    return id < frames_.size();
  }

  // Length of the chain, root included, so a file-level token reports 1.
  [[nodiscard]] std::uint32_t depth(ExpansionId id) const;

  // The chain from `id` to the root, innermost first, appended to `out`.
  // Terminates by construction (see the note about `parent` above) and also
  // stops at `support::kMaxExpansionDepth`, so a corrupted table cannot hang a
  // diagnostic.
  void chain(ExpansionId id, std::vector<ExpansionId>& out) const;

  // Frames stored, root included.
  [[nodiscard]] std::size_t size() const {
    return frames_.size();
  }
  // Distinct invocation sites, i.e. `size() - 1`.
  [[nodiscard]] std::size_t sites() const {
    return frames_.empty() ? 0 : frames_.size() - 1;
  }

  void clear();

private:
  struct FrameKey {
    support::SymId macro = support::kInvalidSym;
    SourceLoc invocation;
    ExpansionId parent = kNoExpansion;
    SourceLoc define;

    [[nodiscard]] bool operator==(const FrameKey& other) const {
      return macro == other.macro && invocation.file == other.invocation.file &&
             invocation.offset == other.invocation.offset && parent == other.parent &&
             define.file == other.define.file && define.offset == other.define.offset;
    }
  };
  struct FrameKeyHash {
    [[nodiscard]] std::size_t operator()(const FrameKey& key) const;
  };

  std::deque<ExpansionFrame> frames_;
  std::unordered_map<FrameKey, ExpansionId, FrameKeyHash> index_;
};

} // namespace minc::pp
