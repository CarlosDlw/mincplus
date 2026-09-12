// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include <string>

#include <gtest/gtest.h>

#include "support/utf8/bom.h"
#include "support/utf8/decode.h"
#include "support/utf8/validate.h"

namespace minc::support::utf8 {
namespace {

TEST(Utf8Test, AsciiValid) {
  EXPECT_TRUE(isValid("fn int main() {}"));
  EXPECT_FALSE(firstInvalidOffset("ok").has_value());
}

TEST(Utf8Test, MultibyteValid) {
  EXPECT_TRUE(isValid("caf\xC3\xA9"));
  EXPECT_EQ(countCodePoints("caf\xC3\xA9"), 4u);
  EXPECT_EQ(countCodePoints(""), 0u);
}

TEST(Utf8Test, RejectsTruncated) {
  auto off = firstInvalidOffset("ab\xC3");
  ASSERT_TRUE(off.has_value());
  EXPECT_EQ(*off, 2u);
  EXPECT_FALSE(isValid("ab\xC3"));
}

TEST(Utf8Test, RejectsOverlong) {
  EXPECT_FALSE(isValid("\xC0\xAF"));
  EXPECT_EQ(firstInvalidOffset("\xC0\xAF"), 0u);
  // The decoder agrees with the validator; there is only one state machine.
  EXPECT_FALSE(decodeOne("\xC0\xAF", 0).has_value());
}

TEST(Utf8Test, RejectsSurrogate) {
  EXPECT_FALSE(isValid("\xED\xA0\x80"));
  EXPECT_FALSE(decodeOne("\xED\xA0\x80", 0).has_value());
}

TEST(Utf8Test, RejectsAboveUnicodeRange) {
  EXPECT_FALSE(isValid("\xF5\x80\x80\x80"));
  EXPECT_FALSE(decodeOne("\xF5\x80\x80\x80", 0).has_value());
}

TEST(Utf8Test, RejectsStrayContinuation) {
  EXPECT_FALSE(isValid("\x80"));
  EXPECT_EQ(firstInvalidOffset("\x80"), 0u);
}

TEST(Utf8Test, DecodeOne) {
  auto a = decodeOne("A", 0);
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->codePoint, U'A');
  EXPECT_EQ(a->length, 1u);

  auto e = decodeOne("caf\xC3\xA9", 3);
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->codePoint, U'\u00E9');
  EXPECT_EQ(e->length, 2u);

  auto emoji = decodeOne("\xF0\x9F\x98\x80", 0); // U+1F600
  ASSERT_TRUE(emoji.has_value());
  EXPECT_EQ(emoji->codePoint, U'\U0001F600');
  EXPECT_EQ(emoji->length, 4u);

  EXPECT_FALSE(decodeOne("", 0).has_value());
  EXPECT_FALSE(decodeOne("\xFF", 0).has_value());
  EXPECT_FALSE(decodeOne("\xE2\x82", 0).has_value()); // truncated 3-byte form
  EXPECT_FALSE(decodeOne("abc", 3).has_value());      // past the end
}

TEST(Utf8Test, CountCodePointsIsTotalOnMalformedInput) {
  // Each malformed byte is counted once, so callers always get a number.
  EXPECT_EQ(countCodePoints("\xFF\xFE"), 2u);
}

TEST(Utf8BomTest, DetectsEachMark) {
  // Explicit lengths: constructing from a literal would stop at the NUL bytes
  // that make up the UTF-32 marks.
  const std::string utf8("\xEF\xBB\xBF", 3);
  const std::string utf16le("\xFF\xFE", 2);
  const std::string utf16be("\xFE\xFF", 2);
  const std::string utf32le("\xFF\xFE\x00\x00", 4);
  const std::string utf32be("\x00\x00\xFE\xFF", 4);

  EXPECT_EQ(detectByteOrderMark(utf8), ByteOrderMark::Utf8);
  EXPECT_EQ(detectByteOrderMark(utf16le), ByteOrderMark::Utf16LittleEndian);
  EXPECT_EQ(detectByteOrderMark(utf16be), ByteOrderMark::Utf16BigEndian);
  // UTF-32LE starts with the UTF-16LE bytes; the longer mark must win.
  EXPECT_EQ(detectByteOrderMark(utf32le), ByteOrderMark::Utf32LittleEndian);
  EXPECT_EQ(detectByteOrderMark(utf32be), ByteOrderMark::Utf32BigEndian);
  EXPECT_EQ(detectByteOrderMark("fn int main()"), ByteOrderMark::None);
}

TEST(Utf8BomTest, LengthsAndClassification) {
  EXPECT_EQ(byteOrderMarkLength(ByteOrderMark::None), 0u);
  EXPECT_EQ(byteOrderMarkLength(ByteOrderMark::Utf8), 3u);
  EXPECT_EQ(byteOrderMarkLength(ByteOrderMark::Utf16LittleEndian), 2u);
  EXPECT_EQ(byteOrderMarkLength(ByteOrderMark::Utf32LittleEndian), 4u);

  EXPECT_FALSE(isUnsupportedEncoding(ByteOrderMark::None));
  EXPECT_FALSE(isUnsupportedEncoding(ByteOrderMark::Utf8));
  EXPECT_TRUE(isUnsupportedEncoding(ByteOrderMark::Utf16LittleEndian));
  EXPECT_TRUE(isUnsupportedEncoding(ByteOrderMark::Utf32BigEndian));
}

TEST(Utf8BomTest, ShortInputIsNotAMark) {
  EXPECT_EQ(detectByteOrderMark("\xFF"), ByteOrderMark::None);
  EXPECT_EQ(detectByteOrderMark(""), ByteOrderMark::None);
  EXPECT_STREQ(toString(ByteOrderMark::Utf16LittleEndian), "UTF-16 little-endian");
}

} // namespace
} // namespace minc::support::utf8
