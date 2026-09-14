// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// "Did you mean ...?", answered from the spec table and never acted on.
//
// A suggestion is a *note* attached to a usage error, and the exit code stays 2.
// The guide the industry writes this down in (clig.dev) is explicit about why:
// invalid input is not necessarily a typo -- it is often a logical mistake or a
// shell variable that expanded to nothing -- and a program that silently runs
// what it guessed has changed what the user typed into something they did not
// write, and then has to keep supporting it. Worse, they never learn the real
// spelling. So this module produces text and nothing else.
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace minc::driver {

// The number of single-character edits that turn one spelling into the other,
// with an adjacent transposition counted as one -- which is the typo people
// actually make (`bulid`, `targte`) and the one plain Levenshtein scores as two,
// far enough to fall outside the threshold below.
[[nodiscard]] std::size_t editDistance(std::string_view left, std::string_view right);

// The candidate closest to `input`, or nothing when none is close enough.
//
// "Close enough" is `1 + input.size() / 3` edits: a short word gets one edit,
// which is all a four-letter word can afford, and a long one gets a few, because
// a long misspelling is usually a small edit in a long name. Candidates are
// compared as written, so the caller decides what can be suggested -- the
// command table's names, or the options that apply to the command being typed.
[[nodiscard]] std::optional<std::string_view>
nearestName(std::span<const std::string_view> candidates, std::string_view input);

} // namespace minc::driver
