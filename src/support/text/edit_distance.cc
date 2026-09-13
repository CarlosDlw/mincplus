// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "support/text/edit_distance.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace minc::support {

std::uint32_t boundedEditDistance(std::string_view a, std::string_view b,
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

} // namespace minc::support
