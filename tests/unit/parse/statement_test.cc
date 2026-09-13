// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Statements, checked through the tree they produce.
//
// Two things are worth asserting here and neither is visible from the source
// text alone: the *shape* -- which node ends up inside which, so `else if` is
// known to be an `if` inside an `else` and not a kind of its own -- and that
// every one of these spellings stays lossless, because a statement that ate a
// token would otherwise only show up as a formatter bug much later.
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

// How many direct children of `node` have the given kind. Used instead of
// positional indexing so a test says what it means.
[[nodiscard]] std::size_t countChildren(const syntax::SyntaxNode& node, SyntaxKind kind) {
  std::size_t count = 0;
  for (std::size_t i = 0; i < node.childCount(); ++i) {
    const syntax::NodeOrToken child = node.child(i);
    if (!child.isToken() && child.asNode().kind() == kind) {
      ++count;
    }
  }
  return count;
}

// The spellings of a node's own significant token children, joined by a space.
// Enough to pin a multi-word type without reaching for the source bytes, and
// trivia is skipped because the tree carries it: the parser is blind to it, but
// the builder hangs it inside the node it was written in.
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

// Every statement spelling must survive the round trip: the tree concatenated
// back together has to be the source, byte for byte.
void expectLossless(const ParseFixture& fixture) {
  EXPECT_EQ(fixture.errorCount(), 0u) << fixture.errorMessages();
  EXPECT_TRUE(fixture.tree().validate());
  EXPECT_EQ(fixture.reconstruct(), fixture.source());
  EXPECT_TRUE(fixture.tree().stats().lossless);
}

TEST(StatementTest, IfElseIfElseIsOneChainOfNestedIfs) {
  const ParseFixture fixture(fnBody("if a { b(); } else if c { d(); } else { e(); }"));
  expectLossless(fixture);

  const syntax::SyntaxNode root = fixture.tree().root();
  const syntax::SyntaxNode fn = root.childOfKind(SyntaxKind::FnDecl).value();
  const syntax::SyntaxNode block = fn.childOfKind(SyntaxKind::Block).value();
  // The whole chain is one statement, and the outer block holds exactly one.
  EXPECT_EQ(countChildren(block, SyntaxKind::IfStmt), 1u);

  const syntax::SyntaxNode outer = block.childOfKind(SyntaxKind::IfStmt).value();
  const syntax::SyntaxNode clause = outer.childOfKind(SyntaxKind::ElseClause).value();
  // `else if` is an `if` *inside* the else clause. Asserting the nesting is the
  // whole point: a second node kind for it would parse too and read wrong in
  // every consumer.
  EXPECT_EQ(countChildren(clause, SyntaxKind::IfStmt), 1u);
  EXPECT_EQ(countChildren(clause, SyntaxKind::Block), 0u);

  const syntax::SyntaxNode inner = clause.childOfKind(SyntaxKind::IfStmt).value();
  const syntax::SyntaxNode innerClause = inner.childOfKind(SyntaxKind::ElseClause).value();
  EXPECT_EQ(countChildren(innerClause, SyntaxKind::Block), 1u);
}

TEST(StatementTest, AnIfWithoutElseHasNoElseClause) {
  const ParseFixture fixture(fnBody("if a { b(); }"));
  expectLossless(fixture);

  const syntax::SyntaxNode root = fixture.tree().root();
  const syntax::SyntaxNode fn = root.childOfKind(SyntaxKind::FnDecl).value();
  const syntax::SyntaxNode block = fn.childOfKind(SyntaxKind::Block).value();
  const syntax::SyntaxNode stmt = block.childOfKind(SyntaxKind::IfStmt).value();
  // Absent, not empty: nothing was written, so nothing is there.
  EXPECT_FALSE(stmt.childOfKind(SyntaxKind::ElseClause).has_value());
}

// Parentheses are not part of the statement grammar -- `(cond)` is a
// parenthesised expression and the block simply follows -- so every mixture
// parses, including the one a reader might expect to be rejected.
TEST(StatementTest, ParenthesesAroundConditionsAreOptional) {
  // By value, not by reference: the list holds `const char*` and binding a
  // `const std::string&` to it would materialize a temporary per element that
  // GCC's `-Wrange-loop-construct` flags -- under `-Werror`, that is a build
  // failure in the sanitize preset, which is the point of running two compilers.
  for (const std::string condition : {"a", "(a)", "((a))", "a == b", "(a) == b", "(a == b)",
                                      "(a) && b", "(a) && (b || c)", "!a", "+(1) == 1"}) {
    const ParseFixture fixture(fnBody("if " + condition + " { b(); }"));
    expectLossless(fixture);
  }
}

