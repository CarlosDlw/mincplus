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
namespace {

// The identity of the definition at `index`.
//
// A `DefId` carries the file the declaration was *written* in -- a header's
// definition stays a header's definition even though a unit's text is one buffer
// -- so it is read from the def and not from the file the query was about. One
// rule, in one place, because the two lookups below both need it and a second
// copy of it is a second chance to spell it wrong.
[[nodiscard]] DefId idOf(const Def& def, std::uint32_t index) {
  const support::FileId owner =
      def.nameSpan.file != support::kInvalidFile ? def.nameSpan.file : def.span.file;
  return DefId{owner, index};
}

} // namespace

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
      best = idOf(def, index);
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
  const ast::Node& node = file.at(nameNode);

  // Identity first. The unit range is one token per name, so it still tells two
  // declarations apart when the preprocessor gave them one written location --
  // which is not a corner case but what a macro that expands one argument into
  // two names does:
  //
  //   #define PAIR(b) let b: i32; let CONCAT(b, 2): i32;
  //
  // Both names then answer to the *same* written span, and a lookup on it
  // answers with the wrong declaration silently: the second name's type is never
  // recorded, and a use of it is the poison with no diagnostic at all.
  if (node.unit.valid() && !node.unit.empty()) {
    for (std::uint32_t index = 0; index < map.defs.size(); ++index) {
      if (map.defs[index].unitSpan == node.unit) {
        return idOf(map.defs[index], index);
      }
    }
  }

  // A synthetic declaration -- a name the parser or the preprocessor inserted --
  // can have no unit range to match, and then the written span is the only
  // handle. It is exact whenever nothing was expanded, so this is the fallback
  // and not the rule.
  for (std::uint32_t index = 0; index < map.defs.size(); ++index) {
    if (map.defs[index].nameSpan == node.origin) {
      return idOf(map.defs[index], index);
    }
  }
  return std::nullopt;
}

} // namespace minc::resolve
