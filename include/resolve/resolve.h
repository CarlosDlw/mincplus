// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Name resolution: the unit's scopes, its definitions, and one answer per name.
//
// Two phases, never one:
//
//   * **collect** walks the item trees -- never bodies -- and creates the file
//     scope and its definitions. It exists because file-scope names are
//     order-independent: a body may call a function written after it, so the set
//     of visible names has to be known before any body is looked at.
//   * **resolve** walks bodies. Parameters enter the function scope before the
//     body is walked; a block scope is opened as the walk enters a block; each
//     `let`/`const` adds its name as the walk passes it, so block scope is
//     point-of-declaration and no pre-scan is needed.
//
// Resolution is **total**: every name use produces a `NameRef`, and an
// unresolved one carries a reason. Nothing throws, nothing stops early, and an
// unresolved name is never an error for the expression around it -- one name,
// one diagnostic.
//
// Design record: `docs/architectures/resolve.md`.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "ast/ast.h"
#include "resolve/map.h"
#include "resolve/resolve_error.h"
#include "support/intern/interner.h"
#include "support/limits.h"

namespace minc::resolve {

// Everything the resolver enforces. The bounds follow the preprocessor's rule:
// they can be lowered (a constrained environment, a test that wants to prove the
// bound is a bound) and never disabled.
struct ResolveOptions {
  bool warnUnused = false;
  bool warnShadow = false;
  std::size_t maxDefs = support::kMaxDefsPerUnit;
  std::size_t maxScopes = support::kMaxScopesPerUnit;
  std::size_t maxRefs = support::kMaxNameRefsPerUnit;
  std::size_t maxScopeDepth = support::kMaxScopeDepth;
  std::size_t maxSuggestionCandidates = support::kMaxSuggestionCandidates;
  std::uint32_t maxSuggestionDistance = support::kMaxSuggestionDistance;
};

struct ResolveOutput {
  DefMap map;
  std::vector<ResolveError> errors;
  std::vector<ResolveError> warnings;
};

// Resolves one translation unit. `symbols` is the interner the lowered file's
// `SymId`s came from; every comparison this stage makes is on `SymId` integers,
// and the interner is only read -- except to bind the language's predefined
// names (`true`, `false`), which is why it is taken by mutable reference: adding
// a name to the symbol table is a decision this stage makes.
[[nodiscard]] ResolveOutput resolveUnit(const ast::LoweredFile& file, support::Interner& symbols,
                                        ResolveOptions options = {});

// --- lookup ------------------------------------------------------------------
//
// Public because the language server asks these questions directly, and because
// a test that pins "innermost-outward, first hit wins" should call the same code
// the resolver does rather than a copy of it.

// The declaration `name` denotes at `from`, or nullopt.
[[nodiscard]] std::optional<DefId> lookup(const DefMap& map, ScopeId from, Namespace ns,
                                          support::SymId name);

// The declaration `name` would denote if the innermost one were removed: the
// next hit walking outward. The `-Wshadow` question, and one step the walk above
// was going to take anyway.
[[nodiscard]] std::optional<DefId> lookupOuter(const DefMap& map, ScopeId from, Namespace ns,
                                               support::SymId name);

// The names visible from `from` in `ns`, innermost-wins, at most `limit` of
// them, in a deterministic order. Used by suggestions and by the command's
// "what could I have meant" output.
[[nodiscard]] std::vector<support::SymId> visibleNames(const DefMap& map, ScopeId from,
                                                       Namespace ns, std::size_t limit);

// The best candidate for a misspelling of `name`, or `kInvalidSym`. Bounded by
// construction: candidates are visible names only, filtered by length before any
// distance is computed, scored with an early exit at `maxDistance`, capped at
// `maxCandidates`, and broken by declaration order then `SymId` -- so the answer
// is a function of the input and not of a container's insertion order.
[[nodiscard]] support::SymId suggestName(const DefMap& map, ScopeId from, Namespace ns,
                                         support::SymId name, const support::Interner& symbols,
                                         std::size_t maxCandidates, std::uint32_t maxDistance);

} // namespace minc::resolve