TEST(StatementTest, WhileTakesAConditionWithOrWithoutParentheses) {
  for (const std::string condition : {"a", "(a)", "a < 10", "(a < 10)"}) {
    const ParseFixture fixture(fnBody("while " + condition + " { b(); }"));
    expectLossless(fixture);

    const syntax::SyntaxNode root = fixture.tree().root();
    const syntax::SyntaxNode fn = root.childOfKind(SyntaxKind::FnDecl).value();
    const syntax::SyntaxNode block = fn.childOfKind(SyntaxKind::Block).value();
    const syntax::SyntaxNode loop = block.childOfKind(SyntaxKind::WhileStmt).value();
    EXPECT_EQ(countChildren(loop, SyntaxKind::Block), 1u);
  }
}

TEST(StatementTest, ForHasThreeKindedClausesAndABody) {
  const ParseFixture fixture(fnBody("for let i = 0; i < 3; i = i + 1 { b(); }"));
  expectLossless(fixture);

  const syntax::SyntaxNode root = fixture.tree().root();
  const syntax::SyntaxNode fn = root.childOfKind(SyntaxKind::FnDecl).value();
  const syntax::SyntaxNode block = fn.childOfKind(SyntaxKind::Block).value();
  const syntax::SyntaxNode loop = block.childOfKind(SyntaxKind::ForStmt).value();

  // Each clause is found by name, which is why the condition and the step have
  // kinds of their own: three bare expressions in a row could only be told
  // apart by counting.
  EXPECT_EQ(countChildren(loop, SyntaxKind::ForCondition), 1u);
  EXPECT_EQ(countChildren(loop, SyntaxKind::ForStep), 1u);
  EXPECT_EQ(countChildren(loop, SyntaxKind::Block), 1u);
  EXPECT_EQ(countChildren(loop, SyntaxKind::LetStmt), 1u);
}

TEST(StatementTest, ForClausesAreOptional) {
  for (const std::string loop : {"for ;; { b(); }", "for (;;) { b(); }", "for ; a; { b(); }",
                                 "for ;; a { b(); }", "for let i = 0; ; { b(); }"}) {
    const ParseFixture fixture(fnBody(loop));
    expectLossless(fixture);

    const syntax::SyntaxNode root = fixture.tree().root();
    const syntax::SyntaxNode fn = root.childOfKind(SyntaxKind::FnDecl).value();
    const syntax::SyntaxNode block = fn.childOfKind(SyntaxKind::Block).value();
    const syntax::SyntaxNode stmt = block.childOfKind(SyntaxKind::ForStmt).value();
    // The clause slots are always there, empty or not, so a consumer never has
    // to guess by counting: an omitted condition is an empty `ForCondition`.
    EXPECT_EQ(countChildren(stmt, SyntaxKind::ForCondition), 1u) << loop;
    EXPECT_EQ(countChildren(stmt, SyntaxKind::ForStep), 1u) << loop;
  }
}

TEST(StatementTest, ForInitializerIsAStatement) {
  // A declaration, an assignment and nothing at all are the three things C
  // allows there, and each is a statement node rather than a special case.
  const ParseFixture decl(fnBody("for let i = 0; i < 1; i = i + 1 { b(); }"));
  expectLossless(decl);
  const ParseFixture assign(fnBody("for i = 0; i < 1; i = i + 1 { b(); }"));
  expectLossless(assign);
  const ParseFixture empty(fnBody("for ;; { b(); }"));
  expectLossless(empty);

  const syntax::SyntaxNode root = assign.tree().root();
  const syntax::SyntaxNode fn = root.childOfKind(SyntaxKind::FnDecl).value();
  const syntax::SyntaxNode block = fn.childOfKind(SyntaxKind::Block).value();
  const syntax::SyntaxNode stmt = block.childOfKind(SyntaxKind::ForStmt).value();
  EXPECT_EQ(countChildren(stmt, SyntaxKind::ExprStmt), 1u);

  const syntax::SyntaxNode emptyRoot = empty.tree().root();
  const syntax::SyntaxNode emptyFn = emptyRoot.childOfKind(SyntaxKind::FnDecl).value();
  const syntax::SyntaxNode emptyBlock = emptyFn.childOfKind(SyntaxKind::Block).value();
  const syntax::SyntaxNode emptyStmt = emptyBlock.childOfKind(SyntaxKind::ForStmt).value();
  EXPECT_EQ(countChildren(emptyStmt, SyntaxKind::EmptyStmt), 1u);
}

