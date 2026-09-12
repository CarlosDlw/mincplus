// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The one tag space has to stay consistent with the lexer's token kinds: if the
// two ever disagree, a leaf would be printed with the wrong name or, worse, a
// token kind would collide with a node kind.
#include <cstdint>
#include <string_view>

#include <gtest/gtest.h>

#include "lex/token_kind.h"
#include "parse/syntax_kind.h"

namespace minc::parse {
namespace {

TEST(SyntaxKindTest, EveryTokenKindIsBelowTheFirstNodeKind) {
  // The static_assert in the header pins the maximum; this walks the whole
  // range, so adding a token kind that is not the largest still fails here.
  for (std::uint16_t raw = 0; raw < static_cast<std::uint16_t>(lex::TokenKind::Last); ++raw) {
    const auto kind = static_cast<SyntaxKind>(raw);
    EXPECT_TRUE(isTokenKind(kind)) << "token kind " << raw << " is not below kFirstNodeKind";
    EXPECT_EQ(toString(kind), lex::toString(static_cast<lex::TokenKind>(raw)))
        << "token kind " << raw << " is named differently by the two modules";
  }
}

TEST(SyntaxKindTest, TokenAndNodeKindsDoNotOverlap) {
  for (const SyntaxKind kind : allNodeKinds()) {
    EXPECT_FALSE(isTokenKind(kind)) << toString(kind) << " collides with the token range";
  }
}

TEST(SyntaxKindTest, EveryNodeKindHasAName) {
  ASSERT_FALSE(allNodeKinds().empty());
  for (const SyntaxKind kind : allNodeKinds()) {
    const std::string_view name = toString(kind);
    EXPECT_NE(name, "Unknown") << "node kind " << static_cast<std::uint16_t>(kind)
                               << " has no row in the kind table";
    EXPECT_FALSE(name.empty());
  }
}

TEST(SyntaxKindTest, NodeKindsAreDistinct) {
  const std::span<const SyntaxKind> kinds = allNodeKinds();
  for (std::size_t i = 0; i < kinds.size(); ++i) {
    for (std::size_t j = i + 1; j < kinds.size(); ++j) {
      EXPECT_NE(kinds[i], kinds[j]) << "duplicate node kind " << toString(kinds[i]);
    }
  }
}

TEST(SyntaxKindTest, ConversionRoundTrips) {
  const SyntaxKind kind = toSyntaxKind(lex::TokenKind::KwFn);
  EXPECT_TRUE(isTokenKind(kind));
  EXPECT_EQ(toTokenKind(kind), lex::TokenKind::KwFn);
  EXPECT_EQ(toString(kind), "KwFn");
}

// The reserved kinds are the promise that macros, token trees, and attributes
// can be added without a grammar redesign, so assert they are really there.
TEST(SyntaxKindTest, ReservedKindsExist) {
  bool hasMacro = false;
  bool hasTokenTree = false;
  bool hasAttribute = false;
  for (const SyntaxKind kind : allNodeKinds()) {
    hasMacro = hasMacro || kind == SyntaxKind::MacroCall;
    hasTokenTree = hasTokenTree || kind == SyntaxKind::TokenTree;
    hasAttribute = hasAttribute || kind == SyntaxKind::Attribute;
  }
  EXPECT_TRUE(hasMacro);
  EXPECT_TRUE(hasTokenTree);
  EXPECT_TRUE(hasAttribute);
}

} // namespace
} // namespace minc::parse
