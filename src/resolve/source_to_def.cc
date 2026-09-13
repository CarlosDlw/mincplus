// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "resolve/source_to_def.h"

#include <cstdint>
#include <optional>

#include "ast/node.h"
#include "resolve/def.h"
#include "resolve/map.h"
#include "support/span/file_id.h"
#include "support/span/span.h"

namespace minc::resolve {

std::optional<DefId> defAt(const DefMap& map, support::FileId file, std::uint32_t offset) {
  std::optional<DefId> best;
  std::uint32_t bestSize = 0;
  for (std::uint32_t index = 0; index < map.defs.size(); ++index) {
    const Def& def = map.defs[index];
    if (def.nameSpan.file != file || !def.nameSpan.contains(offset)) {
      continue;
    }
    const std::uint32_t size = def.nameSpan.size();
    // Innermost wins: the smallest span that contains the position. Strictly
    // smaller, so equal spans keep the earlier declaration -- determinism, not a
    // coin flip.
    if (!best.has_value() || size < bestSize) {
      best = DefId{file, index};
      bestSize = size;
    }
  }
  return best;
}

std::optional<DefId> defOfNameNode(const DefMap& map, const ast::LoweredFile& file,
                                   ast::AstId nameNode) {
  if (!nameNode.valid()) {
    return std::nullopt;
  }
  const support::Span span = file.at(nameNode).origin;
  for (std::uint32_t index = 0; index < map.defs.size(); ++index) {
    if (map.defs[index].nameSpan == span) {
      return DefId{span.file, index};
    }
  }
  return std::nullopt;
}

} // namespace minc::resolve
