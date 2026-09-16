// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "ast/ast.h"

#include <cstddef>
#include <span>
#include <utility>

#include "lex/token_kind.h"
#include "parse/syntax_kind.h"

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

SliceParts LoweredFile::slicePartsOf(AstId id) const {
  SliceParts parts;
  if (!id.valid() || id.index >= nodes_.size()) {
    return parts;
  }
  // The separator is two adjacent `Dot` tokens: the lexer has no `..` of its
  // own, and inventing one would be a token the parser asked for rather than a
  // spelling the language has. Walking the children in order is therefore also
  // the walk that splits the operands, and the two halves are "before the dots"
  // and "after them" -- nothing else.
  constexpr parse::SyntaxKind kDot = parse::toSyntaxKind(lex::TokenKind::Dot);
  const std::span<const AstId> children = childrenOf(id);
  bool pastSeparator = false;
  for (std::size_t i = 0; i < children.size(); ++i) {
    const Node& node = at(children[i]);
    if (node.isToken()) {
      if (!pastSeparator && node.kind == kDot && i + 1 < children.size() &&
          at(children[i + 1]).isToken() && at(children[i + 1]).kind == kDot) {
        pastSeparator = true;
      }
      continue;
    }
    if (!pastSeparator) {
      if (!parts.base.valid()) {
        parts.base = children[i];
      } else {
        parts.begin = children[i];
      }
    } else if (!parts.end.valid()) {
      parts.end = children[i];
    }
  }
  return parts;
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
