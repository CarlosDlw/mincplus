// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// "Did you mean ...?", bounded so it can run on every keystroke.
//
// An unknown name is the most common diagnostic in a language without
// `auto`-everything, and every reference implementation goes to some length to
// suggest one -- rustc will load crates it has not loaded yet to find an import
// worth suggesting. What matters here is the *bound*, because this runs in an
// editor while the user is typing:
//
//   * candidates are the names **visible** in the failing scope chain, in the
//     requested namespace -- never the whole unit, and never a name the user
//     could not have written;
//   * the length difference is checked before any distance is computed;
//   * the distance is `support::boundedEditDistance`, shared with the type
//     checker's "did you mean `i32`?", so the two cannot rank a typo
//     differently;
//   * the candidate set is capped;
//   * the answer is the first candidate in declaration order, and declaration
//     order is a function of the input.
#include "resolve/resolve.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "support/intern/interner.h"
#include "support/intern/sym_id.h"
#include "support/text/edit_distance.h"

namespace minc::resolve {
namespace {

using support::SymId;

} // namespace

support::SymId suggestName(const DefMap& map, ScopeId from, Namespace ns, support::SymId name,
                           const support::Interner& symbols, std::size_t maxCandidates,
                           std::uint32_t maxDistance) {
  if (name == support::kInvalidSym) {
    return support::kInvalidSym;
  }
  const std::string_view target = symbols.lookup(name);
  if (target.empty()) {
    return support::kInvalidSym;
  }

  const std::vector<SymId> candidates = visibleNames(map, from, ns, maxCandidates);
  SymId best = support::kInvalidSym;
  std::uint32_t bestDistance = maxDistance + 1;
  for (const SymId candidate : candidates) {
    if (candidate == name) {
      continue;
    }
    const std::string_view text = symbols.lookup(candidate);
    if (text.empty()) {
      continue;
    }
    const std::size_t diff =
        text.size() > target.size() ? text.size() - target.size() : target.size() - text.size();
    if (diff > maxDistance) {
      continue; // no distance is computed for a pair that cannot win
    }
    const std::uint32_t distance = support::boundedEditDistance(target, text, maxDistance);
    // Strictly closer wins: candidates arrive in declaration order, so a tie
    // keeps the earlier declaration and the answer stays a function of the
    // input rather than of the scoring.
    if (distance < bestDistance) {
      bestDistance = distance;
      best = candidate;
    }
  }
  return best;
}

} // namespace minc::resolve
