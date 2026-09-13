// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// A scope: the names a region introduces, one table per namespace.
//
// Two structures per namespace, and the split is deliberate:
//
//   * `names` is in **declaration order** and is what iteration uses -- a
//     diagnostic, a dump and a suggestion all have to be a function of the
//     input, not of the insertion order of a container.
//   * `byName` is keyed by `SymId` and is what lookup uses. It is never iterated
//     for output, so its order is nobody's business.
//
// A hash index rather than a sorted vector because the file scope of a large
// unit is written to a million times and a linear insert would be quadratic --
// a hazard bound is a diagnostic, and this is one of the places a naive
// structure turns one into a hang.
#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "ast/node.h"
#include "resolve/def.h"
#include "support/intern/sym_id.h"
#include "support/span/span.h"

namespace minc::resolve {

struct Scope {
  // Invalid for the file scope, which has no parent.
  ScopeId parent;
  ScopeKind kind = ScopeKind::File;
  support::Span span;
  // The lowered node that opened this scope, or `ast::kInvalidAst` for the file
  // scope. Kept so `source_to_def` and a future "scope at this position" query
  // do not have to re-derive which node a scope came from.
  std::uint32_t node = ast::kInvalidAst;
  // Iteration order: exactly the order the declarations were read in.
  std::array<std::vector<DefId>, kNamespaceCount> names;
  // Lookup only; never iterated for output. Named `byName` rather than `index`
  // so it cannot be mistaken for a `ScopeId`'s integer index.
  std::array<std::unordered_map<support::SymId, DefId>, kNamespaceCount> byName;

  [[nodiscard]] std::vector<DefId>& table(Namespace ns) {
    return names[static_cast<std::size_t>(ns)];
  }
  [[nodiscard]] const std::vector<DefId>& table(Namespace ns) const {
    return names[static_cast<std::size_t>(ns)];
  }
};

} // namespace minc::resolve
