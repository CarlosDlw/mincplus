// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

#include <gtest/gtest.h>

#include "driver/suggest.h"

namespace minc::driver {
namespace {

constexpr std::array<std::string_view, 4> kCandidates{"build", "run", "check", "resolve"};

TEST(SuggestTest, EditDistanceCountsTheFourEdits) {
  EXPECT_EQ(editDistance("build", "build"), 0u);
  EXPECT_EQ(editDistance("", "build"), 5u);
  EXPECT_EQ(editDistance("build", ""), 5u);
  // One insertion, one deletion, one substitution.
  EXPECT_EQ(editDistance("buld", "build"), 1u);
  EXPECT_EQ(editDistance("build", "buld"), 1u);
  EXPECT_EQ(editDistance("build", "buiid"), 1u);
}

TEST(SuggestTest, ATranspositionCostsOneEdit) {
  // The typo people actually make. Plain Levenshtein scores these as two, which
  // is far enough to fall outside the threshold and to lose the suggestion.
  EXPECT_EQ(editDistance("bulid", "build"), 1u);
  EXPECT_EQ(editDistance("--targte", "--target"), 1u);
  EXPECT_EQ(editDistance("recieve", "receive"), 1u);
}

TEST(SuggestTest, TheNearestCandidateWins) {
  EXPECT_EQ(nearestName(kCandidates, "buld"), std::optional<std::string_view>("build"));
  EXPECT_EQ(nearestName(kCandidates, "runn"), std::optional<std::string_view>("run"));
  EXPECT_EQ(nearestName(kCandidates, "chek"), std::optional<std::string_view>("check"));
}

TEST(SuggestTest, NothingIsSuggestedWhenNothingIsClose) {
  EXPECT_FALSE(nearestName(kCandidates, "frobnicate").has_value());
  EXPECT_FALSE(nearestName(kCandidates, "").has_value());
  EXPECT_FALSE(nearestName({}, "build").has_value());
}

TEST(SuggestTest, AShortWordGetsOneEditAndNoMore) {
  // `run` and `sun` differ by one edit and the suggestion is worth making; `fun`
  // is the same distance and just as arbitrary, which is the cost of suggesting
  // at all and the reason the note never acts on its own.
  EXPECT_TRUE(nearestName(kCandidates, "sun").has_value());
  EXPECT_FALSE(nearestName(kCandidates, "wxyz").has_value());
}

TEST(SuggestTest, ATieGoesToTheEarlierCandidate) {
  // Table order, so the answer is stable rather than whichever the scan saw.
  constexpr std::array<std::string_view, 2> tied{"abc", "abd"};
  EXPECT_EQ(nearestName(tied, "abe"), std::optional<std::string_view>("abc"));
}

} // namespace
} // namespace minc::driver
