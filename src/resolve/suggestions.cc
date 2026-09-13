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
//   * the distance is Damerau-Levenshtein with an early exit at the maximum, so
//     no pair costs more than a banded walk;
//   * the candidate set is capped;
//   * the answer is the first candidate in declaration order, and declaration
//     order is a function of the input.
#include "resolve/resolve.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "support/intern/interner.h"
#include "support/intern/sym_id.h"

namespace minc::resolve {
namespace {

using support::SymId;

// Damerau-Levenshtein (optimal string alignment) with a cutoff. Returns a value
// greater than `maxDistance` as soon as a whole row exceeds it, which is what
// keeps two long names from costing a full quadratic table.
[[nodiscard]] std::uint32_t editDistance(std::string_view a, std::string_view b,
                                         std::uint32_t maxDistance) {
  const std::size_t n = a.size();
  const std::size_t m = b.size();
  const std::uint32_t ceiling = maxDistance + 1;

  // Lengths that already differ by more than the cutoff cannot come back under
  // it; the caller usually checks this first, but the function is safe alone.
  const std::size_t diff = n > m ? n - m : m - n;
  if (diff > maxDistance) {
    return ceiling;
  }
  if (n == 0) {
    return static_cast<std::uint32_t>(m);
  }
  if (m == 0) {
    return static_cast<std::uint32_t>(n);
  }

  std::vector<std::uint32_t> prev2(m + 1, 0);
  std::vector<std::uint32_t> prev(m + 1, 0);
  std::vector<std::uint32_t> cur(m + 1, 0);
  for (std::size_t j = 0; j <= m; ++j) {
    prev[j] = static_cast<std::uint32_t>(j);
  }

  for (std::size_t i = 1; i <= n; ++i) {
    cur[0] = static_cast<std::uint32_t>(i);
    std::uint32_t rowMin = cur[0];
    for (std::size_t j = 1; j <= m; ++j) {
      const std::uint32_t substitution = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0U : 1U);
      const std::uint32_t insertion = prev[j] + 1U;
      const std::uint32_t deletion = cur[j - 1] + 1U;
      std::uint32_t best = std::min({substitution, insertion, deletion});
      if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) {
        best = std::min(best, prev2[j - 2] + 1U);
      }
      cur[j] = best;
      rowMin = std::min(rowMin, best);
    }
    if (rowMin > maxDistance) {
      return ceiling;
    }
    std::swap(prev2, prev);
    std::swap(prev, cur);
  }
  return prev[m];
}

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
    const std::uint32_t distance = editDistance(target, text, maxDistance);
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
