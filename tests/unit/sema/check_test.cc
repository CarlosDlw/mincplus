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

TEST(CheckTest, ReachabilityIsExactWhileTheGrammarHasNoBranches) {
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

} // namespace
} // namespace minc::test
