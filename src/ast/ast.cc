// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "ast/ast.h"

#include <cstddef>
#include <utility>

namespace minc::ast {

LoweredFile LoweredFile::make(support::FileId file, std::uint32_t revision,
                              std::string_view unitText, std::vector<Node> nodes,
                              std::vector<AstId> children, ItemTree items) {
  LoweredFile out;
  out.file_ = file;
  out.revision_ = revision;
  out.unitText_ = unitText;
  out.nodes_ = std::move(nodes);
  out.children_ = std::move(children);
  out.items_ = std::move(items);
  return out;
}

AstId LoweredFile::childOfKind(AstId id, NodeKind kind) const {
  for (const AstId child : childrenOf(id)) {
    if (at(child).kind == kind) {
      return child;
    }
  }
  return AstId{};
}

std::vector<AstId> LoweredFile::childrenOfKind(AstId id, NodeKind kind) const {
  std::vector<AstId> out;
  for (const AstId child : childrenOf(id)) {
    if (at(child).kind == kind) {
      out.push_back(child);
    }
  }
  return out;
}

std::string_view LoweredFile::spellingOf(const Node& node) const {
  const std::uint32_t begin = node.unit.begin;
  const std::uint32_t end = node.unit.end;
  if (begin > unitText_.size() || end > unitText_.size() || end < begin) {
    return {};
  }
  return unitText_.substr(begin, end - begin);
}

std::string_view LoweredFile::spellingOf(AstId id) const {
  if (!id.valid() || id.index >= nodes_.size()) {
    return {};
  }
  return spellingOf(nodes_[id.index]);
}

} // namespace minc::ast
