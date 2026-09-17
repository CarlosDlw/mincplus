// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The green tree: immutable, untyped, and position-free.
//
// A green node stores its kind, its width in bytes, and its children -- and
// deliberately no parent and no absolute offset, because the same node is
// shared by every place it appears and therefore has no single parent or
// position. A cursor (syntax/node.h) adds both back on demand.
//
// Tokens carry the source text as a view, so a leaf knows what it is without a
// second table. Leaves and interior nodes are distinct types but share the
// `SyntaxKind` tag space, so traversal that does not care which one it has does
// not have to branch.
//
// Everything here is allocated from an `Arena` and nothing is ever freed
// individually: a revision is released as a whole. Values are trivially
// destructible, which is exactly the arena's contract.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "parse/syntax_kind.h"
#include "support/mem/arena.h"

namespace minc::syntax {

struct GreenNode;

struct GreenToken {
  parse::SyntaxKind kind = parse::SyntaxKind::Error;
  // View into the source (or empty for a token recovery inserted).
  std::string_view text;

  [[nodiscard]] std::uint32_t width() const {
    return static_cast<std::uint32_t>(text.size());
  }
};

struct GreenChild {
  // Where this child starts inside its parent, in bytes: the sum of the widths
  // of the children before it. **Filled by the cache** when a node is interned
  // (`GreenCache::node`), and 0 in the array a caller passes to it -- a caller
  // builds children in order and has no reason to compute what the node it is
  // handing them to already will.
  //
  // It is here, and not computed on the way down, because the alternative is
  // quadratic: a cursor's `child(i)` had to sum the widths of every child before
  // `i`, so the natural loop `for (i < childCount()) child(i)` cost O(n^2) on a
  // *wide* node -- a product of a thousand members, an initializer list, an
  // argument list -- which is a shape real files have. One word per child, and
  // the walk is linear.
  //
  // It costs nothing: the struct is two pointers and a flag either way, and the
  // flag's padding is what this now uses.
  std::uint32_t offset = 0;
  bool isNode = false;
  const GreenNode* node = nullptr;
  const GreenToken* token = nullptr;

  [[nodiscard]] static GreenChild ofNode(const GreenNode* child) {
    GreenChild out;
    out.isNode = true;
    out.node = child;
    return out;
  }
  [[nodiscard]] static GreenChild ofToken(const GreenToken* child) {
    GreenChild out;
    out.isNode = false;
    out.token = child;
    return out;
  }
  [[nodiscard]] parse::SyntaxKind kind() const;
  [[nodiscard]] std::uint32_t width() const;
};

struct GreenNode {
  parse::SyntaxKind kind = parse::SyntaxKind::Error;
  // Sum of the children's widths; cached because it is summed constantly and
  // recomputing it would make an offset computation O(subtree).
  std::uint32_t width = 0;
  std::span<const GreenChild> children;
};

// Defined out of line and still in the header, and after `GreenNode` because it
// reads through the pointer: this is the hottest one-liner in the tree -- every
// walk asks every child for its width -- and an out-of-line call per child is
// what turned a walk of a wide node into a bottleneck rather than a walk.
inline std::uint32_t GreenChild::width() const {
  return isNode ? node->width : token->width();
}

// Hash-consing cache. Two identical subtrees come back as the *same pointer*,
// which is what makes the green structure a DAG rather than a tree and what
// lets an unchanged subtree be recognised later without comparing it.
//
// It lives beside the arena (in the tree store) rather than inside a single
// tree, so nodes are shared across files as well -- an empty `()` is one node
// no matter how many functions have one.
class GreenCache {
public:
  explicit GreenCache(support::Arena& arena) : arena_(&arena) {}

  GreenCache(const GreenCache&) = delete;
  GreenCache& operator=(const GreenCache&) = delete;

  // Both return nullptr when the arena is exhausted; callers must check.
  [[nodiscard]] const GreenToken* token(parse::SyntaxKind kind, std::string_view text);
  [[nodiscard]] const GreenNode* node(parse::SyntaxKind kind, std::span<const GreenChild> children);

  [[nodiscard]] std::size_t tokenCount() const {
    return tokens_.size();
  }
  [[nodiscard]] std::size_t nodeCount() const {
    return nodes_.size();
  }
  void clear();

private:
  struct TokenKey {
    std::uint16_t kind = 0;
    std::string_view text;
    [[nodiscard]] bool operator==(const TokenKey& other) const {
      return kind == other.kind && text == other.text;
    }
  };
  struct TokenKeyHash {
    [[nodiscard]] std::size_t operator()(const TokenKey& key) const;
  };

  // Owns the child pointers, because the span the caller passes is a view into
  // a builder-local buffer that is gone by the next call.
  struct NodeKey {
    std::uint16_t kind = 0;
    std::vector<const void*> children;
    [[nodiscard]] bool operator==(const NodeKey& other) const {
      return kind == other.kind && children == other.children;
    }
  };
  struct NodeKeyHash {
    [[nodiscard]] std::size_t operator()(const NodeKey& key) const;
  };

  support::Arena* arena_;
  std::unordered_map<TokenKey, const GreenToken*, TokenKeyHash> tokens_;
  std::unordered_map<NodeKey, const GreenNode*, NodeKeyHash> nodes_;
};

} // namespace minc::syntax
