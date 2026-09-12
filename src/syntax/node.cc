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
  if (green_ == nullptr) {
    return NodeOrToken{};
  }
  std::uint32_t offset = offset_;
  for (std::size_t i = 0; i < green_->children.size(); ++i) {
    const GreenChild& child = green_->children[i];
    if (i == index) {
      if (child.isNode) {
        return NodeOrToken::ofNode(SyntaxNode(child.node, offset));
      }
      return NodeOrToken::ofToken(SyntaxToken{child.token, offset});
    }
    offset += child.width();
  }
  return NodeOrToken{};
}

std::optional<SyntaxNode> SyntaxNode::childOfKind(parse::SyntaxKind kind) const {
  if (green_ == nullptr) {
    return std::nullopt;
  }
  std::uint32_t offset = offset_;
  for (const GreenChild& child : green_->children) {
    if (child.isNode && child.node->kind == kind) {
      return SyntaxNode(child.node, offset);
    }
    offset += child.width();
  }
  return std::nullopt;
}

std::optional<SyntaxToken> SyntaxNode::tokenOfKind(parse::SyntaxKind kind) const {
  if (green_ == nullptr) {
    return std::nullopt;
  }
  std::uint32_t offset = offset_;
  for (const GreenChild& child : green_->children) {
    if (!child.isNode && child.token->kind == kind) {
      return SyntaxToken{child.token, offset};
    }
    offset += child.width();
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
  std::uint32_t offset = offset_;
  for (const GreenChild& child : green_->children) {
    if (child.isNode) {
      out.emplace_back(child.node, offset);
    } else {
      // Still advance: a token between two nodes must not shift their offsets.
      offset += child.width();
      continue;
    }
    offset += child.width();
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
