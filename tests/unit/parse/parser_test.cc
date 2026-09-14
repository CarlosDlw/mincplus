// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The grammar, checked through the tree it produces.
//
// Precedence and associativity are asserted by *shape* -- which operand is
// nested inside which -- because that is the property that matters and the one
// a misplaced table row breaks silently.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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

TEST(ParserTest, APointerTypeIsPartOfTheTypeRun) {
  const ParseFixture fixture(fnBody("let p: *i32 = null;"));
  ASSERT_EQ(fixture.errorCount(), 0u) << fixture.errorMessages();
  const std::string tree = fixture.dump(/*showTrivia=*/false);
  const std::size_t type = tree.find("Type@");
  ASSERT_NE(type, std::string::npos);
  const std::size_t end = tree.find("Equal@", type);
  ASSERT_NE(end, std::string::npos);
  const std::string typeRegion = tree.substr(type, end - type);
  // The `*` belongs to the Type node, not to the initializer: the annotation is
  // `*i32`, and the parser does not decide whether the word after it is a type
  // or an expression -- that is `sema`'s -- so it collects the run whole.
  EXPECT_NE(typeRegion.find("\"*\""), std::string::npos) << typeRegion;
  EXPECT_NE(typeRegion.find("\"i32\""), std::string::npos) << typeRegion;
}

TEST(ParserTest, AStarIsATypeInAnAnnotationAndMultiplicationInAnInitializer) {
  // There is no ambiguity to resolve, and this pins both readings at once: a
  // complete expression precedes the infix `*`, an annotation never does.
  const ParseFixture fixture(fnBody("let q: i32 = a * b; let p: *i32 = &a;"));
  ASSERT_EQ(fixture.errorCount(), 0u) << fixture.errorMessages();
  const std::string tree = fixture.dump(/*showTrivia=*/false);
  const std::size_t multiply = tree.find("BinaryExpr");
  ASSERT_NE(multiply, std::string::npos);
  // The annotation's star is inside a Type node, not a BinaryExpr.
  const std::size_t pointerType = tree.find("Type@", multiply);
  ASSERT_NE(pointerType, std::string::npos);
  const std::size_t annotationEnd = tree.find("Equal@", pointerType);
  ASSERT_NE(annotationEnd, std::string::npos);
  EXPECT_NE(tree.substr(pointerType, annotationEnd - pointerType).find("\"*\""), std::string::npos);
}

TEST(ParserTest, AddressOfAndDerefArePrefixExpressions) {
  const ParseFixture fixture(fnBody("let y: i32 = *p;"));
  ASSERT_EQ(fixture.errorCount(), 0u) << fixture.errorMessages();
  EXPECT_NE(fixture.dump(/*showTrivia=*/false).find("PrefixExpr"), std::string::npos);

  const ParseFixture address(fnBody("let a: *i32 = &x;"));
  ASSERT_EQ(address.errorCount(), 0u) << address.errorMessages();
  EXPECT_NE(address.dump(/*showTrivia=*/false).find("PrefixExpr"), std::string::npos);
}

TEST(ParserTest, IndexIsAnIndexExpressionAndKeepsItsBrackets) {
  const ParseFixture fixture(fnBody("let y: i32 = p[0];"));
  ASSERT_EQ(fixture.errorCount(), 0u) << fixture.errorMessages();
  const std::string tree = fixture.dump(/*showTrivia=*/false);
  EXPECT_NE(tree.find("IndexExpr"), std::string::npos);
  EXPECT_NE(tree.find("\"[\""), std::string::npos) << tree;
  EXPECT_NE(tree.find("\"]\""), std::string::npos) << tree;
  // Lossless like every other shape: a formatter could rewrite it.
  EXPECT_EQ(fixture.reconstruct(), fixture.source());
}

