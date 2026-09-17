// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "syntax/node.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace minc::syntax {

bool SyntaxToken::isTrivia() const {
  if (green == nullptr || !parse::isTokenKind(green->kind)) {
    return false;
  }
  return lex::isTrivia(parse::toTokenKind(green->kind));
}

std::size_t SyntaxNode::childCount() const {
  return green_ != nullptr ? green_->children.size() : 0;
}

NodeOrToken SyntaxNode::child(std::size_t index) const {
  // **O(1)**, because the child's offset within its parent is stored beside it
  // (`green.h`). It used to be O(index) -- the widths of every child before this
  // one summed on the way -- which made the natural walk
  // `for (i < childCount()) child(i)` quadratic on a wide node.
  if (green_ == nullptr || index >= green_->children.size()) {
    return NodeOrToken{};
  }
  const GreenChild& child = green_->children[index];
  const std::uint32_t offset = offset_ + child.offset;
  if (child.isNode) {
    return NodeOrToken::ofNode(SyntaxNode(child.node, offset));
  }
  return NodeOrToken::ofToken(SyntaxToken{child.token, offset});
}

std::optional<SyntaxNode> SyntaxNode::childOfKind(parse::SyntaxKind kind) const {
  if (green_ == nullptr) {
    return std::nullopt;
  }
  for (const GreenChild& child : green_->children) {
    if (child.isNode && child.node->kind == kind) {
      return SyntaxNode(child.node, offset_ + child.offset);
    }
  }
  return std::nullopt;
}

std::optional<SyntaxToken> SyntaxNode::tokenOfKind(parse::SyntaxKind kind) const {
  if (green_ == nullptr) {
    return std::nullopt;
  }
  for (const GreenChild& child : green_->children) {
    if (!child.isNode && child.token->kind == kind) {
      return SyntaxToken{child.token, offset_ + child.offset};
    }
  }
  return std::nullopt;
}

bool SyntaxNode::hasChild(parse::SyntaxKind kind) const {
  return childOfKind(kind).has_value();
}

std::vector<SyntaxNode> SyntaxNode::nodeChildren() const {
  std::vector<SyntaxNode> out;
  if (green_ == nullptr) {
    return out;
  }
  out.reserve(green_->children.size());
  for (const GreenChild& child : green_->children) {
    if (child.isNode) {
      out.emplace_back(child.node, offset_ + child.offset);
    }
  }
  return out;
}

NodeOrToken NodeOrToken::ofNode(SyntaxNode node) {
  NodeOrToken out;
  out.isNode_ = true;
  out.node_ = node;
  return out;
}

NodeOrToken NodeOrToken::ofToken(SyntaxToken token) {
  NodeOrToken out;
  out.isNode_ = false;
  out.token_ = token;
  return out;
}

parse::SyntaxKind NodeOrToken::kind() const {
  return isNode_ ? node_.kind() : token_.kind();
}

std::uint32_t NodeOrToken::offset() const {
  return isNode_ ? node_.offset() : token_.offset;
}

std::uint32_t NodeOrToken::width() const {
  return isNode_ ? node_.width() : token_.width();
}

std::uint32_t NodeOrToken::end() const {
  return offset() + width();
}

} // namespace minc::syntax
