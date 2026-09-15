// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "resolve/source_to_def.h"

#include <cstdint>
#include <optional>

#include "ast/node.h"
#include "resolve/def.h"
#include "resolve/def_index.h"
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
      best = defIdOf(def, index);
      bestSize = size;
    }
  }
  return best;
}

std::optional<DefId> defOfNameNode(const DefMap& map, const ast::LoweredFile& file,
                                   ast::AstId nameNode) {
  // The rule -- the node's unit offset first, its written span second -- is
  // `DefIndex`'s, and this builds one rather than repeating it. The cost is the
  // scan this function used to do, paid once per query; a caller that asks about
  // many nodes (the language server, an item map) holds a `DefIndex` and asks it
  // directly.
  return DefIndex(map).defAtName(file, nameNode);
}

} // namespace minc::resolve
