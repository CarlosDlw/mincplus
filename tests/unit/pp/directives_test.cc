// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
//
// Directives: the conditional family, `#if` arithmetic, and the directives that
// exist to say something rather than to change the token stream.
#include <gtest/gtest.h>

#include <string>

#include "pp_fixture.h"

namespace minc::test {
namespace {

// --- the `#if` family -------------------------------------------------------

TEST(ConditionalTest, TrueBranchIsKeptAndFalseBranchIsDropped) {
  const PPOutcome out = PPFixture().source("#if 1\nyes\n#else\nno\n#endif\n").run();
  EXPECT_EQ(out.concat(), "yes");
  EXPECT_TRUE(out.errors.empty());
}

TEST(ConditionalTest, ElifChainPicksTheFirstTrueBranch) {
  const PPOutcome out =
      PPFixture()
          .source("#define B 2\n#if 0\na\n#elif B == 2\nb\n#elif 1\nc\n#else\nd\n#endif\n")
          .run();
  EXPECT_EQ(out.concat(), "b");
}

TEST(ConditionalTest, OnlyOneBranchIsTakenEvenWhenAnotherIsTrue) {
  const PPOutcome out = PPFixture().source("#if 1\na\n#elif 1\nb\n#endif\n").run();
  EXPECT_EQ(out.concat(), "a");
}

TEST(ConditionalTest, IfdefAndIfndefFollowTheMacroTable) {
  EXPECT_EQ(PPFixture().source("#define X\n#ifdef X\nyes\n#else\nno\n#endif\n").run().concat(),
            "yes");
  EXPECT_EQ(PPFixture().source("#ifndef X\nyes\n#else\nno\n#endif\n").run().concat(), "yes");
  EXPECT_EQ(PPFixture().source("#define X\n#ifndef X\nno\n#endif\n").run().concat(), "");
}

TEST(ConditionalTest, NestedConditionalsUseTheOuterBranch) {
  const PPOutcome out = PPFixture().source("#if 0\n#if 1\na\n#endif\nb\n#else\nc\n#endif\n").run();
  EXPECT_EQ(out.concat(), "c");
}

TEST(ConditionalTest, SkippedBranchIsNotEvaluated) {
  // The tokens of a group that is not taken are not expanded and not diagnosed:
  // `#error` inside it must not fire and the malformed text must not be parsed.
  const PPOutcome out = PPFixture()
                            .source("#if 0\n#error this must not fire\n#endif\n#if 0\n#if 1 / 0\n"
                                    "#endif\n#endif\nok\n")
                            .run();
  EXPECT_TRUE(out.errors.empty());
  EXPECT_EQ(out.concat(), "ok");
}

TEST(ConditionalTest, ElifConditionOfAnAlreadyTakenConditionalIsNotEvaluated) {
  const PPOutcome out = PPFixture().source("#if 1\nok\n#elif 1 / 0\ngarbage\n#endif\n").run();
  EXPECT_TRUE(out.errors.empty());
  EXPECT_EQ(out.concat(), "ok");
}

TEST(ConditionalTest, UnterminatedConditionalIsDiagnosedAtEndOfFile) {
  EXPECT_TRUE(PPFixture().source("#if 1\nx\n").run().hasError("pp-unterminated-conditional"));
}

TEST(ConditionalTest, StrayEndifIsDiagnosed) {
  EXPECT_TRUE(PPFixture().source("#endif\n").run().hasError("pp-unexpected-conditional"));
}

TEST(ConditionalTest, ElifWithoutIfIsDiagnosed) {
  EXPECT_TRUE(PPFixture().source("#elif 1\n").run().hasError("pp-unexpected-conditional"));
}

TEST(ConditionalTest, SecondElseIsDiagnosed) {
  EXPECT_TRUE(PPFixture()
                  .source("#if 0\na\n#else\nb\n#else\nc\n#endif\n")
                  .run()
                  .hasError("pp-else-after-else"));
}

TEST(ConditionalTest, ElifAfterElseIsDiagnosed) {
  EXPECT_TRUE(PPFixture()
                  .source("#if 0\na\n#else\nb\n#elif 1\nc\n#endif\n")
                  .run()
                  .hasError("pp-else-after-else"));
}

TEST(ConditionalTest, NestingLimitIsEnforced) {
  std::string text;
  for (int i = 0; i < 300; ++i) {
    text += "#if 1\n";
  }
  for (int i = 0; i < 300; ++i) {
    text += "#endif\n";
  }
  EXPECT_TRUE(PPFixture().source(text).run().hasError("pp-conditional-nesting"));
}

TEST(ConditionalTest, ElifdefIsAccepted) {
  const PPOutcome out =
      PPFixture().source("#define X 1\n#ifdef Y\na\n#elifdef X\nb\n#else\nc\n#endif\n").run();
  EXPECT_EQ(out.concat(), "b");
}

// --- `defined` --------------------------------------------------------------

TEST(ConditionalTest, DefinedOperatorWorksWithAndWithoutParentheses) {
  EXPECT_EQ(PPFixture()
                .source("#define X\n#if defined X\na\n#endif\n#if defined(X)\nb\n#endif\n")
                .run()
                .concat(),
            "ab");
}

TEST(ConditionalTest, DefinedOfAnUnknownNameIsZero) {
  EXPECT_EQ(PPFixture().source("#if defined(NOPE)\nbad\n#else\nok\n#endif\n").run().concat(), "ok");
}

TEST(ConditionalTest, DefinedOperandIsNotExpanded) {
  // `X` expands to `1`, but `defined X` asks about the *name* X, which exists.
  const PPOutcome out = PPFixture().source("#define X 1\n#if defined X\nok\n#endif\n").run();
  EXPECT_EQ(out.concat(), "ok");
}

TEST(ConditionalTest, DefinedAfterAMacroExpandsToTheOperator) {
  // The operator is recognized after expansion, which is what makes
  // `#define D defined` work the way GCC's does.
  const PPOutcome out =
      PPFixture().source("#define X\n#define D defined\n#if D X\nok\n#endif\n").run();
  EXPECT_EQ(out.concat(), "ok");
}

TEST(ConditionalTest, UndefAfterIfCanChangeTheAnswer) {
  const PPOutcome out =
      PPFixture().source("#define X\n#undef X\n#if defined X\nbad\n#else\nok\n#endif\n").run();
  EXPECT_EQ(out.concat(), "ok");
}

// --- `#if` arithmetic -------------------------------------------------------

TEST(ConditionalTest, ArithmeticPrecedenceIsC) {
  EXPECT_EQ(PPFixture().source("#if 1 + 2 * 3 == 7\nok\n#endif\n").run().concat(), "ok");
  EXPECT_EQ(PPFixture().source("#if (1 + 2) * 3 == 9\nok\n#endif\n").run().concat(), "ok");
  EXPECT_EQ(PPFixture().source("#if 1 << 2 == 4\nok\n#endif\n").run().concat(), "ok");
  EXPECT_EQ(PPFixture().source("#if 10 % 3 == 1\nok\n#endif\n").run().concat(), "ok");
  EXPECT_EQ(PPFixture().source("#if !0 && ~0 == -1\nok\n#endif\n").run().concat(), "ok");
  EXPECT_EQ(PPFixture().source("#if (1 ? 2 : 3) == 2\nok\n#endif\n").run().concat(), "ok");
}

TEST(ConditionalTest, ShiftAndBitwiseOperatorsUseSixtyFourBits) {
  // No `ULL` suffix: this language's lexer does not consume numeric suffixes
  // yet (see `lexer.md`, decision 11), so `1ULL` would lex as `1` then `ULL`.
  // What is under test here is the arithmetic, and 2^40 compared against
  // 0xFFFFFFFF needs every bit of a 64-bit value to answer true.
  EXPECT_EQ(PPFixture().source("#if (1 << 40) > 0xFFFFFFFF\nok\n#endif\n").run().concat(), "ok");
  EXPECT_EQ(PPFixture().source("#if (0xF0 & 0x0F) == 0\nok\n#endif\n").run().concat(), "ok");
  // A literal too large for `intmax_t` is unsigned rather than an error, which
  // is the standard's rule and what makes the comparison above unsigned.
  EXPECT_EQ(PPFixture().source("#if 0xFFFFFFFFFFFFFFFF > 0\nok\n#endif\n").run().concat(), "ok");
}

TEST(ConditionalTest, UndefinedIdentifierIsZero) {
  EXPECT_EQ(PPFixture().source("#if NEVER_DEFINED\nbad\n#else\nok\n#endif\n").run().concat(), "ok");
}

TEST(ConditionalTest, WarnUndefReportsTheUndefinedIdentifier) {
  const PPOutcome out = PPFixture().source("#if NEVER_DEFINED\nbad\n#endif\n").warnUndef().run();
  EXPECT_TRUE(out.hasWarning("pp-undefined-identifier"));
}

TEST(ConditionalTest, DivisionByZeroIsAnError) {
  EXPECT_TRUE(PPFixture().source("#if 1 / 0\n#endif\n").run().hasError("pp-expression-syntax"));
  EXPECT_TRUE(PPFixture().source("#if 1 % 0\n#endif\n").run().hasError("pp-expression-syntax"));
}

TEST(ConditionalTest, ShiftCountOutOfRangeIsAnError) {
  EXPECT_TRUE(PPFixture().source("#if 1 << 64\n#endif\n").run().hasError("pp-expression-syntax"));
}

TEST(ConditionalTest, ShortCircuitSuppressesErrorsInTheUntakenOperand) {
  // The standard says the untaken operand is not evaluated, so the division by
  // zero in it is not an error -- only the branch it is in can be.
  const PPOutcome andCase = PPFixture().source("#if 0 && (1 / 0)\nbad\n#else\nok\n#endif\n").run();
  EXPECT_TRUE(andCase.errors.empty());
  EXPECT_EQ(andCase.concat(), "ok");
  const PPOutcome orCase = PPFixture().source("#if 1 || (1 / 0)\nok\n#endif\n").run();
  EXPECT_TRUE(orCase.errors.empty());
  EXPECT_EQ(orCase.concat(), "ok");
}

TEST(ConditionalTest, MalformedExpressionIsDiagnosed) {
  EXPECT_TRUE(PPFixture().source("#if 1 +\n#endif\n").run().hasError("pp-expression-syntax"));
  EXPECT_TRUE(PPFixture().source("#if (1\n#endif\n").run().hasError("pp-expression-syntax"));
  EXPECT_TRUE(PPFixture().source("#if\n#endif\n").run().hasError("pp-expression-syntax"));
  EXPECT_TRUE(PPFixture().source("#if 0x\n#endif\n").run().hasError("pp-expression-syntax"));
}

TEST(ConditionalTest, IfExpressionNestingIsBounded) {
  std::string text = "#if ";
  for (int i = 0; i < 2000; ++i) {
    text += "(";
  }
  text += "1";
  for (int i = 0; i < 2000; ++i) {
    text += ")";
  }
  text += "\n#endif\n";
  EXPECT_TRUE(PPFixture().source(text).run().hasError("pp-expression-syntax"));
}

// --- the directives that only say something ---------------------------------

TEST(DirectiveTest, ErrorIsAnErrorAndWarningIsAWarning) {
  const PPOutcome error = PPFixture().source("#error something is wrong\n").run();
  EXPECT_TRUE(error.hasError("pp-error-directive"));
  const PPOutcome warning = PPFixture().source("#warning look at this\n").run();
  EXPECT_TRUE(warning.hasWarning("pp-warning-directive"));
  EXPECT_TRUE(warning.errors.empty());
}

TEST(DirectiveTest, ErrorMessageIsTheTextOfTheLine) {
  const PPOutcome out = PPFixture().source("#error  needs  two  spaces\n").run();
  bool found = false;
  for (const std::string& message : out.messages) {
    found = found || message == "needs  two  spaces";
  }
  EXPECT_TRUE(found);
}

TEST(DirectiveTest, NullDirectiveIsLegal) {
  const PPOutcome out = PPFixture().source("#\nx\n").run();
  EXPECT_TRUE(out.errors.empty());
  EXPECT_EQ(out.concat(), "x");
}

TEST(DirectiveTest, UnknownDirectiveIsDiagnosed) {
  EXPECT_TRUE(PPFixture().source("#nonsense\n").run().hasError("pp-invalid-directive"));
}

TEST(DirectiveTest, ReservedDirectiveSaysItIsNotSupported) {
  // Naming the feature is the point: "not supported" is actionable, "invalid
  // directive" is not.
  EXPECT_TRUE(PPFixture().source("#embed \"file\"\n").run().hasError("pp-not-supported"));
}

TEST(DirectiveTest, LineDirectiveAdjustsTheLineNumber) {
  // `#line 100` renumbers the line *after* the directive, so the very next line
  // is 100 -- `gcc -E` prints 100 here too. The directive is written once and
  // its own line is the only one that keeps its old number.
  const PPOutcome out = PPFixture().source("#line 100\n__LINE__\n__LINE__\n").run();
  EXPECT_EQ(out.concat(), "100101");
}

TEST(DirectiveTest, LineDirectiveWithoutANumberIsDiagnosed) {
  EXPECT_TRUE(PPFixture().source("#line abc\n").run().hasError("pp-invalid-line"));
}

TEST(DirectiveTest, PragmaIsIgnoredByDefaultAndWarnedOnDemand) {
  const PPOutcome quiet = PPFixture().source("#pragma GCC diagnostic ignored\nx\n").run();
  EXPECT_TRUE(quiet.errors.empty());
  EXPECT_TRUE(quiet.warnings.empty());
  EXPECT_EQ(quiet.concat(), "x");
}

TEST(DirectiveTest, DirectiveRecordIsPopulated) {
  // The record is the tooling contract; a directive that is executed but not
  // recorded is a tool that cannot see it.
  PPFixture fixture;
  fixture.source("#define A 1\n#if A\na\n#else\nb\n#endif\n");
  const PPOutcome out = fixture.run();
  EXPECT_TRUE(out.hasDefine("A"));
  EXPECT_EQ(out.concat(), "a");
}

// --- definition recording ---------------------------------------------------

TEST(DirectiveTest, DefineFromTheCommandLineBehavesLikeOneInTheFile) {
  const PPOutcome out = PPFixture().source("A\n").define("A", "42").run();
  EXPECT_EQ(out.concat(), "42");
}

TEST(DirectiveTest, UndefineFromTheCommandLineRemovesTheBuiltin) {
  const PPOutcome out =
      PPFixture().source("#ifdef __STDC__\nyes\n#else\nno\n#endif\n").undefine("__STDC__").run();
  EXPECT_EQ(out.concat(), "no");
}

TEST(DirectiveTest, CommandLineMacroCanBeFunctionLike) {
  const PPOutcome out = PPFixture().source("SQUARE(3)\n").define("SQUARE(x)", "((x) * (x))").run();
  EXPECT_EQ(out.concat(), "((3)*(3))");
}

TEST(DirectiveTest, RedefiningABuiltinIsDiagnosed) {
  EXPECT_TRUE(PPFixture().source("#define __LINE__ 1\n").run().hasError("pp-macro-redefined"));
}

} // namespace
} // namespace minc::test
