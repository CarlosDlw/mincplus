// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include <cstdint>
#include <string_view>

#include <gtest/gtest.h>

#include "support/line/line_table.h"

namespace minc::support {
namespace {

TEST(LineTableTest, EmptyTextHasOneLine) {
  LineTable t("");
  EXPECT_EQ(t.lineCount(), 1u);
  LineCol p = t.lookup(0, 0);
  EXPECT_EQ(p.line, 1u);
  EXPECT_EQ(p.col, 1u);
}

TEST(LineTableTest, SingleLineLookup) {
  LineTable t("hello");
  EXPECT_EQ(t.lineCount(), 1u);
  LineCol p = t.lookup(2, 5);
  EXPECT_EQ(p.line, 1u);
  EXPECT_EQ(p.col, 3u);
}

TEST(LineTableTest, MultilineLookup) {
  LineTable t("ab\ncd\nef");
  EXPECT_EQ(t.lineCount(), 3u);
  EXPECT_EQ(t.lookup(0, 8).line, 1u);
  EXPECT_EQ(t.lookup(3, 8).line, 2u);
  EXPECT_EQ(t.lookup(3, 8).col, 1u);
  EXPECT_EQ(t.lookup(4, 8).col, 2u);
  EXPECT_EQ(t.lookup(6, 8).line, 3u);
}

TEST(LineTableTest, NewlineOffsetBelongsToNextLine) {
  LineTable t("a\nb");
  LineCol atNewline = t.lookup(1, 3);
  EXPECT_EQ(atNewline.line, 1u);
  LineCol after = t.lookup(2, 3);
  EXPECT_EQ(after.line, 2u);
  EXPECT_EQ(after.col, 1u);
}

TEST(LineTableTest, ClampsPastEnd) {
  LineTable t("ab");
  LineCol p = t.lookup(100, 2);
  EXPECT_EQ(p.line, 1u);
  EXPECT_EQ(p.col, 3u);
}

TEST(LineTableTest, LineTextStripsNewline) {
  LineTable t("foo\nbar\n");
  auto l1 = t.lineText("foo\nbar\n", 1);
  auto l2 = t.lineText("foo\nbar\n", 2);
  ASSERT_TRUE(l1.has_value());
  ASSERT_TRUE(l2.has_value());
  EXPECT_EQ(*l1, "foo");
  EXPECT_EQ(*l2, "bar");
}

TEST(LineTableTest, LineTextOutOfRange) {
  LineTable t("x");
  EXPECT_FALSE(t.lineText("x", 0).has_value());
  EXPECT_FALSE(t.lineText("x", 5).has_value());
}

TEST(LineTableTest, LineRangeLastLineNoNewline) {
  LineTable t("ab\ncd");
  auto r = t.lineRange("ab\ncd", 2);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->begin, 3u);
  EXPECT_EQ(r->end, 5u);
}

TEST(LineTableTest, RebuildResets) {
  LineTable t("a\nb\nc");
  EXPECT_EQ(t.lineCount(), 3u);
  t.rebuild("solo");
  EXPECT_EQ(t.lineCount(), 1u);
  EXPECT_EQ(t.lookup(2, 4).line, 1u);
}

TEST(LineTableTest, TrailingNewlineCreatesEmptyLastLine) {
  LineTable t("a\n");
  EXPECT_EQ(t.lineCount(), 2u);
  auto last = t.lineText("a\n", 2);
  ASSERT_TRUE(last.has_value());
  EXPECT_TRUE(last->empty());
}

TEST(LineTableTest, CrlfIsOneTerminator) {
  const std::string_view text = "foo\r\nbar\r\n";
  LineTable t(text);
  EXPECT_EQ(t.lineCount(), 3u);
  auto first = t.lineText(text, 1);
  auto second = t.lineText(text, 2);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(*first, "foo"); // the CR must not leak into the line text
  EXPECT_EQ(*second, "bar");
  EXPECT_EQ(t.lookup(5, static_cast<std::uint32_t>(text.size())).line, 2u);
  EXPECT_EQ(t.lookup(5, static_cast<std::uint32_t>(text.size())).col, 1u);
}

TEST(LineTableTest, LoneCrIsATerminator) {
  const std::string_view text = "a\rb";
  LineTable t(text);
  EXPECT_EQ(t.lineCount(), 2u);
  EXPECT_EQ(*t.lineText(text, 1), "a");
  EXPECT_EQ(*t.lineText(text, 2), "b");
}

TEST(LineTableTest, CrlfLastLineRangeExcludesCr) {
  const std::string_view text = "ab\r\ncd";
  LineTable t(text);
  auto r = t.lineRange(text, 2);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->begin, 4u);
  EXPECT_EQ(r->end, 6u);
}

} // namespace
} // namespace minc::support
