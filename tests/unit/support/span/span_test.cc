// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include "support/limits.h"
#include "support/span/span.h"
#include "support/span/span_ops.h"

namespace minc::support {
namespace {

TEST(SpanTest, DefaultIsInvalid) {
  Span s;
  EXPECT_FALSE(s.valid());
  EXPECT_EQ(s.file, kInvalidFile);
}

TEST(SpanTest, AtCreatesSingleByte) {
  Span s = Span::at(2, 7);
  EXPECT_TRUE(s.valid());
  EXPECT_EQ(s.file, 2u);
  EXPECT_EQ(s.begin, 7u);
  EXPECT_EQ(s.end, 8u);
  EXPECT_EQ(s.size(), 1u);
}

TEST(SpanTest, AtSaturatesInsteadOfWrapping) {
  // offset + 1 would wrap to 0 and silently point at the file start.
  Span s = Span::at(0, kMaxOffset);
  EXPECT_TRUE(s.valid());
  EXPECT_TRUE(s.empty());
  EXPECT_EQ(s.end, kMaxOffset);
}

TEST(SpanTest, SizeNeverUnderflows) {
  Span reversed(0, 9, 4);
  EXPECT_FALSE(reversed.valid());
  EXPECT_EQ(reversed.size(), 0u);
}

TEST(SpanTest, ContainsHalfOpen) {
  Span s(0, 4, 8);
  EXPECT_TRUE(s.contains(4));
  EXPECT_TRUE(s.contains(7));
  EXPECT_FALSE(s.contains(8));
  EXPECT_TRUE(s.containsClosed(8));
  EXPECT_FALSE(s.contains(3));
}

TEST(SpanTest, EmptySpan) {
  Span s(1, 5, 5);
  EXPECT_TRUE(s.valid());
  EXPECT_TRUE(s.empty());
  EXPECT_EQ(s.size(), 0u);
  EXPECT_FALSE(s.contains(5));
  EXPECT_TRUE(s.containsClosed(5));
}

TEST(SpanTest, MergeSameFile) {
  Span a(0, 2, 5);
  Span b(0, 4, 9);
  Span m = merge(a, b);
  EXPECT_TRUE(m.valid());
  EXPECT_EQ(m.begin, 2u);
  EXPECT_EQ(m.end, 9u);
}

TEST(SpanTest, MergeDifferentFilesIsInvalid) {
  Span m = merge(Span(0, 0, 3), Span(1, 0, 3));
  EXPECT_FALSE(m.valid());
}

TEST(SpanTest, MergeInvalidIsInvalid) {
  EXPECT_FALSE(merge(Span{}, Span(0, 0, 1)).valid());
  EXPECT_FALSE(merge(Span(0, 0, 1), Span{}).valid());
}

TEST(SpanTest, ExtendToCoverGrows) {
  Span s(0, 5, 6);
  extendToCover(s, Span(0, 1, 3));
  EXPECT_EQ(s.begin, 1u);
  EXPECT_EQ(s.end, 6u);
}

TEST(SpanTest, ExtendToCoverIgnoresForeignFile) {
  Span s(0, 5, 6);
  extendToCover(s, Span(1, 0, 100));
  EXPECT_EQ(s.begin, 5u);
  EXPECT_EQ(s.end, 6u);
}

TEST(SpanTest, ExtendInvalidTakesOther) {
  Span s;
  extendToCover(s, Span(3, 9, 10));
  EXPECT_TRUE(s.valid());
  EXPECT_EQ(s.file, 3u);
}

TEST(SpanTest, ToStringFormats) {
  EXPECT_EQ(toString(Span(2, 10, 14)), "f2:10-14");
  EXPECT_EQ(toString(Span{}), "<invalid>");
}

} // namespace
} // namespace minc::support
