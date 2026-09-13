// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "resolve/resolve.h"

#include <cstddef>
#include <optional>
#include <vector>

#include "resolve/def.h"
#include "resolve/map.h"
#include "resolve/scope.h"
#include "support/intern/sym_id.h"

namespace minc::resolve {
namespace {

// The bound on the scope-chain walk. The parents form a tree built by the
// resolver, so a cycle is impossible -- but a walk that trusted that would be a
// hang if the invariant were ever broken, and a bound costs one comparison. It
// is the same number the resolver uses when it refuses to open another scope.
[[nodiscard]] std::size_t depthLimit(const DefMap& map) {
  return map.maxScopeDepth;
}

[[nodiscard]] const DefId* findInScope(const Scope& scope, Namespace ns, support::SymId name) {
  const auto& byName = scope.byName[static_cast<std::size_t>(ns)];
  const auto it = byName.find(name);
  return it == byName.end() ? nullptr : &it->second;
}

} // namespace

std::optional<DefId> lookup(const DefMap& map, ScopeId from, Namespace ns, support::SymId name) {
  if (name == support::kInvalidSym) {
    return std::nullopt;
  }
  const std::size_t limit = depthLimit(map);
  ScopeId current = from;
  for (std::size_t depth = 0; current.valid() && depth < limit; ++depth) {
    // Defensive: a scope id that is not in this map is a bug in the caller, and
    // returning "not found" is better than indexing out of bounds.
    if (current.index >= map.scopes.size()) {
      return std::nullopt;
    }
    const Scope& scope = map.scopes[current.index];
    if (const DefId* hit = findInScope(scope, ns, name)) {
      return *hit;
    }
    current = scope.parent;
  }
  return std::nullopt;
}

std::optional<DefId> lookupOuter(const DefMap& map, ScopeId from, Namespace ns,
                                 support::SymId name) {
  if (name == support::kInvalidSym) {
    return std::nullopt;
  }
  const std::size_t limit = depthLimit(map);
  ScopeId current = from;
  for (std::size_t depth = 0; current.valid() && depth < limit; ++depth) {
    if (current.index >= map.scopes.size()) {
      return std::nullopt;
    }
    const Scope& scope = map.scopes[current.index];
    if (findInScope(scope, ns, name) != nullptr) {
      // The innermost hit was here; the shadowing question is about the *next*
      // one out, which is the rest of the same walk.
      return lookup(map, scope.parent, ns, name);
    }
    current = scope.parent;
  }
  return std::nullopt;
}

std::vector<support::SymId> visibleNames(const DefMap& map, ScopeId from, Namespace ns,
                                         std::size_t limit) {
  std::vector<support::SymId> out;
  if (limit == 0) {
    return out;
  }
  const std::size_t chain = depthLimit(map);
  ScopeId current = from;
  for (std::size_t depth = 0; current.valid() && depth < chain; ++depth) {
    if (current.index >= map.scopes.size()) {
      break;
    }
    const Scope& scope = map.scopes[current.index];
    // Declaration order inside a scope, innermost scope first: a deterministic
    // sequence that depends only on the input.
    for (const DefId id : scope.table(ns)) {
      if (!id.valid() || id.index >= map.defs.size()) {
        continue;
      }
      const support::SymId name = map.defs[id.index].name;
      if (name == support::kInvalidSym) {
        continue;
      }
      bool seen = false;
      for (const support::SymId existing : out) {
        if (existing == name) {
          seen = true;
          break;
        }
      }
      if (seen) {
        continue; // an inner declaration already claimed this name
      }
      out.push_back(name);
      if (out.size() >= limit) {
        return out;
      }
    }
    current = scope.parent;
  }
  return out;
}

} // namespace minc::resolve
