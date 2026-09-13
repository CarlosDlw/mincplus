// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The checker on code that is right: what it infers, what it folds, what it
// records, and the properties the rest of the pipeline depends on.
#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "sema/sema_fixture.h"
#include "sema/type.h"
#include "sema/typed_ast.h"

namespace minc::test {
namespace {

TEST(CheckTest, ASimpleUnitChecksCleanly) {
  SemaFixture f;
  f.source("fn i32 add() { return 0; }\n"
           "fn i32 main() { let x: i32 = 1 + 2; return x; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
  EXPECT_EQ(f.warningCount(), 0u);
}

TEST(CheckTest, InferenceUsesTheInitializersTypeAndDefaultsALiteral) {
  {
    SemaFixture f;
    f.source("fn i32 main() { let x = 1; return x; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_EQ(f.errorCount(), 0u);
    // `1` is a deferred literal; with nothing to constrain it, it becomes `i32`.
    EXPECT_EQ(f.bindingType("x"), "i32");
  }
  {
    SemaFixture f;
    f.source("fn i32 main() { const y = 2.5; return 0; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_EQ(f.bindingType("y"), "f64");
  }
  {
    SemaFixture f;
    f.source("fn i32 main() { const z = 1 + 2 * 3; return 0; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_EQ(f.bindingType("z"), "i32");
  }
}

TEST(CheckTest, ADeclaredTypeDecidesADeferredLiteral) {
  SemaFixture f;
  f.source("fn i32 main() { let small: u8 = 255; let wide: i64 = 1; return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
  EXPECT_EQ(f.bindingType("small"), "u8");
  EXPECT_EQ(f.bindingType("wide"), "i64");
}

TEST(CheckTest, AnAnnotatedTypeSurvivesTheWholeExpression) {
  SemaFixture f;
  f.source("fn i32 main() { let x: i64 = 1 + 2; return 0; }\n");
  ASSERT_TRUE(f.build());
  // The literals adopt the context's type, so the sum is `i64` -- not an `i32`
  // that narrows at the last step.
  EXPECT_EQ(f.bindingType("x"), "i64");
}

TEST(CheckTest, CSpellingsAndPrimitiveNamesAreOneType) {
  SemaFixture f;
  f.source("fn i32 main() { let a: int = 1; let b: i32 = a; return b; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
  // One `TypeId`: that is the whole point of interning, and it is what will let a
  // header's `int` and a body's `i32` be compared as the same type.
  EXPECT_EQ(f.bindingType("a"), "i32");
  EXPECT_EQ(f.bindingType("b"), "i32");
}

TEST(CheckTest, TheTargetDecidesWhatLongMeans) {
  {
    SemaFixture f("test.mx", sema::Target::SystemVAmd64);
    f.source("fn i32 main() { let x: long = 1; return 0; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_EQ(f.bindingType("x"), "i64");
  }
  {
    SemaFixture f("test.mx", sema::Target::WindowsX64);
    f.source("fn i32 main() { let x: long = 1; return 0; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_EQ(f.bindingType("x"), "i32");
  }
}

TEST(CheckTest, ConstantsFoldAndCarryTheirValue) {
  SemaFixture f;
  f.source("fn i32 main() { const c = 2 * 3 + 1; return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
  // The collected value is what makes `const z = 0; 1 / z` a diagnosed division
  // by zero rather than an undefined behaviour inherited by the IR.
  const sema::ExprInfo info = f.initializerInfo("c");
  EXPECT_TRUE(info.isConstant);
  EXPECT_TRUE(info.hasIntValue);
  if (info.hasIntValue) {
    EXPECT_EQ(info.value.signedValue(), 7);
  }
}

TEST(CheckTest, AConstantConditionPicksItsArm) {
  SemaFixture f;
  f.source("fn i32 main() { const c = true ? 1 : 2; return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
  const sema::ExprInfo info = f.initializerInfo("c");
  EXPECT_TRUE(info.hasIntValue);
  if (info.hasIntValue) {
    EXPECT_EQ(info.value.signedValue(), 1);
  }
}

TEST(CheckTest, LvaluesAreRecordedAndNotForATemporary) {
  SemaFixture f;
  f.source("fn i32 main() { let x = 1; x = x + 1; let y = x + 1; return y; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
  // `x` alone is a place; `x + 1` is not. The distinction is what `=` and `++`
  // read, and it is computed once per node.
  const sema::ExprInfo pathInfo = f.initializerInfo("y");
  EXPECT_FALSE(pathInfo.isLvalue);
}

TEST(CheckTest, AVoidFunctionIsCheckedAndItsCallIsNotAValue) {
  {
    SemaFixture f;
    f.source("fn void log() { return; }\nfn i32 main() { log(); return 0; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_EQ(f.errorCount(), 0u);
  }
  {
    SemaFixture f;
    f.source("fn void log() { return; }\nfn i32 main() { return log(); }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-return-mismatch"));
  }
}

TEST(CheckTest, ACallTakesTheFunctionsReturnType) {
  SemaFixture f;
  f.source("fn i64 wide() { return 1; }\nfn i32 main() { let x: i64 = wide(); return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
  EXPECT_EQ(f.bindingType("x"), "i64");
}

TEST(CheckTest, AFunctionWrittenLaterIsAlreadyTyped) {
  SemaFixture f;
  f.source("fn i32 main() { return helper(); }\nfn i32 helper() { return 1; }\n");
  ASSERT_TRUE(f.build());
  // Signatures are collected before any body is checked, which is what makes a
  // forward reference ordinary rather than a special case.
  EXPECT_EQ(f.errorCount(), 0u);
}

TEST(CheckTest, MainIsCheckedForItsSignature) {
  {
    SemaFixture f;
    f.source("fn i32 main() { return 0; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_FALSE(f.hasError("sema-main-signature"));
  }
  {
    SemaFixture f;
    f.source("fn void main() { return; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-main-signature"));
  }
}

TEST(CheckTest, AUnitWithNoMainIsNotThisStagesProblem) {
  SemaFixture f;
  f.source("fn i32 helper() { return 0; }\n");
  ASSERT_TRUE(f.build());
  // The entry point is a property of the *program*, and this stage sees one
  // translation unit. `link` owns that question.
  EXPECT_EQ(f.errorCount(), 0u);
}

// The baseline the branch-aware cases build on: a body is terminating when its
// last statement returns, and a nested block is no different. The `if`, the
// `else` and the loops are the tests that follow, and each of them has to agree
// with this rule rather than replace it.
TEST(CheckTest, StraightLineReachabilityIsExact) {
  {
    SemaFixture f;
    f.source("fn i32 f() { return 1; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_FALSE(f.hasError("sema-missing-return"));
  }
  {
    SemaFixture f;
    f.source("fn i32 f() { let x = 1; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-missing-return"));
  }
  {
    SemaFixture f;
    f.source("fn i32 f() { { return 1; } }\n");
    ASSERT_TRUE(f.build());
    // A block whose last statement returns is terminating, so no diagnostic.
    EXPECT_FALSE(f.hasError("sema-missing-return"));
  }
}

TEST(CheckTest, UnreachableCodeIsAWarningAndNotAnError) {
  SemaFixture f;
  f.source("fn i32 f() { return 1; let x = 2; return x; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
  EXPECT_TRUE(f.hasWarning("sema-unreachable-code"));
  // One warning for the region, not one per following statement.
  EXPECT_EQ(f.warningCount(), 1u);
}

TEST(CheckTest, NarrowingIsAWarningOnlyWhenAsked) {
  const std::string_view source =
      "fn i32 main() { let wide: i32 = 1; let narrow: i8 = wide; return 0; }\n";
  {
    SemaFixture quiet;
    quiet.source(std::string(source));
    ASSERT_TRUE(quiet.build());
    EXPECT_EQ(quiet.errorCount(), 0u);
    EXPECT_EQ(quiet.warningCount(), 0u);
  }
  {
    SemaFixture loud;
    loud.source(std::string(source)).warnConversion();
    ASSERT_TRUE(loud.build());
    EXPECT_EQ(loud.errorCount(), 0u);
    EXPECT_TRUE(loud.hasWarning("sema-implicit-conversion"));
  }
}

TEST(CheckTest, TheArtifactIsTotalOnExpressions) {
  SemaFixture f;
  f.source("fn i32 main() { let x = 1; return x; }\n");
  ASSERT_TRUE(f.build());

  // Every node has an answer: a real type or the poison, never "not asked".
  for (std::uint32_t i = 0; i < f.lowered().nodeCount(); ++i) {
    const sema::TypeId type = f.typed().typeOf(minc::ast::AstId{i});
    EXPECT_TRUE(type.valid()) << "node " << i << " has no type";
    // And the query is total for an id from another unit, too.
  }
  EXPECT_TRUE(f.typed().typeOf(minc::ast::AstId{999999}).valid());
}

TEST(CheckTest, TheFunctionTableHoldsWhatTheVerifierWouldAsk) {
  SemaFixture f;
  f.source("fn i64 compute() { return 1; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_EQ(f.typed().functionTable.size(), 1u);
  const sema::FunctionInfo& info = f.typed().functionTable.front();
  EXPECT_EQ(info.returnType, sema::kTypeI64);
  EXPECT_TRUE(info.body.valid());
  EXPECT_EQ(f.types().spelling(info.returnType), "i64");
  EXPECT_EQ(f.types().spelling(sema::functionReturnType(f.typed(), info.decl)), "i64");
}

TEST(CheckTest, AskingTwiceGivesTheSameAnswer) {
  SemaFixture f;
  f.source("fn i32 main() { let x: u8 = 7; return 0; }\n");
  ASSERT_TRUE(f.build());

  const std::string first = f.dump();
  const std::string second = f.dump();
  // A dump is a function of the input: same bytes, same order, no addresses and
  // no hash-order iteration.
  EXPECT_EQ(first, second);
  EXPECT_NE(first.find("u8 [const] =7"), std::string::npos) << first;
}

// --- control flow ------------------------------------------------------------

TEST(CheckTest, ConditionsMustBeBoolInEveryConstruct) {
  // One rule, one owner: `if`, `while` and `for` disagree about nothing here.
  for (const std::string source :
       {"fn i32 f() { if 1 { } return 0; }\n", "fn i32 f() { while 1 { } return 0; }\n",
        "fn i32 f() { for ; 1; { } return 0; }\n"}) {
    SemaFixture f;
    f.source(source);
    ASSERT_TRUE(f.build()) << source;
    EXPECT_TRUE(f.hasError("sema-condition-not-bool")) << source;
  }
}

TEST(CheckTest, ABoolConditionIsAccepted) {
  SemaFixture f;
  f.source("fn i32 f(a: i32)\n{\n  if a == 1 { return 1; }\n  while a > 0 { a = a - 1; }\n  for ; "
           "a < 3; a = a + 1 { }\n  return a;\n}\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u) << f.firstError().message;
}

TEST(CheckTest, AnIfWithBothArmsReturningSatisfiesTheReturnCheck) {
  SemaFixture f;
  f.source("fn i32 f(a: bool) { if a { return 1; } else { return 2; } }\n");
  ASSERT_TRUE(f.build());
  EXPECT_FALSE(f.hasError("sema-missing-return"));
}

TEST(CheckTest, AnIfWithOneArmReturningDoesNot) {
  // The false branch falls through, so the function can reach its end without a
  // value. Accepting this would be accepting a missing `return`.
  SemaFixture f;
  f.source("fn i32 f(a: bool) { if a { return 1; } }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-missing-return"));
}

TEST(CheckTest, AnElseIfChainCountsAsBothArms) {
  SemaFixture f;
  f.source("fn i32 f(a: i32) { if a == 1 { return 1; } else if a == 2 { return 2; } else { return "
           "3; } }\n");
  ASSERT_TRUE(f.build());
  EXPECT_FALSE(f.hasError("sema-missing-return"));
}

TEST(CheckTest, ALoopThatCannotLeaveIsTerminating) {
  // `while true` with no `break` never falls through, so the function never
  // reaches its end and nothing is missing.
  SemaFixture f;
  f.source("fn i32 f() { while true { let x = 1; x = x + 1; } }\n");
  ASSERT_TRUE(f.build());
  EXPECT_FALSE(f.hasError("sema-missing-return"));
}

TEST(CheckTest, ABreakMakesTheLoopAbleToLeave) {
  SemaFixture f;
  f.source("fn i32 f() { while true { break; } }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-missing-return"));
}

TEST(CheckTest, ABreakAnywhereInTheBodyCounts) {
  // Buried in an `if` inside the loop's `else`: the scan has to find it, or this
  // function would be accepted with a path that returns nothing.
  SemaFixture f;
  f.source("fn i32 f(a: bool) { while true { if a { } else { break; } } }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-missing-return"));
}

TEST(CheckTest, ABreakInANestedLoopBelongsToThatLoop) {
  // The inner `break` leaves the inner loop, not the outer one, so the outer
  // loop still cannot fall through. Counting it would be a false "missing
  // return" on correct code.
  SemaFixture f;
  f.source("fn i32 f() { while true { while true { break; } } }\n");
  ASSERT_TRUE(f.build());
  EXPECT_FALSE(f.hasError("sema-missing-return"));
}

TEST(CheckTest, AnEmptyForConditionMeansForever) {
  SemaFixture f;
  f.source("fn i32 f() { for ;; { let x = 1; } }\n");
  ASSERT_TRUE(f.build());
  EXPECT_FALSE(f.hasError("sema-missing-return"));
}

TEST(CheckTest, AJumpOutsideALoopIsAnError) {
  {
    SemaFixture f;
    f.source("fn i32 f() { break; return 0; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-break-outside-loop"));
  }
  {
    SemaFixture f;
    f.source("fn i32 f() { continue; return 0; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-continue-outside-loop"));
  }
}

TEST(CheckTest, AJumpInsideAnIfInsideALoopIsFine) {
  SemaFixture f;
  f.source("fn i32 f(a: i32) { for let i = 0; i < a; i = i + 1 { if a == i { continue; } break; } "
           "return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u) << f.firstError().message;
}

TEST(CheckTest, AStatementAfterAnInfiniteLoopIsUnreachable) {
  SemaFixture f;
  f.source("fn i32 f() { while true { } return 1; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasWarning("sema-unreachable-code"));
}

// --- parameters --------------------------------------------------------------

TEST(CheckTest, AParameterIsATypedMutableBinding) {
  SemaFixture f;
  f.source("fn i32 f(a: i32)\n{\n  a = a + 1;\n  return a;\n}\n");
  ASSERT_TRUE(f.build());
  // Assignable: a parameter is not a `const`. A function that could not assign
  // to its own parameter would have to copy it first.
  EXPECT_EQ(f.errorCount(), 0u) << f.firstError().message;
}

TEST(CheckTest, AParameterUsesTheTypeItsDeclarationGaveIt) {
  SemaFixture f;
  f.source("fn i32 f(a: i32)\n{\n  return a;\n}\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u) << f.firstError().message;
}

TEST(CheckTest, ArgumentsAreCheckedAgainstTheParameterTypes) {
  const std::string_view declares = "fn i32 f(a: i32) { return a; }\n";
  {
    SemaFixture f;
    f.source(std::string(declares) + "fn i32 main() { return f(1); }\n");
    ASSERT_TRUE(f.build());
    EXPECT_EQ(f.errorCount(), 0u) << f.firstError().message;
  }
  {
    // A literal adapts to the parameter's type, so this is the count that is
    // wrong, not the value.
    SemaFixture f;
    f.source(std::string(declares) + "fn i32 main() { return f(); }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-argument-count"));
  }
  {
    // `str` does not convert to `i32`.
    SemaFixture f;
    f.source(std::string(declares) + "fn i32 main() { return f(\"x\"); }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-invalid-assignment"));
  }
}

TEST(CheckTest, AParameterTypeIsUsedInTheSignature) {
  // Calling through the wrong shim is what a mis-read parameter type would look
  // like from the outside: the argument no longer fits.
  SemaFixture f;
  f.source("fn i32 f(unsigned long long int x) { return 0; }\n"
           "fn i32 main() { return f(1); }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u) << f.firstError().message;
  EXPECT_FALSE(f.hasError("sema-unknown-type"));
}

TEST(CheckTest, AVoidParameterIsRejected) {
  SemaFixture f;
  f.source("fn i32 f(x: void) { return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-type-not-value"));
}

TEST(CheckTest, AParameterShadowsNothingAndAnUnknownTypeIsNamed) {
  SemaFixture f;
  f.source("fn i32 f(a: i33) { return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-unknown-type"));
}

TEST(CheckTest, ParametersOfEveryArityWork) {
  SemaFixture f;
  f.source("fn i32 zero() { return 0; }\n"
           "fn i32 one(a: i32) { return a; }\n"
           "fn i32 two(a: i32, b: i32) { return a + b; }\n"
           "fn i32 main() { return zero() + one(1) + two(2, 3); }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u) << f.firstError().message;
}

TEST(CheckTest, APathologicalNestingIsADiagnosticAndNotAStackOverflow) {
  // A right-leaning chain: `1 + (1 + (1 + ...))`. The bound is checked *before*
  // the descent, so a deep input is a diagnostic.
  std::string source = "fn i32 main() { let x = 1";
  for (int i = 0; i < 5000; ++i) {
    source += " + (1";
  }
  for (int i = 0; i < 5000; ++i) {
    source += ")";
  }
  source += "; return 0; }\n";

  SemaFixture f;
  f.source(std::move(source));
  ASSERT_TRUE(f.build());
  // The parser's own depth guard may reject it first, in which case there is
  // nothing for the checker to say; either way it must not crash.
  if (!f.hasParseError() && f.hasAstError() == false) {
    EXPECT_TRUE(f.hasError("sema-limit-types") || f.errorCount() > 0);
  }
}

// A declaration a macro wrote is a declaration like any other: it has its own
// identity, so a use of it gets its own type. The hazard is not the spelling but
// the *location* -- a macro that expands one argument into two names gives both
// declarations the same written range -- and its symptom was silent: the pasted
// name's type was never recorded, and a use of it was the poison with no
// diagnostic at all. This is the end-to-end half of the case
// `ScopeTest.TwoNamesFromOneMacroArgumentAreTwoDeclarations` pins below it.
TEST(CheckTest, ANameWrittenByAPasteGetsItsOwnDeclarationAndType) {
  SemaFixture f;
  f.source("#define CONCAT(a, b) a ## b\n"
           "#define PAIR(b) let b: i32 = 1; let CONCAT(b, 2): i32 = 2;\n"
           "fn i32 f()\n"
           "{\n"
           "  PAIR(a)\n"
           "  return a + a2;\n"
           "}\n"
           "fn i32 main() { return f(); }\n");
  ASSERT_TRUE(f.build());
  // The lazy arm: `firstError()` is only read when there is one.
  EXPECT_EQ(f.errorCount(), 0u) << (f.errorCount() == 0 ? std::string() : f.firstError().message);
  EXPECT_EQ(f.typeOfSpelling("a2"), "i32");
  EXPECT_EQ(f.typeOfSpelling("a"), "i32");
}

} // namespace
} // namespace minc::test
