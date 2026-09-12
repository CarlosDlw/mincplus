// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
//
// The header-name scanner. Every case here is one the token stream cannot
// express, which is the reason this entry point exists next to `lexOne`.
#include <optional>
#include <string_view>

#include <gtest/gtest.h>

#include "lex/header_name.h"

namespace minc::lex {
namespace {

TEST(HeaderNameTest, AngleForm) {
  const std::optional<HeaderName> name = scanHeaderName("#include <a/b.h>\n", 9);
  ASSERT_TRUE(name.has_value());
  EXPECT_EQ(name->offset, 9u);
  EXPECT_EQ(name->length, 7u); // "<a/b.h>", delimiters included
  EXPECT_TRUE(name->angle);
  EXPECT_EQ(name->text, "a/b.h");
}

TEST(HeaderNameTest, QuotedForm) {
  const std::optional<HeaderName> name = scanHeaderName("#include \"a.h\"\n", 9);
  ASSERT_TRUE(name.has_value());
  EXPECT_EQ(name->length, 5u);
  EXPECT_FALSE(name->angle);
  EXPECT_EQ(name->text, "a.h");
}

TEST(HeaderNameTest, SlashesAreNotAComment) {
  // `h-char` and `q-char` exclude only the newline and the closing delimiter, so
  // `//` and `/*` are ordinary characters. This is the case the plain lexer gets
  // wrong in a way no amount of joining tokens can repair: it reads `//b.h>` as a
  // comment and swallows the `>`, and the include is then reported unterminated.
  const std::optional<HeaderName> line = scanHeaderName("#include <a//b.h>\n", 9);
  ASSERT_TRUE(line.has_value());
  EXPECT_EQ(line->text, "a//b.h");

  const std::optional<HeaderName> block = scanHeaderName("#include <a/*b.h>\n", 9);
  ASSERT_TRUE(block.has_value());
  EXPECT_EQ(block->text, "a/*b.h");
}

TEST(HeaderNameTest, EscapesAreNotProcessed) {
  // A q-char-sequence has no escapes: `\d` and `\x` are two bytes each and the
  // name is exactly what was written. Lexing this as a string literal is what
  // used to draw "unknown escape" on valid code.
  const std::optional<HeaderName> name = scanHeaderName("#include \"c:\\dir\\x.h\"\n", 9);
  ASSERT_TRUE(name.has_value());
  EXPECT_EQ(name->text, "c:\\dir\\x.h");
}

TEST(HeaderNameTest, DelimitersInsideAreLegal) {
  // `>` is legal inside `"..."` and `"` is legal inside `<...>`, so the two
  // forms are not interchangeable and neither can be found by scanning for the
  // other's delimiter.
  const std::optional<HeaderName> angle = scanHeaderName("<a\"b.h>", 0);
  ASSERT_TRUE(angle.has_value());
  EXPECT_EQ(angle->text, "a\"b.h");

  const std::optional<HeaderName> quoted = scanHeaderName("\"a>b.h\"", 0);
  ASSERT_TRUE(quoted.has_value());
  EXPECT_EQ(quoted->text, "a>b.h");
}

TEST(HeaderNameTest, SpacesBelongToTheName) {
  // A space is an h-char, so `<a b.h>` is one name with a space in it -- not an
  // error about extra tokens.
  const std::optional<HeaderName> name = scanHeaderName("<a b.h>", 0);
  ASSERT_TRUE(name.has_value());
  EXPECT_EQ(name->text, "a b.h");
}

TEST(HeaderNameTest, UnclosedIsNullopt) {
  // The newline ends the attempt: `.mx` has no line splicing, so a header-name
  // cannot span one, and the caller is the one that reports it.
  EXPECT_FALSE(scanHeaderName("#include <a.h\n", 9).has_value());
  EXPECT_FALSE(scanHeaderName("#include \"a.h\n", 9).has_value());
  EXPECT_FALSE(scanHeaderName("#include <a.h", 9).has_value());
  EXPECT_FALSE(scanHeaderName("#include \"a.h", 9).has_value());
}

TEST(HeaderNameTest, NotAHeaderName) {
  // Anything that is not an opening delimiter, and any offset past the end.
  EXPECT_FALSE(scanHeaderName("a.h", 0).has_value());
  EXPECT_FALSE(scanHeaderName("", 0).has_value());
  EXPECT_FALSE(scanHeaderName("<a.h>", 5).has_value());
  EXPECT_FALSE(scanHeaderName("'a.h'", 0).has_value());
}

TEST(HeaderNameTest, EmptyNameIsStillAName) {
  // `<` `>` with nothing between is an empty h-char-sequence. The scanner found a
  // header-name; that the name is unusable is the caller's diagnostic, which is
  // where the message can name the directive it appeared in.
  const std::optional<HeaderName> name = scanHeaderName("<>", 0);
  ASSERT_TRUE(name.has_value());
  EXPECT_TRUE(name->text.empty());
  EXPECT_EQ(name->length, 2u);
  EXPECT_TRUE(name->angle);
}

TEST(HeaderNameTest, CarriageReturnEndsTheLine) {
  // A lone CR is a line terminator everywhere else in the project, so it is one
  // here too rather than a byte the name silently absorbs.
  EXPECT_FALSE(scanHeaderName("<a.h\r\n", 0).has_value());
  EXPECT_FALSE(scanHeaderName("<a.h\r", 0).has_value());
}

TEST(HeaderNameTest, ScanIsRelativeToTheOffset) {
  // The offset is where the caller found the delimiter, not a position this
  // scanner derives: it is a pure function of its two arguments.
  const std::optional<HeaderName> name = scanHeaderName("xx <a.h> yy", 3);
  ASSERT_TRUE(name.has_value());
  EXPECT_EQ(name->offset, 3u);
  EXPECT_EQ(name->text, "a.h");
}

} // namespace
} // namespace minc::lex
