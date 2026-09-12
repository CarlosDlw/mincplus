// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// A parsed file.
//
// The tree is a *view*: the green nodes live in the Session's arena, the leaf
// text is a view into the Session's source text, and the errors are a plain
// vector. So a `SyntaxTree` is only meaningful while its `Session` is alive,
// which is the intended lifetime -- the same one the sources have.
//
// Errors are here, not in the tree. An `Error` node says *where* the offending
// bytes are; this says *what* was wrong. A consumer that only wants structure
// never touches the diagnostics.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "parse/parse_error.h"
#include "support/span/file_id.h"
#include "syntax/green.h"
#include "syntax/node.h"

namespace minc::syntax {

// Counts and invariants recorded while the tree was built.
struct TreeStats {
  std::uint32_t nodeCount = 0;
  std::uint32_t tokenCount = 0;
  // Deepest path from the root, so a caller can see the recursion the parser
  // actually used (and the tests can assert the guard works).
  std::uint32_t maxDepth = 0;
  // True when the leaves tile the source exactly, byte for byte. Audited while
  // building, not assumed.
  bool lossless = false;
  // True when the parser gave up early: the tree still covers every byte, but
  // it ends in one `Error` node holding the unparsed remainder.
  bool bailedOut = false;
};

class SyntaxTree {
public:
  SyntaxTree() = default;
  SyntaxTree(const GreenNode* root, std::string_view text, support::FileId file,
             std::uint32_t revision, std::vector<parse::ParseError> errors, TreeStats stats)
      : root_(root), text_(text), file_(file), revision_(revision), errors_(std::move(errors)),
        stats_(stats) {}

  [[nodiscard]] const GreenNode* greenRoot() const {
    return root_;
  }
  [[nodiscard]] SyntaxNode root() const {
    return SyntaxNode(root_, 0);
  }
  [[nodiscard]] std::string_view text() const {
    return text_;
  }
  [[nodiscard]] support::FileId file() const {
    return file_;
  }
  // Byte offsets are only meaningful within one revision, so this travels with
  // the tree and with every cache key derived from it.
  [[nodiscard]] std::uint32_t revision() const {
    return revision_;
  }

  [[nodiscard]] const std::vector<parse::ParseError>& errors() const {
    return errors_;
  }
  [[nodiscard]] bool hasErrors() const {
    return !errors_.empty();
  }
  [[nodiscard]] const TreeStats& stats() const {
    return stats_;
  }

  // True when the leaves tile [0, size()) exactly and every node's width is the
  // sum of its children's. This is the tree-level version of the token stream's
  // `lossless()`: it is what a formatter, a refactor, or a language server would
  // rely on, so it is checked rather than asserted in prose.
  [[nodiscard]] bool validate() const;

  // The canonical text of the tree: concatenating every leaf's text. Equal to
  // `text()` whenever `stats().lossless`, which is what makes the tree usable
  // as a proxy for the source.
  [[nodiscard]] std::string reconstruct() const;

private:
  const GreenNode* root_ = nullptr;
  std::string_view text_;
  support::FileId file_ = support::kInvalidFile;
  std::uint32_t revision_ = 0;
  std::vector<parse::ParseError> errors_;
  TreeStats stats_;
};

} // namespace minc::syntax