TEST(ParserTest, TheWholePointerSurfaceIsLossless) {
  const ParseFixture fixture(
      fnBody("let x: i32 = 1; let p: *i32 = &x; *p = x; let y: i32 = p[1]; return y;"));
  ASSERT_EQ(fixture.errorCount(), 0u) << fixture.errorMessages();
  EXPECT_TRUE(fixture.tree().validate());
  EXPECT_EQ(fixture.reconstruct(), fixture.source());
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
  const std::string many(support::kMaxNestingDepth * 2U, '(');
  const ParseFixture fixture(fnBody("return " + many + "1;"));
  EXPECT_GT(fixture.errorCount(), 0u);
  EXPECT_TRUE(fixture.bailedOut());
  // Even after giving up, every byte is still in the tree.
  EXPECT_TRUE(fixture.tree().validate());
  EXPECT_EQ(fixture.reconstruct(), fixture.source());
}

// Each of these shapes recurses through a production that does not pass back
// through the expression entry point on the way down, so a guard on the entry
// point alone leaves input that overflows the stack. They were real crashes
// before each production guarded itself, which is why they are pinned here
// rather than folded into the parenthesis case above.
TEST(ParserTest, EveryRecursiveProductionGuardsItself) {
  const std::size_t depth = static_cast<std::size_t>(support::kMaxNestingDepth) * 2U;

  std::string assignmentChain;
  std::string conditionalChain = "1";
  for (std::size_t i = 0; i < depth; ++i) {
    assignmentChain += "a = ";
    conditionalChain += " ? 2 : 1";
  }

  struct Case {
    const char* label;
    std::string source;
  };
  const std::vector<Case> cases = {
      {"prefix chain", fnBody("return " + std::string(depth, '-') + "1;")},
      {"logical-not chain", fnBody("return " + std::string(depth, '!') + "1;")},
      {"assignment chain", fnBody(assignmentChain + "1;")},
      {"conditional chain", fnBody("return " + conditionalChain + ";")},
      {"nested blocks",
       "fn i32 main() " + std::string(depth, '{') + std::string(depth, '}') + "\n"},
  };

  for (const Case& testCase : cases) {
    const ParseFixture fixture(testCase.source);
    ASSERT_TRUE(fixture.built()) << testCase.label;
    EXPECT_TRUE(fixture.bailedOut()) << testCase.label << " should hit the depth guard";
    EXPECT_GT(fixture.errorCount(), 0u) << testCase.label;
    // Giving up is a recovery, not a shortcut: every byte is still in the tree.
    EXPECT_TRUE(fixture.tree().validate()) << testCase.label;
    EXPECT_EQ(fixture.reconstruct(), fixture.source()) << testCase.label;
  }
}

TEST(ParserTest, AnExternDeclarationIsOneNodeWithNoBody) {
  // `extern fn ...;` and `fn ... { }` are one kind and not two. The shape is the
  // same -- return type, name, parameters -- and the difference is the word in
  // front plus whether a body follows, so a consumer asks for the body and reads
  // an empty answer as "this is a declaration".
  const ParseFixture fixture("extern fn i32 puts(s: str);\n");
  ASSERT_TRUE(fixture.built());
  ASSERT_EQ(fixture.errorCount(), 0u) << fixture.errorMessages();
  EXPECT_TRUE(fixture.tree().validate());
  EXPECT_EQ(fixture.reconstruct(), fixture.source());

  const syntax::SyntaxNode root = fixture.tree().root();
  const auto decl = syntax::FnDecl::cast(root.childOfKind(SyntaxKind::FnDecl).value());
  ASSERT_TRUE(decl.has_value());
  EXPECT_TRUE(decl->isExtern());
  EXPECT_FALSE(syntax::bodyOf(*decl).has_value());
  EXPECT_EQ(syntax::identifierText(*syntax::returnTypeOf(*decl)), "i32");
  EXPECT_EQ(syntax::identifierText(*syntax::nameOf(*decl)), "puts");
}

TEST(ParserTest, ADefinitionIsNotExternAndHasABody) {
  const ParseFixture fixture(fnBody("return 0;"));
  ASSERT_TRUE(fixture.built());
  ASSERT_EQ(fixture.errorCount(), 0u) << fixture.errorMessages();

  const auto decl =
      syntax::FnDecl::cast(fixture.tree().root().childOfKind(SyntaxKind::FnDecl).value());
  ASSERT_TRUE(decl.has_value());
  EXPECT_FALSE(decl->isExtern());
  EXPECT_TRUE(syntax::bodyOf(*decl).has_value());
}

