// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The two literal forms, and the one decision that tells them apart.
//
// `[` opens a list (`[1, 2, 3]`) and it also opens the *count* of a typed
// initializer (`[3]i32{1, 2, 3}`). The rule that separates them is in
// `Parser::atTypedInitializer`, and the reason it needs tests of its own is that
// a wrong reading here is not a diagnostic: it is a different tree, and the
// second form of one program is the first form of another.
#include <cstddef>
#include <string>

#include <gtest/gtest.h>

#include "parse/parse_fixture.h"
#include "parse/parser.h"
#include "parse/syntax_kind.h"
#include "syntax/node.h"
#include "syntax/tree.h"

namespace minc::parse {
namespace {

using test::ParseFixture;

[[nodiscard]] std::string fnBody(const std::string& statements) {
  return "fn i32 main() { " + statements + " }\n";
}

// The single initializer node of a `let`, by kind, so a test says which of the
// two forms it expected rather than counting nodes.
[[nodiscard]] syntax::SyntaxNode initializerOf(const ParseFixture& fixture) {
  const syntax::SyntaxNode root = fixture.tree().root();
  const syntax::SyntaxNode fn = root.childOfKind(SyntaxKind::FnDecl).value();
  const syntax::SyntaxNode block = fn.childOfKind(SyntaxKind::Block).value();
  const syntax::SyntaxNode stmt = block.childOfKind(SyntaxKind::LetStmt).value();
  for (std::size_t i = 0; i < stmt.childCount(); ++i) {
    const syntax::NodeOrToken child = stmt.child(i);
    if (!child.isToken()) {
      const SyntaxKind kind = child.asNode().kind();
      if (kind == SyntaxKind::TypedInitializer || kind == SyntaxKind::ArrayLiteral) {
        return child.asNode();
      }
    }
  }
  return {};
}

// Every form must stay lossless: the tree concatenated back together has to be
// the source, byte for byte.
void expectLossless(const ParseFixture& fixture) {
  EXPECT_EQ(fixture.errorCount(), 0u) << fixture.errorMessages();
  EXPECT_TRUE(fixture.tree().validate());
  EXPECT_EQ(fixture.reconstruct(), fixture.source());
  EXPECT_TRUE(fixture.tree().stats().lossless);
}

TEST(InitializerTest, TheTwoFormsAreTwoKinds) {
  // A list, and the same elements under a type: two nodes, because a consumer
  // asks for the type *by name* instead of remembering when the first child
  // happens to be one.
  const ParseFixture list(fnBody("let a = [1, 2, 3];"));
  expectLossless(list);
  EXPECT_EQ(initializerOf(list).kind(), SyntaxKind::ArrayLiteral);

  const ParseFixture typed(fnBody("let a = [3]i32{1, 2, 3};"));
  expectLossless(typed);
  EXPECT_EQ(initializerOf(typed).kind(), SyntaxKind::TypedInitializer);
}

TEST(InitializerTest, TheCountIsAWholeTypeRunAndNotOneToken) {
  // `[` after the count group is the *next array*, not an index of a list: the
  // scan steps over a nested `[N]` group as one thing, which is what makes
  // `[2][3]i32{...}` a typed initializer at all.
  const ParseFixture nested(fnBody("let g = [2][3]i32{1, 2, 3, 4, 5, 6};"));
  expectLossless(nested);
  EXPECT_EQ(initializerOf(nested).kind(), SyntaxKind::TypedInitializer);

  // ... and a type run as long as the language's multi-word C specifiers is
  // still followed by the brace it has to be followed by.
  const ParseFixture words(fnBody("let g = [2]unsigned long long int{1, 2};"));
  expectLossless(words);
  EXPECT_EQ(initializerOf(words).kind(), SyntaxKind::TypedInitializer);

  // The pointer element too: `[N]*T{...}` is an array of pointers.
  const ParseFixture pointed(fnBody("let p = [2]*i32{null, null};"));
  expectLossless(pointed);
  EXPECT_EQ(initializerOf(pointed).kind(), SyntaxKind::TypedInitializer);
}

// How many nodes of a kind the whole tree holds. A subscript of a literal puts
// the literal one level down, so the question "which form was this" is asked of
// the tree and not of one child.
[[nodiscard]] std::size_t countNodes(const syntax::SyntaxNode& node, SyntaxKind kind) {
  std::size_t count = node.kind() == kind ? 1U : 0U;
  for (std::size_t i = 0; i < node.childCount(); ++i) {
    const syntax::NodeOrToken child = node.child(i);
    if (!child.isToken()) {
      count += countNodes(child.asNode(), kind);
    }
  }
  return count;
}

TEST(InitializerTest, AListFollowedByASubscriptIsStillAList) {
  // The one shape where the two readings are one token apart: `[1][0]` is
  // *indexing* a one-element list -- there is no brace after the group, so the
  // group was not a count. The parser never backtracks, so this is a rule and not
  // a preference, and a rule that is not tested is a rule that changed silently.
  const ParseFixture indexed(fnBody("let x = [1][0];"));
  expectLossless(indexed);
  EXPECT_EQ(countNodes(indexed.tree().root(), SyntaxKind::ArrayLiteral), 1u);
  EXPECT_EQ(countNodes(indexed.tree().root(), SyntaxKind::TypedInitializer), 0u);

  // A comma in the group is the list, unconditionally -- and a list of two is
  // indexed the same way.
  const ParseFixture comma(fnBody("let x = [1, 2][0];"));
  expectLossless(comma);
  EXPECT_EQ(countNodes(comma.tree().root(), SyntaxKind::ArrayLiteral), 1u);
  EXPECT_EQ(countNodes(comma.tree().root(), SyntaxKind::TypedInitializer), 0u);
}

TEST(InitializerTest, TheBracesHoldBothTheListAndTheFill) {
  // The `;` is the whole difference, and the trailing comma is allowed, so both
  // spellings are pinned here rather than left to the checker's rules to imply.
  const ParseFixture fill(fnBody("let z = [64]u8{0; 64};"));
  expectLossless(fill);
  EXPECT_EQ(initializerOf(fill).kind(), SyntaxKind::TypedInitializer);

  const ParseFixture contextFill(fnBody("let z: [64]u8 = [0; 64];"));
  expectLossless(contextFill);
  EXPECT_EQ(initializerOf(contextFill).kind(), SyntaxKind::ArrayLiteral);

  const ParseFixture trailing(fnBody("let a = [3]i32{1, 2, 3,};"));
  expectLossless(trailing);
  EXPECT_EQ(initializerOf(trailing).kind(), SyntaxKind::TypedInitializer);
}

TEST(InitializerTest, AnInferredCountIsWrittenWithTheSameGrammar) {
  // `_` is an `Identifier` and the count of a typed initializer, and the parser
  // accepts it as a count *token*: whether it is legal there is the reader's
  // question, which is the stage that knows what encloses the type.
  const ParseFixture inferred(fnBody("let d = [_]u8{1, 2, 3};"));
  expectLossless(inferred);
  EXPECT_EQ(initializerOf(inferred).kind(), SyntaxKind::TypedInitializer);

  // A *name* as a count is still the parse error it was: the two spellings are
  // one token kind apart and the difference is kept in the stage that has the
  // bracket.
  const ParseFixture named(fnBody("let d = [n]u8{1};"));
  EXPECT_GT(named.errorCount(), 0u);
}

TEST(InitializerTest, ATypeWrittenAsANameIsAnInitializerToo) {
  // `Row{1, 2, 3}`: the same node, with the type written as a word instead of
  // as a bracketed count. The brace is what says the word was a *type*, which is
  // the whole of the rule -- no symbol table is consulted, and none is needed.
  const ParseFixture named(fnBody("let a = Row{1, 2, 3};"));
  expectLossless(named);
  EXPECT_EQ(initializerOf(named).kind(), SyntaxKind::TypedInitializer);

  const ParseFixture generic(fnBody("let a = Vec<i32>{1, 2, 3, 4};"));
  expectLossless(generic);
  EXPECT_EQ(initializerOf(generic).kind(), SyntaxKind::TypedInitializer);

  // And it is a *value*, so every postfix the language has may follow it.
  const ParseFixture indexed(fnBody("let a = Row{7, 8, 9}[2];"));
  expectLossless(indexed);
  EXPECT_EQ(countNodes(indexed.tree().root(), SyntaxKind::TypedInitializer), 1u);
}

TEST(InitializerTest, AWordInFrontOfABraceIsOnlyATypeOutsideACondition) {
  // The collision this rule has and no other: in a condition a `{` after a word
  // is the *body* of the statement, and `if x { }` is by far the common case. The
  // two inputs below differ in nothing a reader can point at, so the parse that
  // wins is the condition -- and the shape is reported instead of read.
  const ParseFixture plain(fnBody("if x { }"));
  expectLossless(plain);
  EXPECT_EQ(countNodes(plain.tree().root(), SyntaxKind::TypedInitializer), 0u);

  const ParseFixture false_(fnBody("while false { }"));
  expectLossless(false_);
  EXPECT_EQ(countNodes(false_.tree().root(), SyntaxKind::TypedInitializer), 0u);

  // Inside a group the closing token bounds the expression, so the collision
  // cannot happen and the initializer is read: this is the spelling the language
  // asks for, and the one the sentence below names.
  const ParseFixture grouped(fnBody("if (Row{1, 2, 3}[0] > 0) { }"));
  expectLossless(grouped);
  EXPECT_EQ(countNodes(grouped.tree().root(), SyntaxKind::TypedInitializer), 1u);

  // The forbidden shape: one sentence, and the *body* is still read as the body
  // (the initializer is read first, so the `{ ... }` that closes the statement is
  // the block it was written as).
  const ParseFixture forbidden(fnBody("if Row{1, 2, 3}[0] > 0 { return 1; }"));
  EXPECT_EQ(forbidden.errorCount(), 1u) << forbidden.errorMessages();
  EXPECT_EQ(countNodes(forbidden.tree().root(), SyntaxKind::TypedInitializer), 1u);
  EXPECT_EQ(countNodes(forbidden.tree().root(), SyntaxKind::Block), 2u);
  EXPECT_EQ(countNodes(forbidden.tree().root(), SyntaxKind::IfStmt), 1u);
  // The tree is still the source, byte for byte: the report is about the *shape*,
  // and nothing was consumed twice or dropped while it was made.
  EXPECT_EQ(forbidden.reconstruct(), forbidden.source());
  EXPECT_TRUE(forbidden.tree().stats().lossless);

  // And the same for the form with a type argument list, whose `<...>` is what
  // tells the two readings apart without any lookahead into the braces.
  const ParseFixture listed(fnBody("if Vec<i32>{1, 2, 3, 4}[0] > 0 { }"));
  EXPECT_EQ(listed.errorCount(), 1u) << listed.errorMessages();
}

} // namespace
} // namespace minc::parse