TEST(StatementTest, BreakAndContinueAreStatements) {
  const ParseFixture fixture(fnBody("while a { break; continue; }"));
  expectLossless(fixture);

  const syntax::SyntaxNode root = fixture.tree().root();
  const syntax::SyntaxNode fn = root.childOfKind(SyntaxKind::FnDecl).value();
  const syntax::SyntaxNode block = fn.childOfKind(SyntaxKind::Block).value();
  const syntax::SyntaxNode loop = block.childOfKind(SyntaxKind::WhileStmt).value();
  const syntax::SyntaxNode body = loop.childOfKind(SyntaxKind::Block).value();
  EXPECT_EQ(countChildren(body, SyntaxKind::BreakStmt), 1u);
  EXPECT_EQ(countChildren(body, SyntaxKind::ContinueStmt), 1u);
}

TEST(StatementTest, ControlFlowNestsAndStaysBounded) {
  const ParseFixture fixture(fnBody("while a { if b { for ;; { break; } } else { continue; } }"));
  expectLossless(fixture);
}

TEST(StatementTest, ConditionWithNoExpressionIsOneError) {
  const ParseFixture fixture(fnBody("if { b(); }"));
  EXPECT_EQ(fixture.errorCount(), 1u) << fixture.errorMessages();
  EXPECT_EQ(fixture.reconstruct(), fixture.source());
}

// --- parameters --------------------------------------------------------------

TEST(ParamTest, AnnotationFormMatchesTheLanguageBindingSyntax) {
  const ParseFixture fixture("fn i32 add(a: i32, b: i32) { return a + b; }\n");
  expectLossless(fixture);

  const syntax::SyntaxNode root = fixture.tree().root();
  const syntax::SyntaxNode fn = root.childOfKind(SyntaxKind::FnDecl).value();
  const syntax::SyntaxNode params = fn.childOfKind(SyntaxKind::ParamList).value();
  EXPECT_EQ(countChildren(params, SyntaxKind::Param), 2u);
  for (std::size_t i = 0; i < params.childCount(); ++i) {
    const syntax::NodeOrToken child = params.child(i);
    if (child.isToken() || child.asNode().kind() != SyntaxKind::Param) {
      continue;
    }
    // A parameter is a `Name` and then a `Type`, in that order, always.
    EXPECT_TRUE(child.asNode().childOfKind(SyntaxKind::Name).has_value());
    EXPECT_TRUE(child.asNode().childOfKind(SyntaxKind::Type).has_value());
  }
}

TEST(ParamTest, AMultiWordTypeIsOneType) {
  const ParseFixture fixture("fn i32 f(x: unsigned long long int) { return 0; }\n");
  expectLossless(fixture);

  const syntax::SyntaxNode root = fixture.tree().root();
  const syntax::SyntaxNode fn = root.childOfKind(SyntaxKind::FnDecl).value();
  const syntax::SyntaxNode params = fn.childOfKind(SyntaxKind::ParamList).value();
  const syntax::SyntaxNode param = params.childOfKind(SyntaxKind::Param).value();
  const syntax::SyntaxNode type = param.childOfKind(SyntaxKind::Type).value();
  const syntax::SyntaxNode name = param.childOfKind(SyntaxKind::Name).value();
  EXPECT_EQ(tokenText(type), "unsigned long long int");
  EXPECT_EQ(tokenText(name), "x");
}

TEST(ParamTest, ATrailingCommaEndsTheList) {
  const ParseFixture fixture("fn i32 f(a: i32, b: i32,) { return a + b; }\n");
  expectLossless(fixture);
  const syntax::SyntaxNode root = fixture.tree().root();
  const syntax::SyntaxNode fn = root.childOfKind(SyntaxKind::FnDecl).value();
  const syntax::SyntaxNode params = fn.childOfKind(SyntaxKind::ParamList).value();
  EXPECT_EQ(countChildren(params, SyntaxKind::Param), 2u);
}