TEST(ParserTest, EitherFormWrittenAsTheOtherIsOneDiagnostic) {
  // Two ways to write one form and mean the other, and each names the word that
  // is missing or misplaced. One mistake, one diagnostic -- and the tree still
  // holds every byte, because the form is built either way.
  const ParseFixture bodyless("fn i32 f();\nfn i32 main() { return 0; }\n");
  ASSERT_TRUE(bodyless.built());
  EXPECT_EQ(bodyless.errorCount(), 1u) << bodyless.errorMessages();
  EXPECT_NE(bodyless.errorMessages().find("parse-missing-extern"), std::string::npos)
      << bodyless.errorMessages();
  EXPECT_NE(bodyless.errorMessages().find("`extern fn`"), std::string::npos)
      << bodyless.errorMessages();
  EXPECT_EQ(bodyless.reconstruct(), bodyless.source());

  const ParseFixture withBody("extern fn i32 f() { return 0; }\n");
  ASSERT_TRUE(withBody.built());
  EXPECT_EQ(withBody.errorCount(), 1u) << withBody.errorMessages();
  EXPECT_NE(withBody.errorMessages().find("parse-extern-with-body"), std::string::npos)
      << withBody.errorMessages();
  EXPECT_EQ(withBody.reconstruct(), withBody.source());
}

TEST(ParserTest, ADeclarationStopsTheRecoveryTheWayADefinitionDoes) {
  // The file loop and the item recovery ask one predicate for "can this token
  // start a declaration", so a construct the parser could not understand stops
  // at the next `fn` *and* at the next `extern` rather than swallowing it.
  const ParseFixture fixture("%%%\nextern fn i32 puts(s: str);\n");
  ASSERT_TRUE(fixture.built());
  EXPECT_EQ(fixture.errorCount(), 1u) << fixture.errorMessages();
  EXPECT_EQ(fixture.reconstruct(), fixture.source());
  EXPECT_EQ(fixture.tree().root().nodeChildren().size(), 2u) << fixture.dump(false);
}

TEST(ParserTest, AVariadicMarkerIsTheLastChildOfTheParameterList) {
  // `...` is a *node* of its own and not a flag anywhere: a list that ends in
  // one says so structurally, and the arity a signature has does not count it --
  // which is why it is not a `Param`.
  const ParseFixture fixture("extern fn i32 printf(fmt: str, ...);\n");
  ASSERT_TRUE(fixture.built());
  ASSERT_EQ(fixture.errorCount(), 0u) << fixture.errorMessages();
  EXPECT_TRUE(fixture.tree().validate());
  EXPECT_EQ(fixture.reconstruct(), fixture.source());

  const syntax::SyntaxNode root = fixture.tree().root();
  const auto decl = syntax::FnDecl::cast(root.childOfKind(SyntaxKind::FnDecl).value());
  ASSERT_TRUE(decl.has_value());
  EXPECT_TRUE(decl->isExtern());
  EXPECT_TRUE(syntax::isVariadic(*decl));

  const syntax::SyntaxNode params = *syntax::parameterListOf(*decl);
  EXPECT_TRUE(params.childOfKind(SyntaxKind::VariadicParam).has_value());
  // One `Param` and one marker: the count of parameters is the count of `Param`
  // children, and the marker is not one of them.
  EXPECT_EQ(params.childOfKind(SyntaxKind::Param).has_value(), true);
  EXPECT_TRUE(params.nodeChildren().back().kind() == SyntaxKind::VariadicParam);
}

TEST(ParserTest, ADefinitionIsNeverVariadic) {
  const ParseFixture fixture(fnBody("return 0;"));
  ASSERT_TRUE(fixture.built());
  const auto decl =
      syntax::FnDecl::cast(fixture.tree().root().childOfKind(SyntaxKind::FnDecl).value());
  ASSERT_TRUE(decl.has_value());
  EXPECT_FALSE(syntax::isVariadic(*decl));
}

