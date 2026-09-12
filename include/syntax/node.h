// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Cursors over the green tree: the layer that adds back what a shared node
// cannot store.
//
// A `SyntaxNode` or `SyntaxToken` is a cheap value -- a green pointer plus an
// absolute offset -- computed on the way down from the root. Nothing is
// allocated and nothing is cached, so a traversal pays only for the path it
// walks. The price is that a node does not know its parent; identity is
// `(file, byte range)`, never a pointer, because the same green node may have
// several parents.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "parse/syntax_kind.h"
#include "syntax/green.h"

namespace minc::syntax {

struct SyntaxToken {
  const GreenToken* green = nullptr;
  std::uint32_t offset = 0;

  [[nodiscard]] parse::SyntaxKind kind() const {
    return green != nullptr ? green->kind : parse::SyntaxKind::Error;
  }
  [[nodiscard]] std::string_view text() const {
    return green != nullptr ? green->text : std::string_view{};
  }
  [[nodiscard]] std::uint32_t width() const {
    return green != nullptr ? green->width() : 0;
  }
  [[nodiscard]] std::uint32_t end() const {
    return offset + width();
  }
  [[nodiscard]] bool isTrivia() const;
  [[nodiscard]] bool is(parse::SyntaxKind other) const {
    return kind() == other;
  }
};

class SyntaxNode {
public:
  SyntaxNode() = default;
  SyntaxNode(const GreenNode* green, std::uint32_t offset) : green_(green), offset_(offset) {}

  [[nodiscard]] const GreenNode* green() const {
    return green_;
  }
  [[nodiscard]] bool valid() const {
    return green_ != nullptr;
  }
  [[nodiscard]] parse::SyntaxKind kind() const {
    return green_ != nullptr ? green_->kind : parse::SyntaxKind::Error;
  }
  [[nodiscard]] std::uint32_t offset() const {
    return offset_;
  }
  [[nodiscard]] std::uint32_t width() const {
    return green_ != nullptr ? green_->width : 0;
  }
  [[nodiscard]] std::uint32_t end() const {
    return offset_ + width();
  }

  // Every child, leaves included, in source order. A `NodeOrToken` because the
  // tree is homogeneous: a walk that wants only nodes has to say so.
  [[nodiscard]] std::size_t childCount() const;
  [[nodiscard]] class NodeOrToken child(std::size_t index) const;

  // First interior child of this kind, or nullopt.
  [[nodiscard]] std::optional<SyntaxNode> childOfKind(parse::SyntaxKind kind) const;
  // First leaf of this kind among the immediate children.
  [[nodiscard]] std::optional<SyntaxToken> tokenOfKind(parse::SyntaxKind kind) const;
  [[nodiscard]] bool hasChild(parse::SyntaxKind kind) const;

  // Interior children only, in order.
  [[nodiscard]] std::vector<SyntaxNode> nodeChildren() const;

private:
  const GreenNode* green_ = nullptr;
  std::uint32_t offset_ = 0;
};

// A child of a node, which may be a leaf or an interior node.
class NodeOrToken {
public:
  NodeOrToken() = default;

  [[nodiscard]] static NodeOrToken ofNode(SyntaxNode node);
  [[nodiscard]] static NodeOrToken ofToken(SyntaxToken token);

  [[nodiscard]] bool isToken() const {
    return !isNode_;
  }
  [[nodiscard]] parse::SyntaxKind kind() const;
  [[nodiscard]] std::uint32_t offset() const;
  [[nodiscard]] std::uint32_t width() const;
  [[nodiscard]] std::uint32_t end() const;

  [[nodiscard]] const SyntaxNode& asNode() const {
    return node_;
  }
  [[nodiscard]] const SyntaxToken& asToken() const {
    return token_;
  }

private:
  bool isNode_ = false;
  SyntaxNode node_;
  SyntaxToken token_;
};

} // namespace minc::syntax
