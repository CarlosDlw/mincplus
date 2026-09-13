// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The one "did you mean ...?" distance, shared.
//
// Two stages ask it -- name resolution (`resolve/suggestions.cc`) and the type
// checker, which spells the type names it knows -- and they must not answer
// differently. It is the *algorithm* that lives here and nothing else: no
// candidate list, no ranking, no knowledge of names or types. A caller decides
// what to compare; this decides how close two spellings are, with a cutoff,
// because an editor asks on every keystroke.
#pragma once

#include <cstdint>
#include <string_view>

namespace minc::support {

// Damerau-Levenshtein (optimal string alignment: insertion, deletion,
// substitution, transposition) with a cutoff.
//
// Returns a value **greater than** `maxDistance` as soon as the true distance is
// known to exceed it, so a pair that cannot match never builds a full table.
// Callers that can rule a candidate out by length alone should still do so first
// -- this is safe on its own, but the caller usually knows more.
[[nodiscard]] std::uint32_t boundedEditDistance(std::string_view a, std::string_view b,
                                                std::uint32_t maxDistance);

} // namespace minc::support
