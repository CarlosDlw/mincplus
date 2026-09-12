// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "syntax/tree.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace minc::syntax {

std::string SyntaxTree::reconstruct() const {
  std::string out;
  out.reserve(text_.size());
  if (root_ == nullptr) {
    return out;
  }

  // Iterative, because a tree produced by the parser can be as deep as the
  // parser's own guard allows and this must not be the thing that overflows.
  struct Frame {
    const GreenNode* node;
    std::size_t next;
  };
  std::vector<Frame> stack;
  stack.push_back({root_, 0});
  while (!stack.empty()) {
    Frame& frame = stack.back();
    if (frame.next >= frame.node->children.size()) {
      stack.pop_back();
      continue;
    }
    const GreenChild& child = frame.node->children[frame.next];
    ++frame.next;
    if (child.isNode) {
      stack.push_back({child.node, 0});
    } else {
      out.append(child.token->text);
    }
  }
  return out;
}

bool SyntaxTree::validate() const {
  if (root_ == nullptr) {
    return false;
  }

  // A node's width must be exactly the sum of its children's, and no child may
  // be null. The cache makes sharing common, so a mistake there would otherwise
  // show up as a wrong offset far from its cause.
  std::vector<const GreenNode*> stack;
  stack.push_back(root_);
  while (!stack.empty()) {
    const GreenNode* node = stack.back();
    stack.pop_back();
    std::uint32_t width = 0;
    for (const GreenChild& child : node->children) {
      if (child.isNode) {
        if (child.node == nullptr) {
          return false;
        }
        stack.push_back(child.node);
      } else if (child.token == nullptr) {
        return false;
      }
      width += child.width();
    }
    if (width != node->width) {
      return false;
    }
  }

  // The tree is a faithful representation of the source only if its leaves
  // reproduce it byte for byte. That is the guarantee everything downstream
  // leans on, so it is checked here rather than assumed from `stats()`.
  return reconstruct() == text_;
}

} // namespace minc::syntax
