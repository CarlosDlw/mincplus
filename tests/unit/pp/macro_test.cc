// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
//
// Macro replacement, rule by rule. Every expectation here is either the
// standard's example verbatim or an outcome GCC and Clang agree on; the ones
// that are neither are marked, because "we chose this" and "the standard says
// so" are different claims.
#include <gtest/gtest.h>

#include <string>

#include "pp_fixture.h"

namespace minc::test {
namespace {

TEST(MacroTest, ObjectLikeExpandsEveryOccurrence) {
  const PPOutcome out = PPFixture().source("#define A x\nA A\n").run();
  EXPECT_EQ(out.concat(), "xx");
  EXPECT_TRUE(out.errors.empty());
}

TEST(MacroTest, ReplacementListIsRescanned) {
  // The body of `A` is `B`, and the `B` it produces is expanded in turn: the
  // replacement list is rescanned, not just copied.
  const PPOutcome out = PPFixture().source("#define A B\n#define B 1\nA\n").run();
  EXPECT_EQ(out.concat(), "1");
}

TEST(MacroTest, FunctionLikeSubstitutesArguments) {
  const PPOutcome out =
      PPFixture().source("#define MAX(a, b) ((a) > (b) ? (a) : (b))\nMAX(1, 2)\n").run();
  EXPECT_EQ(out.concat(), "((1)>(2)?(1):(2))");
}

TEST(MacroTest, ArgumentsArePreExpanded) {
  const PPOutcome out = PPFixture().source("#define ID(x) x\n#define N 5\nID(N)\n").run();
  EXPECT_EQ(out.concat(), "5");
}

TEST(MacroTest, FunctionLikeNameWithoutParenthesisIsNotInvoked) {
  const PPOutcome out = PPFixture().source("#define F(x) x\nF\n").run();
  EXPECT_EQ(out.concat(), "F");
  EXPECT_TRUE(out.errors.empty());
}

TEST(MacroTest, RescanningContinuesAcrossTheReplacement) {
  // The standard's example (C11 6.10.3.4): `f(2)(9)` must become `2*9*g`, not
  // `2*g(9)`. Getting this wrong is the classic rescanning bug.
  const PPOutcome out = PPFixture().source("#define f(a) a*g\n#define g(a) f(a)\nf(2)(9)\n").run();
  EXPECT_EQ(out.concat(), "2*9*g");
}

TEST(MacroTest, NameNotReplacedWhileItsOwnMacroIsDisabled) {
  // The other half of the same example: the `foo` that came out of the argument
  // is not available for replacement during the scan of the replacement list, so
  // the following `(2)` is not an invocation.
  const PPOutcome out = PPFixture().source("#define foo(x) x\nfoo(foo) (2)\n").run();
  EXPECT_EQ(out.concat(), "foo(2)");
}

TEST(MacroTest, BluePaintStopsMutualRecursion) {
  const PPOutcome out = PPFixture().source("#define A B\n#define B A\nA\n").run();
  EXPECT_EQ(out.concat(), "A");
  EXPECT_TRUE(out.errors.empty());
}

TEST(MacroTest, SelfReferenceGetsTheIneligibleMark) {
  const PPOutcome out = PPFixture().source("#define A A\nA\n").run();
  EXPECT_EQ(out.concat(), "A");
}

TEST(MacroTest, EmptyArgumentContributesNothing) {
  const PPOutcome out = PPFixture().source("#define F(x) [x]\nF()\n").run();
  EXPECT_EQ(out.concat(), "[]");
}

TEST(MacroTest, ArgumentsAreSplitAtTopLevelCommasOnly) {
  const PPOutcome out = PPFixture().source("#define SECOND(a, b) b\nSECOND((1, 2), 3)\n").run();
  EXPECT_EQ(out.concat(), "3");
}

// --- `#` and `##` -----------------------------------------------------------

TEST(MacroTest, HashStringifiesTheUnexpandedArgument) {
  const PPOutcome out = PPFixture().source("#define S(x) #x\n#define N 5\nS(N)\n").run();
  EXPECT_EQ(out.concat(), "\"N\"");
}

TEST(MacroTest, HashStringifiesSpacingAsWritten) {
  // One space per run of whitespace, not per token: `a  +  b` is `"a + b"`.
  const PPOutcome out = PPFixture().source("#define S(x) #x\nS(a  +  b)\n").run();
  EXPECT_EQ(out.concat(), "\"a + b\"");
}

TEST(MacroTest, HashRespellsLiteralsFromSource) {
  // From the source bytes, not from the kind: `0x1F` must not become `31`.
  const PPOutcome out = PPFixture().source("#define S(x) #x\nS(0x1F) S('x')\n").run();
  EXPECT_EQ(out.concat(), "\"0x1F\"\"'x'\"");
}

TEST(MacroTest, HashEscapesQuotesAndBackslashes) {
  // The argument is the literal `"a\\b"` -- a, two backslashes, b, between
  // quotes. `#` escapes the quotes and *each* backslash, so the stringified
  // literal is spelled `"\"a\\\\b\""`; `gcc -E` prints exactly that.
  const PPOutcome out = PPFixture().source("#define S(x) #x\nS(\"a\\\\b\")\n").run();
  EXPECT_EQ(out.concat(), R"("\"a\\\\b\"")");
}

TEST(MacroTest, PasteConcatenatesSpellings) {
  const PPOutcome out = PPFixture().source("#define C(a, b) a##b\nC(my, Name)\n").run();
  EXPECT_EQ(out.concat(), "myName");
}

TEST(MacroTest, PasteDoesNotExpandItsOperands) {
  // `my` names a macro, and `##` must use the spelling, not the value.
  const PPOutcome out =
      PPFixture().source("#define my 7\n#define C(a, b) a##b\nC(my, Name)\n").run();
  EXPECT_EQ(out.concat(), "myName");
}

TEST(MacroTest, PasteWithAnEmptyOperandKeepsTheOtherSide) {
  const PPOutcome right = PPFixture().source("#define C(a, b) a##b\nC(x,)\n").run();
  EXPECT_EQ(right.concat(), "x");
  const PPOutcome left = PPFixture().source("#define C(a, b) a##b\nC(,x)\n").run();
  EXPECT_EQ(left.concat(), "x");
}

TEST(MacroTest, PasteThroughAnExtraIndirectionExpandsFirst) {
  // The canonical `CONCAT` pair: the inner macro's parameters are not `##`
  // operands, so the arguments are expanded before the outer paste.
  const PPOutcome out = PPFixture()
                            .source("#define CAT(a, b) a##b\n#define CAT_EXPANDED(a, b) "
                                    "CAT(a, b)\n#define my 1\nCAT_EXPANDED(my, Name)\n")
                            .run();
  EXPECT_EQ(out.concat(), "1Name");
}

TEST(MacroTest, PasteThatDoesNotMakeOneTokenIsDiagnosed) {
  // The standard calls this undefined; a language that targets production
  // reports the thing the user can fix. Both operands stay in the output.
  const PPOutcome out = PPFixture().source("#define C(a, b) a##b\nC(+, *)\n").run();
  EXPECT_TRUE(out.hasError("pp-invalid-paste"));
  EXPECT_EQ(out.concat(), "+*");
}

TEST(MacroTest, HashWithoutAParameterIsDiagnosedAtTheDefinition) {
  const PPOutcome out = PPFixture().source("#define H(x) # y\nH(1)\n").run();
  EXPECT_TRUE(out.hasError("pp-invalid-hash-operand"));
}

TEST(MacroTest, PasteAtEitherEndOfTheBodyIsDiagnosed) {
  EXPECT_TRUE(PPFixture().source("#define P(x) x ##\nP(1)\n").run().hasError("pp-invalid-paste"));
  EXPECT_TRUE(PPFixture().source("#define Q(x) ## x\nQ(1)\n").run().hasError("pp-invalid-paste"));
}

// --- variadic ---------------------------------------------------------------

TEST(MacroTest, VariadicCollectsTheTailIncludingCommas) {
  const PPOutcome out =
      PPFixture().source("#define F(a, ...) [a](__VA_ARGS__)\nF(1, 2, 3)\n").run();
  EXPECT_EQ(out.concat(), "[1](2,3)");
}

TEST(MacroTest, VariadicWithNoArgumentIsAnEmptyTail) {
  const PPOutcome out = PPFixture().source("#define F(a, ...) [a](__VA_ARGS__)\nF(1)\n").run();
  EXPECT_EQ(out.concat(), "[1]()");
  EXPECT_TRUE(out.errors.empty());
}

TEST(MacroTest, VariadicOnlyMacroAcceptsOneEmptyArgument) {
  const PPOutcome out = PPFixture().source("#define F(...) (__VA_ARGS__)\nF()\n").run();
  EXPECT_EQ(out.concat(), "()");
  EXPECT_TRUE(out.errors.empty());
}

TEST(MacroTest, VaOptExpandsItsContentOnlyWhenTheTailIsNonEmpty) {
  const PPOutcome empty = PPFixture().source("#define O(...) (0 __VA_OPT__(+ x))\nO()\n").run();
  EXPECT_EQ(empty.concat(), "(0)");
  const PPOutcome one = PPFixture().source("#define O(...) (0 __VA_OPT__(+ x))\nO(1)\n").run();
  EXPECT_EQ(one.concat(), "(0+x)");
}

TEST(MacroTest, VaOptContentIsSubstitutedNormally) {
  const PPOutcome out =
      PPFixture().source("#define O(...) (0 __VA_OPT__(+ (__VA_ARGS__)))\nO(5 + 1)\n").run();
  EXPECT_EQ(out.concat(), "(0+(5+1))");
}

TEST(MacroTest, VaOptOutsideAVariadicMacroIsDiagnosed) {
  const PPOutcome out = PPFixture().source("#define O(x) (x __VA_OPT__(y))\nO(1)\n").run();
  EXPECT_TRUE(out.hasError("pp-invalid-directive"));
}

// --- definitions ------------------------------------------------------------

TEST(MacroTest, IdenticalRedefinitionIsSilent) {
  const PPOutcome out = PPFixture().source("#define A 1\n#define A 1\nA\n").run();
  EXPECT_EQ(out.concat(), "1");
  EXPECT_TRUE(out.errors.empty());
}

TEST(MacroTest, DifferentRedefinitionIsDiagnosedAndTheFirstWins) {
  const PPOutcome out = PPFixture().source("#define A 1\n#define A 2\nA\n").run();
  EXPECT_TRUE(out.hasError("pp-macro-redefined"));
  EXPECT_EQ(out.concat(), "1");
}

TEST(MacroTest, UndefThenDefineIsAllowed) {
  const PPOutcome out = PPFixture().source("#define A 1\n#undef A\n#define A 2\nA\n").run();
  EXPECT_EQ(out.concat(), "2");
  EXPECT_TRUE(out.errors.empty());
}

TEST(MacroTest, UndefOfAnUnknownNameIsSilent) {
  const PPOutcome out = PPFixture().source("#undef NEVER_DEFINED\n").run();
  EXPECT_TRUE(out.errors.empty());
}

TEST(MacroTest, MissingNameIsDiagnosed) {
  EXPECT_TRUE(PPFixture().source("#define\n").run().hasError("pp-missing-macro-name"));
  EXPECT_TRUE(PPFixture().source("#undef\n").run().hasError("pp-missing-macro-name"));
}

TEST(MacroTest, DuplicateParameterIsDiagnosed) {
  EXPECT_TRUE(PPFixture().source("#define F(a, a) a\n").run().hasError("pp-invalid-directive"));
}

TEST(MacroTest, ParameterLimitIsEnforced) {
  std::string text = "#define BIG(";
  for (int i = 0; i < 300; ++i) {
    if (i != 0) {
      text += ", ";
    }
    text += "p" + std::to_string(i);
  }
  text += ") p0\n";
  EXPECT_TRUE(PPFixture().source(text).run().hasError("pp-macro-parameter-limit"));
}

// --- invocation errors ------------------------------------------------------

TEST(MacroTest, TooFewArgumentsIsDiagnosed) {
  EXPECT_TRUE(PPFixture()
                  .source("#define F(a, b) a b\nF(1)\n")
                  .run()
                  .hasError("pp-missing-macro-arguments"));
}

TEST(MacroTest, TooManyArgumentsIsDiagnosed) {
  EXPECT_TRUE(PPFixture()
                  .source("#define F(a) a\nF(1, 2)\n")
                  .run()
                  .hasError("pp-too-many-macro-arguments"));
}

TEST(MacroTest, UnterminatedArgumentsAreDiagnosed) {
  EXPECT_TRUE(PPFixture()
                  .source("#define F(a) a\nF(1\n")
                  .run()
                  .hasError("pp-unterminated-macro-arguments"));
}

// --- budgets ----------------------------------------------------------------

TEST(MacroTest, ExpansionDepthIsBounded) {
  // A chain of 300 macros nests 300 contexts deep. The cap is reported once,
  // with a named diagnostic, instead of the run continuing to unbounded depth.
  std::string text;
  for (int i = 0; i < 300; ++i) {
    text += "#define M" + std::to_string(i) + " " +
            (i == 0 ? std::string("1") : "M" + std::to_string(i - 1)) + "\n";
  }
  text += "M299\n";
  const PPOutcome out = PPFixture().source(text).run();
  EXPECT_TRUE(out.hasError("pp-expansion-depth"));
}

TEST(MacroTest, BluePaintTerminatesWithoutAnyBudget) {
  // The important case is that the standard's own rules terminate this: no
  // budget is hit, so a legitimate program is never refused.
  const PPOutcome out = PPFixture().source("#define A B B\n#define B A A\nA\n").run();
  EXPECT_TRUE(out.errors.empty());
}

// --- provenance -------------------------------------------------------------

TEST(MacroTest, ReplacementTokensPointAtTheDefinition) {
  // The whole reason provenance is a separate field: the emitted `100` was
  // written on line 1 (the definition), while it appears where `A` was used.
  const PPOutcome out = PPFixture().source("#define A 100\nA\n").run();
  ASSERT_EQ(out.spellings.size(), 1U);
  EXPECT_EQ(out.spellings[0], "100");
  EXPECT_EQ(out.locations[0], "test.mx:1:11");
}

TEST(MacroTest, ArgumentTokensPointAtTheInvocation) {
  const PPOutcome out = PPFixture().source("#define ID(x) x\n\nID(7)\n").run();
  ASSERT_EQ(out.spellings.size(), 1U);
  EXPECT_EQ(out.locations[0], "test.mx:3:4");
}

} // namespace
} // namespace minc::test
