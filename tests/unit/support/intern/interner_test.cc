// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include "support/intern/interner.h"

namespace minc::support {
namespace {

TEST(InternerTest, SameTextSameId) {
  Interner in;
  SymId a = in.intern("main");
  SymId b = in.intern("main");
  EXPECT_EQ(a, b);
  EXPECT_EQ(in.size(), 1u);
  EXPECT_TRUE(in.contains("main"));
  EXPECT_FALSE(in.contains("other"));
}

TEST(InternerTest, LookupRoundTrip) {
  Interner in;
  SymId id = in.intern("puts");
  EXPECT_EQ(in.lookup(id), "puts");
  EXPECT_TRUE(in.lookup(999).empty());
}

TEST(InternerTest, ViewsStableAcrossGrowth) {
  Interner in;
  SymId first = in.intern("stable");
  std::string_view before = in.lookup(first);
  for (int i = 0; i < 500; ++i) {
    (void)in.intern("name" + std::to_string(i));
  }
  EXPECT_EQ(in.lookup(first), "stable");
  EXPECT_EQ(before, "stable");
}

TEST(InternerTest, InterningAViewFromThePoolIsSafe) {
  Interner in;
  const SymId first = in.intern("alpha");
  // The argument aliases the pool entry; indexing must key off a copy.
  const SymId again = in.intern(in.lookup(first));
  EXPECT_EQ(first, again);
  EXPECT_EQ(in.size(), 1u);
}

TEST(InternerTest, ClearResets) {
  Interner in;
  (void)in.intern("x");
  in.clear();
  EXPECT_TRUE(in.empty());
  EXPECT_EQ(in.size(), 0u);
  EXPECT_FALSE(in.contains("x"));
}

} // namespace
} // namespace minc::support
