// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "ast/validate.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ast/node.h"
#include "support/span/span.h"

namespace minc::ast {
namespace {

// Is there an initializer? The shape is `Name [Type] [expr] ';'`, so the answer
// is "is there an interior child that is neither the name nor the annotation".
// Asking the node kinds rather than listing expression kinds keeps this from
// needing a second list to keep in sync with the grammar.
[[nodiscard]] bool hasInitializer(const LoweredFile& file, AstId stmt) {
  for (const AstId child : file.childrenOf(stmt)) {
    const Node& node = file.at(child);
    if (node.isToken()) {
      continue;
    }
    if (node.kind == NodeKind::Name || node.kind == NodeKind::Type) {
      continue;
    }
    return true;
  }
  return false;
}

void checkBinding(const LoweredFile& file, std::uint32_t index, std::vector<AstError>& out) {
  const Node& node = file.nodes()[index];
  const AstId stmt{index};
  const bool hasType = file.childOfKind(stmt, NodeKind::Type).valid();
  const bool hasInit = hasInitializer(file, stmt);
  if (!hasType && !hasInit) {
    out.push_back(AstError{node.origin, "a binding needs a type or an initializer",
                           AstErrorCode::MissingTypeOrInitializer});
    return;
  }
  if (node.kind == NodeKind::ConstStmt && !hasInit) {
    out.push_back(AstError{node.origin, "a 'const' needs an initializer: it can never be assigned",
                           AstErrorCode::ConstantWithoutInitializer});
  }
}

} // namespace

std::vector<AstError> validate(const LoweredFile& file) {
  std::vector<AstError> out;
  const std::vector<Node>& nodes = file.nodes();
  // The node array is the pre-order of a source-ordered tree, so this single
  // scan visits the declarations in the order a reader meets them. No second
  // walk, and no sorting step that could put two diagnostics at one offset in
  // an implementation-defined order.
  for (std::uint32_t index = 0; index < nodes.size(); ++index) {
    const Node& node = nodes[index];
    if (node.inError) {
      continue; // the parser already reported this region
    }
    switch (node.kind) {
    case NodeKind::LetStmt:
    case NodeKind::ConstStmt:
      checkBinding(file, index, out);
      break;
    default:
      break;
    }
  }
  return out;
}

} // namespace minc::ast
