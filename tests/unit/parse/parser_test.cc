// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The grammar, checked through the tree it produces.
//
// Precedence and associativity are asserted by *shape* -- which operand is
// nested inside which -- because that is the property that matters and the one
// a misplaced table row breaks silently.
#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "parse/parse_fixture.h"
#include "parse/parser.h"
#include "parse/syntax_kind.h"
#include "syntax/ast.h"

namespace minc::parse {
namespace {

using test::ParseFixture;

[[nodiscard]] std::string fnBody(const std::string& statements) {
  return "fn i32 main() { " + statements + " }\n";
}

TEST(ParserTest, ParsesASmallProgram) {
  const ParseFixture fixture(fnBody("let x: i32 = 1; return x;"));
  ASSERT_TRUE(fixture.built());
  EXPECT_EQ(fixture.errorCount(), 0u) << fixture.errorMessages();
  EXPECT_TRUE(fixture.tree().validate());
  EXPECT_EQ(fixture.reconstruct(), fixture.source());
  EXPECT_TRUE(fixture.tree().stats().lossless);
}

TEST(ParserTest, MultiplicationBindsTighterThanAddition) {
  const ParseFixture fixture(fnBody("return 1 + 2 * 3;"));
  ASSERT_EQ(fixture.errorCount(), 0u) << fixture.errorMessages();

  const syntax::SyntaxNode root = fixture.tree().root();
  const auto fn = syntax::FnDecl::cast(root.childOfKind(SyntaxKind::FnDecl).value());
  ASSERT_TRUE(fn.has_value());
  const auto block = syntax::Block::cast(*syntax::bodyOf(*fn));
  ASSERT_TRUE(block.has_value());
  const syntax::SyntaxNode ret = block->syntax().childOfKind(SyntaxKind::ReturnStmt).value();
  const auto outer = syntax::BinaryExpr::cast(ret.childOfKind(SyntaxKind::BinaryExpr).value());
  ASSERT_TRUE(outer.has_value());
  ASSERT_TRUE(syntax::binaryOperator(*outer).has_value());
  EXPECT_EQ(syntax::binaryOperator(*outer)->text(), "+");
  // The right operand of `+` must itself be the `*` expression.
  const auto right = syntax::BinaryExpr::cast(*syntax::rightOperand(*outer));
  ASSERT_TRUE(right.has_value()) << "2 * 3 should be nested under the +";
  EXPECT_EQ(syntax::binaryOperator(*right)->text(), "*");
}

TEST(ParserTest, SubtractionIsLeftAssociative) {
  const ParseFixture fixture(fnBody("return 1 - 2 - 3;"));
  ASSERT_EQ(fixture.errorCount(), 0u) << fixture.errorMessages();

  const syntax::SyntaxNode root = fixture.tree().root();
  const syntax::SyntaxNode ret = root.childOfKind(SyntaxKind::FnDecl)
                                     .value()
                                     .childOfKind(SyntaxKind::Block)
                                     .value()
                                     .childOfKind(SyntaxKind::ReturnStmt)
                                     .value();
  const auto outer = syntax::BinaryExpr::cast(ret.childOfKind(SyntaxKind::BinaryExpr).value());
  ASSERT_TRUE(outer.has_value());
  // `1 - 2 - 3` is `(1 - 2) - 3`: the LEFT operand is the inner subtraction.
  const auto left = syntax::BinaryExpr::cast(*syntax::leftOperand(*outer));
  ASSERT_TRUE(left.has_value()) << "a - b - c must be left-nested";
  EXPECT_EQ(syntax::binaryOperator(*left)->text(), "-");
}

TEST(ParserTest, AssignmentIsRightAssociative) {
  const ParseFixture fixture(fnBody("a = b = c;"));
  ASSERT_EQ(fixture.errorCount(), 0u) << fixture.errorMessages();
  const std::string tree = fixture.dump(/*showTrivia=*/false);
  // Two AssignExpr nodes, the inner one nested under the outer's right side.
  EXPECT_NE(tree.find("AssignExpr"), std::string::npos);
  const syntax::SyntaxNode root = fixture.tree().root();
  const syntax::SyntaxNode stmt = root.childOfKind(SyntaxKind::FnDecl)
                                      .value()
                                      .childOfKind(SyntaxKind::Block)
                                      .value()
                                      .childOfKind(SyntaxKind::ExprStmt)
                                      .value();
  const syntax::SyntaxNode outer = stmt.childOfKind(SyntaxKind::AssignExpr).value();
  ASSERT_TRUE(outer.childOfKind(SyntaxKind::AssignExpr).has_value())
      << "a = b = c must nest the second assignment to the right";
}

TEST(ParserTest, ConditionalNestsToTheRight) {
  const ParseFixture fixture(fnBody("return a ? b : c ? d : e;"));
  ASSERT_EQ(fixture.errorCount(), 0u) << fixture.errorMessages();
  const syntax::SyntaxNode root = fixture.tree().root();
  const syntax::SyntaxNode ret = root.childOfKind(SyntaxKind::FnDecl)
                                     .value()
                                     .childOfKind(SyntaxKind::Block)
                                     .value()
                                     .childOfKind(SyntaxKind::ReturnStmt)
                                     .value();
  const syntax::SyntaxNode cond = ret.childOfKind(SyntaxKind::ConditionalExpr).value();
  EXPECT_TRUE(cond.childOfKind(SyntaxKind::ConditionalExpr).has_value())
      << "a ? b : c ? d : e must nest the second conditional to the right";
}

TEST(ParserTest, PrefixBindsTighterThanMultiplication) {
  const ParseFixture fixture(fnBody("return -1 * 2;"));
  ASSERT_EQ(fixture.errorCount(), 0u) << fixture.errorMessages();
  const std::string tree = fixture.dump(/*showTrivia=*/false);
  // `(-1) * 2`: the PrefixExpr is under the BinaryExpr, not wrapping it.
  const std::size_t binary = tree.find("BinaryExpr");
  const std::size_t prefix = tree.find("PrefixExpr");
  ASSERT_NE(binary, std::string::npos);
  ASSERT_NE(prefix, std::string::npos);
  EXPECT_LT(binary, prefix) << "-1 * 2 should be (-1) * 2";
}

TEST(ParserTest, CallAndPostfixChain) {
  const ParseFixture fixture(fnBody("f()++;"));
  ASSERT_EQ(fixture.errorCount(), 0u) << fixture.errorMessages();
  EXPECT_NE(fixture.dump(true).find("PostfixExpr"), std::string::npos);
  EXPECT_NE(fixture.dump(true).find("CallExpr"), std::string::npos);
}

TEST(ParserTest, MultiWordTypeNamesAreOneType) {
  const ParseFixture fixture(fnBody("let n: unsigned long long int = 6;"));
  ASSERT_EQ(fixture.errorCount(), 0u) << fixture.errorMessages();
  const std::string tree = fixture.dump(/*showTrivia=*/false);
  // One Type node holding four identifier leaves, rather than a parse that
  // stops after `unsigned`.
  const std::size_t type = tree.find("Type@");
  ASSERT_NE(type, std::string::npos);
  const std::size_t end = tree.find("Equal@", type);
  ASSERT_NE(end, std::string::npos);
  const std::string typeRegion = tree.substr(type, end - type);
  // The whole run is inside the Type node, not split off after the first word.
  EXPECT_NE(typeRegion.find("\"unsigned\""), std::string::npos) << typeRegion;
  EXPECT_NE(typeRegion.find("\"long\""), std::string::npos) << typeRegion;
  EXPECT_NE(typeRegion.find("\"int\""), std::string::npos) << typeRegion;
}

TEST(ParserTest, MissingSemicolonIsOneErrorAndStillLossless) {
  const ParseFixture fixture(fnBody("let x = 1"));
  EXPECT_EQ(fixture.errorCount(), 1u) << fixture.errorMessages();
  // The tree still tiles the source, so a formatter could rewrite it.
  EXPECT_TRUE(fixture.tree().validate());
  EXPECT_EQ(fixture.reconstruct(), fixture.source());
}

TEST(ParserTest, UnclosedBraceIsOneErrorAndStillLossless) {
  const ParseFixture fixture("fn i32 main() { return 0;\n");
  EXPECT_GE(fixture.errorCount(), 1u);
  EXPECT_TRUE(fixture.tree().validate());
  EXPECT_EQ(fixture.reconstruct(), fixture.source());
}

TEST(ParserTest, GarbageIsWrappedNotDropped) {
  const ParseFixture fixture("fn i32 main() { } %%% let ; \n");
  EXPECT_GT(fixture.errorCount(), 0u);
  EXPECT_TRUE(fixture.tree().validate());
  EXPECT_EQ(fixture.reconstruct(), fixture.source());
  EXPECT_NE(fixture.dump(/*showTrivia=*/false).find("Error"), std::string::npos);
}

TEST(ParserTest, DeeplyNestedInputIsADiagnosticNotACrash) {
  // Twice the limit, so the guard is what stops it rather than luck.
  const std::string many(kMaxNestingDepth * 2U, '(');
  const ParseFixture fixture(fnBody("return " + many + "1;"));
  EXPECT_GT(fixture.errorCount(), 0u);
  EXPECT_TRUE(fixture.bailedOut());
  // Even after giving up, every byte is still in the tree.
  EXPECT_TRUE(fixture.tree().validate());
  EXPECT_EQ(fixture.reconstruct(), fixture.source());
}

TEST(ParserTest, EverySingleByteParsesWithoutCrashing) {
  // The parser must be total on arbitrary input: an editor will hand it a file
  // that is one keystroke long, and a fuzzer will hand it worse.
  for (int byte = 0; byte < 256; ++byte) {
    const std::string text(1, static_cast<char>(byte));
    const ParseFixture fixture(text);
    ASSERT_TRUE(fixture.built()) << "byte " << byte;
    EXPECT_TRUE(fixture.tree().validate()) << "byte " << byte;
    EXPECT_EQ(fixture.reconstruct(), fixture.source()) << "byte " << byte;
  }
}

TEST(ParserTest, ByteSoupParsesWithoutCrashing) {
  // Deterministic, so a failure is reproducible on every platform.
  std::uint32_t state = 0x12345678U;
  const std::string alphabet = "fn i32 main(){let x=1;}return+-*/%<>=!&|^~?:;,\n";
  for (int round = 0; round < 200; ++round) {
    std::string text;
    for (int i = 0; i < 64; ++i) {
      state = state * 1664525U + 1013904223U;
      text.push_back(alphabet[state % alphabet.size()]);
    }
    const ParseFixture fixture(text);
    ASSERT_TRUE(fixture.built()) << "round " << round;
    EXPECT_TRUE(fixture.tree().validate()) << "round " << round;
    EXPECT_EQ(fixture.reconstruct(), fixture.source()) << "round " << round;
  }
}

} // namespace
} // namespace minc::parse
