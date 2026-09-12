// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The tree layer: losslessness, the cursor, the typed view, sharing, and the
// dump. These are the guarantees every later stage leans on.
#include <cstddef>
#include <string>

#include <gtest/gtest.h>

#include "parse/parse_fixture.h"
#include "parse/syntax_kind.h"
#include "syntax/ast.h"
#include "syntax/node.h"

namespace minc::syntax {
namespace {

using test::ParseFixture;

constexpr const char* kProgram = "fn i32 add() { let a: i32 = 1; return a; }\n"
                                 "fn i32 main() { f(); return add(); }\n";

TEST(TreeTest, ReconstructsTheSourceExactly) {
  const ParseFixture fixture(kProgram);
  ASSERT_TRUE(fixture.built());
  EXPECT_EQ(fixture.errorCount(), 0u) << fixture.errorMessages();
  EXPECT_TRUE(fixture.tree().stats().lossless);
  EXPECT_TRUE(fixture.tree().validate());
  EXPECT_EQ(fixture.reconstruct(), fixture.source());
}

TEST(TreeTest, RootIsAFileCoveringEveryByte) {
  const ParseFixture fixture(kProgram);
  const SyntaxNode root = fixture.tree().root();
  EXPECT_EQ(root.kind(), parse::SyntaxKind::File);
  EXPECT_EQ(root.offset(), 0u);
  EXPECT_EQ(root.end(), static_cast<std::uint32_t>(fixture.source().size()));
}

TEST(TreeTest, ChildAccessorsFindTheRightNodes) {
  const ParseFixture fixture(kProgram);
  const SyntaxNode root = fixture.tree().root();
  const auto fn = FnDecl::cast(root.childOfKind(parse::SyntaxKind::FnDecl).value());
  ASSERT_TRUE(fn.has_value());

  const auto name = nameOf(*fn);
  ASSERT_TRUE(name.has_value());
  EXPECT_EQ(identifierText(*name), "add");

  const auto ret = returnTypeOf(*fn);
  ASSERT_TRUE(ret.has_value());
  EXPECT_EQ(identifierText(*ret), "i32");

  const auto params = parameterListOf(*fn);
  ASSERT_TRUE(params.has_value());
  EXPECT_EQ(params->childCount(), 0u) << "the parameter list is empty";

  const auto body = bodyOf(*fn);
  ASSERT_TRUE(body.has_value());
  EXPECT_EQ(body->kind(), parse::SyntaxKind::Block);
}

TEST(TreeTest, LetAndConstShareOneWrapper) {
  const ParseFixture fixture("fn i32 main() { let a = 1; const b = 2; }\n");
  ASSERT_EQ(fixture.errorCount(), 0u) << fixture.errorMessages();

  const SyntaxNode block = fixture.tree()
                               .root()
                               .childOfKind(parse::SyntaxKind::FnDecl)
                               .value()
                               .childOfKind(parse::SyntaxKind::Block)
                               .value();

  const auto letStmt = VariableStmt::cast(block.childOfKind(parse::SyntaxKind::LetStmt).value());
  ASSERT_TRUE(letStmt.has_value());
  EXPECT_FALSE(letStmt->isConst());
  EXPECT_TRUE(variableInitializer(*letStmt).has_value());
  EXPECT_EQ(identifierText(*variableName(*letStmt)), "a");
  // No annotation, so no Type child: a half-typed declaration is still a
  // declaration, not an error.
  EXPECT_FALSE(variableType(*letStmt).has_value());

  const auto constStmt =
      VariableStmt::cast(block.childOfKind(parse::SyntaxKind::ConstStmt).value());
  ASSERT_TRUE(constStmt.has_value());
  EXPECT_TRUE(constStmt->isConst());
}

TEST(TreeTest, BinaryOperandsAndOperatorAreRecoverable) {
  const ParseFixture fixture("fn i32 main() { return 1 + 2 * 3; }\n");
  ASSERT_EQ(fixture.errorCount(), 0u) << fixture.errorMessages();

  const SyntaxNode ret = fixture.tree()
                             .root()
                             .childOfKind(parse::SyntaxKind::FnDecl)
                             .value()
                             .childOfKind(parse::SyntaxKind::Block)
                             .value()
                             .childOfKind(parse::SyntaxKind::ReturnStmt)
                             .value();
  const auto expr = BinaryExpr::cast(ret.childOfKind(parse::SyntaxKind::BinaryExpr).value());
  ASSERT_TRUE(expr.has_value());
  ASSERT_TRUE(binaryOperator(*expr).has_value());
  EXPECT_EQ(binaryOperator(*expr)->text(), "+");
  EXPECT_TRUE(leftOperand(*expr).has_value());
  EXPECT_TRUE(rightOperand(*expr).has_value());
}

TEST(TreeTest, CallCalleeAndArgumentsAreRecoverable) {
  const ParseFixture fixture("fn i32 main() { g(1, 2); }\n");
  ASSERT_EQ(fixture.errorCount(), 0u) << fixture.errorMessages();

  const SyntaxNode stmt = fixture.tree()
                              .root()
                              .childOfKind(parse::SyntaxKind::FnDecl)
                              .value()
                              .childOfKind(parse::SyntaxKind::Block)
                              .value()
                              .childOfKind(parse::SyntaxKind::ExprStmt)
                              .value();
  const auto call = CallExpr::cast(stmt.childOfKind(parse::SyntaxKind::CallExpr).value());
  ASSERT_TRUE(call.has_value());
  const auto target = callee(*call);
  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(identifierText(*target), "g");
  const auto args = argumentList(*call);
  ASSERT_TRUE(args.has_value());
  // The comma lives inside the argument list, not on the call itself.
  EXPECT_TRUE(args->tokenOfKind(parse::toSyntaxKind(lex::TokenKind::Comma)).has_value());
  EXPECT_EQ(args->nodeChildren().size(), 2u);
}

// Green nodes are position-free, so identical subtrees are the *same* pointer.
// That sharing is what makes the tree a DAG, and it is what a later incremental
// reparse will recognise unchanged subtrees by.
TEST(TreeTest, IdenticalSubtreesAreShared) {
  const ParseFixture fixture("fn i32 a() {}\nfn i32 b() {}\n");
  ASSERT_EQ(fixture.errorCount(), 0u) << fixture.errorMessages();

  const SyntaxNode root = fixture.tree().root();
  const std::vector<SyntaxNode> functions = root.nodeChildren();
  ASSERT_EQ(functions.size(), 2u);
  const auto first = parameterListOf(*FnDecl::cast(functions[0]));
  const auto second = parameterListOf(*FnDecl::cast(functions[1]));
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(first->green(), second->green()) << "two empty parameter lists should be one node";
}

TEST(TreeTest, DumpIsAlignedAndDeterministic) {
  const ParseFixture fixture("fn i32 main() { return 0; }\n");
  const std::string withTrivia = fixture.dump(/*showTrivia=*/true);
  const std::string withoutTrivia = fixture.dump(/*showTrivia=*/false);

  EXPECT_NE(withTrivia.find("File@0.."), std::string::npos);
  EXPECT_NE(withTrivia.find("FnDecl@"), std::string::npos);
  EXPECT_NE(withTrivia.find("Whitespace@"), std::string::npos);

  // Trivia is the bulk of the tree and never what a grammar test looks at.
  EXPECT_EQ(withoutTrivia.find("Whitespace@"), std::string::npos);
  EXPECT_EQ(withoutTrivia.find("Newline@"), std::string::npos);
  EXPECT_NE(withoutTrivia.find("KwFn@"), std::string::npos);

  // No addresses, so the same input gives the same bytes on every run -- which
  // is what lets the golden files be compared.
  EXPECT_EQ(withTrivia.find("0x"), std::string::npos);
  EXPECT_EQ(fixture.dump(true), withTrivia);
}

TEST(TreeTest, AppendTextReproducesASubtree) {
  const ParseFixture fixture("fn i32 main() { return 42; }\n");
  const SyntaxNode ret = fixture.tree()
                             .root()
                             .childOfKind(parse::SyntaxKind::FnDecl)
                             .value()
                             .childOfKind(parse::SyntaxKind::Block)
                             .value()
                             .childOfKind(parse::SyntaxKind::ReturnStmt)
                             .value();
  std::string text;
  appendText(ret, text);
  // Trivia goes to the innermost node that is open when it is flushed, so a
  // node's text is exactly the source it covers. Checking against the source
  // range is the real invariant; the literal below just makes the intent clear.
  EXPECT_EQ(text, fixture.source().substr(ret.offset(), ret.width()));
  EXPECT_NE(text.find("return 42;"), std::string::npos) << text;
}

} // namespace
} // namespace minc::syntax
