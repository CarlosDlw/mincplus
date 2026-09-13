// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The unit's scopes, definitions, and one reference per name use.
//
// References live *beside* the AST and not in it. That is rust-analyzer's rule
// -- a syntax tree is a value and must not store semantic information -- applied
// here, and it has three concrete payoffs: the AST stays hashable, so the item
// tree can be compared cheaply; a formatter or a refactor can rewrite the AST
// without dragging resolution along; and a revision's resolution can be dropped
// without touching the tree.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "resolve/def.h"
#include "resolve/scope.h"
#include "support/intern/sym_id.h"
#include "support/limits.h"
#include "support/span/span.h"

namespace minc::resolve {

// Why a name use has no target. `None` means it resolved.
//
// The set is the *contract* of a `NameRef`, not a status enum that grows with
// the code: `WrongNamespace` (the name exists as a tag or a label) and
// `LimitReached` are named here because the fields above already carry the shape
// for them, and a consumer switching on the reason needs the whole set to
// compile once rather than changing shape when tags land.
enum class UnresolvedReason : std::uint8_t {
  None,
  // Nothing in the namespace is visible under that name.
  NotFound,
  // Reserved: the name exists in another namespace (a tag, a label).
  WrongNamespace,
  // Reserved: the use lies in a region the parser did not understand.
  InErrorRegion,
  // Reserved: a budget was reached before this use could be answered.
  LimitReached,
};

[[nodiscard]] std::string_view toString(UnresolvedReason value);

struct NameRef {
  // Where the name was *written*: the header, the macro argument site, the
  // spelling of a pasted token. This is what a diagnostic points at.
  support::Span span;
  // The same name's range in the translation unit's text. What maps the use back
  // to the node that produced it.
  support::Span unitSpan;
  support::SymId name = support::kInvalidSym;
  // `kInvalidDef` when the name did not resolve; `reason` then says why.
  DefId target;
  UnresolvedReason reason = UnresolvedReason::None;
  // The one other name worth mentioning, already ranked: a candidate for
  // "did you mean ...?". Empty when there is none.
  support::SymId suggestion = support::kInvalidSym;

  [[nodiscard]] bool resolved() const {
    return reason == UnresolvedReason::None;
  }
};

struct DefMap {
  std::vector<Scope> scopes;
  std::vector<Def> defs;
  std::vector<NameRef> refs;
  // The def each file-scope item produced, parallel to the unit's `ItemTree`.
  std::vector<DefId> itemDefs;
  // Every function's scope, parallel to `itemDefs` (and `kInvalidScopeId` for an
  // item with no body), so a consumer can ask about a function without walking
  // the scope tree to find it.
  std::vector<ScopeId> itemScopes;
  ScopeId fileScope;
  // The bound the lookup walk obeys. Stored on the map rather than fixed at the
  // use site, so a resolver run with a lowered `maxScopeDepth` (a test proving
  // the bound is a bound) and every lookup over the same map agree on it.
  std::size_t maxScopeDepth = support::kMaxScopeDepth;

  [[nodiscard]] const Scope& scope(ScopeId id) const {
    return scopes[id.index];
  }
  [[nodiscard]] const Def& def(DefId id) const {
    return defs[id.index];
  }
  [[nodiscard]] Def& def(DefId id) {
    return defs[id.index];
  }
};

} // namespace minc::resolve
