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

void checkBinding(const LoweredFile& file, std::uint32_t index, std::vector<AstError>& out) {
  const Node& node = file.nodes()[index];
  const AstId stmt{index};
  const bool hasType = file.childOfKind(stmt, NodeKind::Type).valid();
  // "Is there an initializer" is the file's own reader (`LoweredFile::initializerOf`)
  // and not a second walk here: it is the same question the flow pass and the two
  // lowering paths ask, and it has to answer the same way for a plain binding and
  // for a destructuring one.
  const bool hasInit = file.initializerOf(stmt).valid();
  if (!hasType && !hasInit) {
    // One sentence for both spellings, and the fix is the same in both: write the
    // type, or write the value. A pattern that names two bindings and gives
    // neither a type nor a value has nothing to take its members from.
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
  // Which nodes are file-scope declarations. The item tree already says so, and
  // asking it is what keeps "is this binding an item?" from becoming a second rule
  // about nesting -- one list of flags, built once, for the scan below.
  std::vector<bool> isItem(nodes.size(), false);
  for (const Item& item : file.items().items) {
    if (item.node < isItem.size()) {
      isItem[item.node] = true;
    }
  }
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
    case NodeKind::ConstStmt: {
      checkBinding(file, index, out);
      // A pattern is a block-scope form. At file scope the declaration is one
      // object with one name -- which is the shape the item table has -- so a
      // destructuring there is refused where the item is, rather than becoming a
      // declaration no later stage can name (`tuples.md`, decision 6).
      if (isItem[index] && file.childOfKind(AstId{index}, NodeKind::TuplePattern).valid()) {
        out.push_back(AstError{node.origin,
                               "a destructuring cannot be a file-scope declaration: a name at "
                               "file scope is one object with one value. Write one binding per "
                               "name, as in `const W = 16; const H = 9;`",
                               AstErrorCode::PatternAtFileScope});
      }
      break;
    }
    default:
      break;
    }
  }
  return out;
}

} // namespace minc::ast