// The C argument order is rejected, and the rejection is the point: the run of
// identifiers before the boundary cannot say which of its words was meant to be
// the name, so accepting it means silently picking one.
TEST(ParamTest, TheCArgumentOrderIsRejectedByName) {
  for (const std::string source :
       {"fn i32 f(i32 a) { return a; }\n", "fn i32 f(unsigned long) { return 0; }\n"}) {
    const ParseFixture fixture(source);
    EXPECT_EQ(fixture.errorCount(), 1u) << fixture.errorMessages();
    EXPECT_NE(fixture.errorMessages().find("name: type"), std::string::npos)
        << source << ": " << fixture.errorMessages();
    EXPECT_EQ(fixture.reconstruct(), fixture.source()) << source;
  }
}

// The forgotten name and the C order are the same tokens; only the number of
// identifiers tells them apart, and the two messages have to say different
// things because the fixes are different.
TEST(ParamTest, AMissingNameReadsDifferentlyFromTheWrongOrder) {
  const ParseFixture unnamed("fn i32 f(i32) { return 0; }\n");
  EXPECT_EQ(unnamed.errorCount(), 1u) << unnamed.errorMessages();
  EXPECT_EQ(unnamed.errorMessages().find("not `type name`"), std::string::npos)
      << unnamed.errorMessages();

  const ParseFixture wrongOrder("fn i32 f(i32 a) { return a; }\n");
  EXPECT_NE(wrongOrder.errorMessages().find("not `type name`"), std::string::npos)
      << wrongOrder.errorMessages();
}

TEST(ParamTest, EmptyListIsTheEmptyParamList) {
  const ParseFixture fixture("fn i32 main() { return 0; }\n");
  expectLossless(fixture);

  const syntax::SyntaxNode root = fixture.tree().root();
  const syntax::SyntaxNode fn = root.childOfKind(SyntaxKind::FnDecl).value();
  const syntax::SyntaxNode params = fn.childOfKind(SyntaxKind::ParamList).value();
  EXPECT_EQ(countChildren(params, SyntaxKind::Param), 0u);
}

TEST(ParamTest, MissingNameOrTypeIsOneErrorAndStillLossless) {
  for (const std::string source : {"fn i32 f(i32) { return 0; }\n", "fn i32 f(x:) { return 0; }\n",
                                   "fn i32 f(1) { return 0; }\n"}) {
    const ParseFixture fixture(source);
    EXPECT_EQ(fixture.errorCount(), 1u) << source << ": " << fixture.errorMessages();
    EXPECT_EQ(fixture.reconstruct(), fixture.source()) << source;
  }
}

// The rejected run is an `Error` node and not a `Type`, so the type reader
// never sees it. That is what keeps a C-order parameter to a single diagnostic
// instead of a parse error followed by a `malformed type` further up.
TEST(ParamTest, ACRejectedParameterRunIsNotHandedToTheTypeReader) {
  const ParseFixture fixture("fn i32 f(i32 a i32 b) { return 0; }\n");
  EXPECT_EQ(fixture.errorCount(), 1u) << fixture.errorMessages();
  EXPECT_EQ(fixture.reconstruct(), fixture.source());

  const syntax::SyntaxNode fn = fixture.tree().root().childOfKind(SyntaxKind::FnDecl).value();
  const syntax::SyntaxNode params = fn.childOfKind(SyntaxKind::ParamList).value();
  EXPECT_EQ(countChildren(params, SyntaxKind::Param), 1u);
  const syntax::SyntaxNode param = params.childOfKind(SyntaxKind::Param).value();
  EXPECT_FALSE(param.childOfKind(SyntaxKind::Type).has_value());
  EXPECT_TRUE(param.childOfKind(SyntaxKind::Error).has_value());
}

TEST(ParamTest, JunkInTheListStillTerminates) {
  // The loop's progress guarantee, from the outside: whatever is in there, the
  // parser must consume it and stop rather than spin.
  for (const std::string source :
       {"fn i32 f(, , ,) { return 0; }\n", "fn i32 f(;;;) { return 0; }\n",
        "fn i32 f(a: i32 b: i32) { return 0; }\n"}) {
    const ParseFixture fixture(source);
    EXPECT_TRUE(fixture.built()) << source;
    EXPECT_EQ(fixture.reconstruct(), fixture.source()) << source;
  }
}

} // namespace
} // namespace minc::parse