TEST(ParserTest, TheVariadicMarkerInTheWrongPlaceIsOneDiagnostic) {
  // Three ways to get the marker wrong, and each costs exactly one message with
  // every byte still in the tree -- including the extra parameters, which are
  // consumed as junk rather than left for `)` to complain about a second time.
  struct Case {
    const char* source;
    const char* code;
  };
  const Case cases[] = {
      {"extern fn i32 f(...);\n", "parse-variadic-position"},
      {"extern fn i32 f(a: i32, ..., b: i32);\n", "parse-variadic-position"},
      {"fn i32 f(a: i32, ...) { return a; }\n", "parse-variadic-definition"},
  };
  for (const Case& testCase : cases) {
    const ParseFixture fixture(testCase.source);
    ASSERT_TRUE(fixture.built()) << testCase.source;
    EXPECT_EQ(fixture.errorCount(), 1u) << testCase.source << ": " << fixture.errorMessages();
    EXPECT_NE(fixture.errorMessages().find(testCase.code), std::string::npos)
        << testCase.source << ": " << fixture.errorMessages();
    EXPECT_EQ(fixture.reconstruct(), fixture.source()) << testCase.source;
  }
}

TEST(ParserTest, ANeverReturnTypeIsATokenInTheRun) {
  // `!` takes part in the type run exactly where a word would, so the run splits
  // the same way `fn i32 f()` splits: everything before the last identifier is
  // the type, and that identifier is the name. Nothing new had to be added to the
  // grammar for this -- which is the whole argument for a punctuator over a
  // reserved word.
  const ParseFixture fixture("extern fn ! exit(code: i32);\n");
  ASSERT_TRUE(fixture.built());
  ASSERT_EQ(fixture.errorCount(), 0u) << fixture.errorMessages();
  EXPECT_TRUE(fixture.tree().validate());
  EXPECT_EQ(fixture.reconstruct(), fixture.source());

  const auto decl =
      syntax::FnDecl::cast(fixture.tree().root().childOfKind(SyntaxKind::FnDecl).value());
  ASSERT_TRUE(decl.has_value());
  EXPECT_TRUE(decl->isExtern());
  const std::optional<syntax::SyntaxNode> returned = syntax::returnTypeOf(*decl);
  ASSERT_TRUE(returned.has_value());
  // The type node holds a `!` and *no word*: a `!` type is not a run of
  // identifiers, which is exactly what `identifierText` would report.
  EXPECT_TRUE(returned->tokenOfKind(parse::toSyntaxKind(lex::TokenKind::Bang)).has_value());
  EXPECT_TRUE(syntax::identifierText(*returned).empty());
  std::string text;
  syntax::appendText(*returned, text);
  EXPECT_NE(text.find('!'), std::string::npos) << text;
  const std::optional<syntax::SyntaxNode> name = syntax::nameOf(*decl);
  ASSERT_TRUE(name.has_value());
  EXPECT_EQ(syntax::identifierText(*name), "exit");
}

TEST(ParserTest, ThePositionOfANeverTypeIsNotTheParsersQuestion) {
  // `let x: !` is a shape the grammar has, and the *parser* accepts it: which
  // position a type is legal in is a question about what a type means, and that
  // belongs to the stage that has types. Both of these parse without a diagnostic
  // and are refused one stage later, with a sentence that names the word.
  const char* const kSources[] = {
      "fn i32 main() { let x: ! = 1; return 0; }\n",
      "fn i32 main() { let x: ! ! = 1; return 0; }\n",
  };
  for (const char* source : kSources) {
    const ParseFixture fixture(source);
    ASSERT_TRUE(fixture.built()) << source;
    EXPECT_EQ(fixture.errorCount(), 0u) << source << ": " << fixture.errorMessages();
    EXPECT_EQ(fixture.reconstruct(), fixture.source()) << source;
  }
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
