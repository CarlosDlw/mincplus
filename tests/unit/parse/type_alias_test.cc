// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// `type Name = T;`: one production, two positions.
//
// The declaration is the *same* node at the file scope and among the statements
// (`type_alias.md`, decision 5) -- where its name is visible is a scope rule and
// not a second grammar -- so this file checks the shape once and the position
// twice, and then checks that each half of the production can be left out with one
// message and a tree that still reconstructs.
#include <cstddef>
#include <string>

#include <gtest/gtest.h>

#include "parse/parse_error.h"
#include "parse/parse_fixture.h"
#include "parse/syntax_kind.h"
#include "syntax/tree.h"

namespace minc::parse {
namespace {

using test::ParseFixture;

// The single `type` declaration of a unit whose only other item is `main`.
[[nodiscard]] syntax::SyntaxNode aliasDecl(const ParseFixture& fixture) {
  return fixture.tree().root().childOfKind(SyntaxKind::TypeAliasDecl).value();
}

// The spellings of a node's own significant token children, joined by a space. A
// type position is *tokens* -- the grammar has one `Type` node and the constructors
// are the `*` and `[N]` inside it (`parser.md`) -- so this is how a test asks what a
// type run was written as without reaching for the source bytes.
[[nodiscard]] std::string tokenText(const syntax::SyntaxNode& node) {
  std::string out;
  for (std::size_t i = 0; i < node.childCount(); ++i) {
    const syntax::NodeOrToken child = node.child(i);
    if (!child.isToken() || child.asToken().isTrivia()) {
      continue;
    }
    if (!out.empty()) {
      out += ' ';
    }
    out += std::string(child.asToken().text());
  }
  return out;
}

void expectLossless(const ParseFixture& fixture) {
  EXPECT_EQ(fixture.errorCount(), 0u) << fixture.errorMessages();
  EXPECT_TRUE(fixture.tree().validate());
  EXPECT_EQ(fixture.reconstruct(), fixture.source());
}

TEST(TypeAliasParseTest, TheDeclarationHasANameAndAType) {
  ParseFixture fixture("type Word = u32;\n");
  ASSERT_TRUE(fixture.built());
  expectLossless(fixture);

  const syntax::SyntaxNode decl = aliasDecl(fixture);
  EXPECT_EQ(decl.kind(), SyntaxKind::TypeAliasDecl);
  // The same two children every binding has, in the same order: a `Name` node and
  // a `Type` node. Nothing new for a consumer to learn -- which is the reason the
  // declaration reuses the binding's shape instead of inventing one.
  EXPECT_TRUE(decl.childOfKind(SyntaxKind::Name).has_value());
  EXPECT_TRUE(decl.childOfKind(SyntaxKind::Type).has_value());
}

TEST(TypeAliasParseTest, ATypePositionOnTheRightIsTheWholeTypeGrammar) {
  // A pointer, an array and a name: the RHS is a *type position*, so every form a
  // binding's annotation accepts is accepted here without the parser knowing
  // anything about aliases. `*[4]i32` is a pointer to an array of four.
  ParseFixture fixture("type Table = *[4]i32;\n");
  ASSERT_TRUE(fixture.built());
  expectLossless(fixture);
  const syntax::SyntaxNode type = aliasDecl(fixture).childOfKind(SyntaxKind::Type).value();
  // The run is the `*`, the `[4]` and the word, in that order: a pointer to an
  // array, and not an array of pointers -- the two orders are the two types.
  EXPECT_EQ(tokenText(type), "* [ 4 ] i32");
}

TEST(TypeAliasParseTest, TheBlockPositionIsTheSameNode) {
  ParseFixture fixture("fn i32 main() { type Local = i32; return 0; }\n");
  ASSERT_TRUE(fixture.built());
  expectLossless(fixture);

  const syntax::SyntaxNode block = fixture.tree()
                                       .root()
                                       .childOfKind(SyntaxKind::FnDecl)
                                       .value()
                                       .childOfKind(SyntaxKind::Block)
                                       .value();
  // A statement among the statements, and the same kind: a tree consumer that
  // knows the file-scope declaration knows this one.
  const syntax::SyntaxNode stmt = block.childOfKind(SyntaxKind::TypeAliasDecl).value();
  EXPECT_EQ(stmt.kind(), SyntaxKind::TypeAliasDecl);
  EXPECT_TRUE(stmt.childOfKind(SyntaxKind::Name).has_value());
  EXPECT_TRUE(stmt.childOfKind(SyntaxKind::Type).has_value());
}

TEST(TypeAliasParseTest, TheBlockPositionIsAlsoALeaderForRecovery) {
  // A `type` heads a statement, which matters for the *token that cannot*: after a
  // mistake the parser resynchronizes on a statement leader, and a keyword that is
  // not one would make a stray `type` into a cascade.
  ParseFixture fixture("fn i32 main() { , type Local = i32; return 0; }\n");
  ASSERT_TRUE(fixture.built());
  EXPECT_EQ(fixture.errorCount(), 1u);
  EXPECT_EQ(fixture.tree().reconstruct(), fixture.source());
}

// --- one half at a time -------------------------------------------------------

TEST(TypeAliasParseTest, AMissingNameIsReportedOnce) {
  ParseFixture fixture("type = i32;\n");
  ASSERT_TRUE(fixture.built());
  ASSERT_EQ(fixture.errorCount(), 1u);
  EXPECT_NE(fixture.errorMessages().find("expected a name"), std::string::npos)
      << fixture.errorMessages();
  EXPECT_EQ(fixture.tree().reconstruct(), fixture.source());
}

TEST(TypeAliasParseTest, AMissingTypeIsReportedOnce) {
  ParseFixture fixture("type Word = ;\n");
  ASSERT_TRUE(fixture.built());
  ASSERT_EQ(fixture.errorCount(), 1u);
  EXPECT_NE(fixture.errorMessages().find("expected a type"), std::string::npos)
      << fixture.errorMessages();
  EXPECT_EQ(fixture.tree().reconstruct(), fixture.source());
}

TEST(TypeAliasParseTest, AMissingEqualsIsReportedOnce) {
  ParseFixture fixture("type Word i32;\n");
  ASSERT_TRUE(fixture.built());
  ASSERT_EQ(fixture.errorCount(), 1u);
  EXPECT_EQ(fixture.tree().reconstruct(), fixture.source());
}

TEST(TypeAliasParseTest, AMissingSemicolonIsReportedOnce) {
  ParseFixture fixture("type Word = i32\nfn i32 main() { return 0; }\n");
  ASSERT_TRUE(fixture.built());
  ASSERT_EQ(fixture.errorCount(), 1u);
  EXPECT_NE(fixture.errorMessages().find("';'"), std::string::npos) << fixture.errorMessages();
  EXPECT_EQ(fixture.tree().reconstruct(), fixture.source());
}

TEST(TypeAliasParseTest, NeitherLinkerWordBelongsToATypeName) {
  // `extern` and `static` are about symbols, and a name for a type never reaches a
  // linker: there is no definition to say is elsewhere and nothing to make
  // internal. Both are refused at the word, with the one code, so the two spellings
  // cannot drift into two sentences.
  ParseFixture external("extern type Word = i32;\n");
  ASSERT_TRUE(external.built());
  EXPECT_EQ(external.tree().reconstruct(), external.source());

  ParseFixture internal("static type Word = i32;\n");
  ASSERT_TRUE(internal.built());
  EXPECT_EQ(internal.tree().reconstruct(), internal.source());
}

} // namespace
} // namespace minc::parse
