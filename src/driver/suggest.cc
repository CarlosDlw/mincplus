// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "driver/suggest.h"

#include <algorithm>
#include <vector>

namespace minc::driver {

std::size_t editDistance(std::string_view left, std::string_view right) {
  // Optimal string alignment: Levenshtein plus the transposition of two adjacent
  // characters as a single edit. The full Damerau-Levenshtein distance needs a
  // matrix of the whole prefix history; OSA needs three rows and differs from it
  // only on strings with several overlapping transpositions, which a typed
  // command line does not have.
  const std::size_t rows = left.size() + 1;
  const std::size_t columns = right.size() + 1;
  if (left.empty()) {
    return right.size();
  }
  if (right.empty()) {
    return left.size();
  }

  // Three rows of the recurrence, reused: two back, one back, and the row being
  // filled.
  std::vector<std::size_t> beforePrevious(columns, 0);
  std::vector<std::size_t> previous(columns, 0);
  std::vector<std::size_t> current(columns, 0);
  for (std::size_t column = 0; column < columns; ++column) {
    previous[column] = column;
  }

  for (std::size_t row = 1; row < rows; ++row) {
    current[0] = row;
    for (std::size_t column = 1; column < columns; ++column) {
      const std::size_t substitution =
          previous[column - 1] + (left[row - 1] == right[column - 1] ? 0U : 1U);
      const std::size_t deletion = previous[column] + 1U;
      const std::size_t insertion = current[column - 1] + 1U;
      std::size_t best = std::min({substitution, deletion, insertion});
      if (row > 1 && column > 1 && left[row - 1] == right[column - 2] &&
          left[row - 2] == right[column - 1]) {
        best = std::min(best, beforePrevious[column - 2] + 1U);
      }
      current[column] = best;
    }
    std::swap(beforePrevious, previous);
    std::swap(previous, current);
  }
  return previous[columns - 1];
}

std::optional<std::string_view> nearestName(std::span<const std::string_view> candidates,
                                            std::string_view input) {
  if (input.empty()) {
    return std::nullopt;
  }
  const std::size_t limit = 1U + input.size() / 3U;

  std::optional<std::string_view> best;
  std::size_t bestDistance = limit + 1U;
  for (const std::string_view candidate : candidates) {
    // A tie goes to the earlier candidate, which is the table's order and so a
    // stable, explainable choice rather than whichever the scan happened to see.
    const std::size_t distance = editDistance(candidate, input);
    if (distance <= limit && distance < bestDistance) {
      best = candidate;
      bestDistance = distance;
    }
  }
  return best;
}

} // namespace minc::driver
