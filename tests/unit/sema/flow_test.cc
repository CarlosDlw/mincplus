// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Definite assignment: the one way this language could read an object that was
// never given a value.
//
// Two properties matter and neither is visible from the source text alone: that
// every path that *reaches* a read has assigned the binding, and that the
// analysis does not fire on a program that assigns on all of them. The second is
// why most of what follows is a case that must be **accepted**: a check that
// rejects working code is worse than no check, and the merge rules are exactly
// where an implementation gets that wrong.
#include <gtest/gtest.h>

#include <string>

#include "sema/sema_fixture.h"

namespace minc::test {
namespace {

TEST(FlowTest, AReadWithNoAssignmentIsAnError) {
  SemaFixture f;
  f.source("fn i32 f() { let x: i32; return x; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-use-before-assignment"));
  EXPECT_EQ(f.errorCount(), 1u);
  // The sentence names the binding, because the reader has to find it.
  EXPECT_NE(f.firstError().message.find("`x`"), std::string::npos);
}

TEST(FlowTest, AssignmentBeforeTheReadIsAccepted) {
  SemaFixture f;
  f.source("fn i32 f() { let x: i32; x = 1; return x; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
}

TEST(FlowTest, ADeclaredAndInitializedBindingIsNeverInQuestion) {
  SemaFixture f;
  f.source("fn i32 f() { let a: i32 = 1; const b = 2; return a + b; }\n"
           "fn i32 main() { return f(); }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
}

TEST(FlowTest, AParameterIsAssignedWhenTheBodyStarts) {
  SemaFixture f;
  f.source("fn i32 f(a: i32, b: i32) { return a * b; }\nfn i32 main() { return f(1, 2); }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
}

// The reading order matters inside an expression: a store is not a read, and
// the value it stores is read first.
TEST(FlowTest, TheTargetOfAPlainAssignmentIsNotARead) {
  SemaFixture f;
  f.source("fn i32 f() { let x: i32; x = 1; return x; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
}

TEST(FlowTest, TheRightHandSideOfAnAssignmentIsARead) {
  SemaFixture f;
  f.source("fn i32 f() { let x: i32; x = x + 1; return x; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-use-before-assignment"));
}

TEST(FlowTest, AStoreIntoItselfIsARead) {
  SemaFixture f;
  f.source("fn i32 f() { let x: i32; x = x; return x; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-use-before-assignment"));
}

TEST(FlowTest, ACompoundAssignmentReadsItsTarget) {
  for (const char* op : {"+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="}) {
    SemaFixture f;
    f.source(std::string("fn i32 f() { let x: i32; x ") + op + " 1; return x; }\n");
    ASSERT_TRUE(f.build()) << op;
    EXPECT_TRUE(f.hasError("sema-use-before-assignment")) << op;
  }
}

TEST(FlowTest, IncrementReadsBeforeItWrites) {
  {
    SemaFixture f;
    f.source("fn i32 f() { let x: i32; x++; return x; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-use-before-assignment"));
  }
  {
    SemaFixture f;
    f.source("fn i32 f() { let x: i32; ++x; return x; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-use-before-assignment"));
  }
}

// One mistake, one diagnostic: the fix is one assignment, so five reads are one
// sentence rather than five.
TEST(FlowTest, ABindingIsReportedOnceHoweverManyTimesItIsRead) {
  SemaFixture f;
  f.source("fn i32 f() { let x: i32; return x + x + (x * x); }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 1u) << f.firstError().message;
}

TEST(FlowTest, ReadingAnAssignedBindingInALoopBodyIsFine) {
  SemaFixture f;
  f.source("fn i32 f() { let x: i32 = 0; while x < 10 { x = x + 1; } return x; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
}

// --- the merges --------------------------------------------------------------

TEST(FlowTest, BothArmsOfAnElseAreEnough) {
  SemaFixture f;
  f.source("fn i32 f(c: bool) { let x: i32; if c { x = 1; } else { x = 2; } return x; }\n"
           "fn i32 main() { return f(true); }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
}

TEST(FlowTest, OneArmOfAnElseIsNotEnough) {
  SemaFixture f;
  f.source("fn i32 f(c: bool) { let x: i32; if c { x = 1; } else { } return x; }\n"
           "fn i32 main() { return f(true); }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-use-before-assignment"));
}

TEST(FlowTest, AnIfWithNoElseAddsNothing) {
  SemaFixture f;
  f.source("fn i32 f(c: bool) { let x: i32; if c { x = 1; } return x; }\n"
           "fn i32 main() { return f(true); }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-use-before-assignment"));
}

TEST(FlowTest, EveryArmOfAnElseIfChainIsNeeded) {
  {
    // All three paths assign: accepted.
    SemaFixture f;
    f.source("fn i32 f(a: i32) { let x: i32; if a < 0 { x = 1; } else if a == 0 { x = 2; } else "
             "{ x = 3; } return x; }\n"
             "fn i32 main() { return f(0); }\n");
    ASSERT_TRUE(f.build());
    EXPECT_EQ(f.errorCount(), 0u);
  }
  {
    // The last path does not: refused, and the chain is what the merge walks.
    SemaFixture f;
    f.source("fn i32 f(a: i32) { let x: i32; if a < 0 { x = 1; } else if a == 0 { x = 2; } else "
             "{ } return x; }\n"
             "fn i32 main() { return f(0); }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-use-before-assignment"));
  }
}

TEST(FlowTest, ABindingAssignedInsideAnArmIsUsableInsideThatArm) {
  SemaFixture f;
  f.source("fn i32 f(c: bool) { let x: i32; if c { x = 1; return x; } return 0; }\n"
           "fn i32 main() { return f(true); }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
}

// A loop may run zero times, so an assignment in the body cannot be assumed
// after it -- the conservative rule, and the one the specs take.
TEST(FlowTest, AnAssignmentInsideALoopBodyDoesNotSurviveTheLoop) {
  SemaFixture f;
  f.source("fn i32 f(c: bool) { let x: i32; while c { x = 1; } return x; }\n"
           "fn i32 main() { return f(true); }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-use-before-assignment"));
}

TEST(FlowTest, ALoopWhoseBodyAssignsIsStillFineInsideTheBody) {
  SemaFixture f;
  f.source(
      "fn i32 f(c: bool) { let x: i32; let total: i32 = 0; while c { x = 1; total = total + x; "
      "return total; } return 0; }\n"
      "fn i32 main() { return f(true); }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
}

// `while true` cannot be left by falling out of the bottom, so the only paths
// that reach the code after it are its `break`s -- which is what makes this
// accepted, and is the rule that separates a useful check from an annoying one.
TEST(FlowTest, ALoopThatCannotLeaveIsLeftOnlyByItsBreaks) {
  SemaFixture f;
  f.source("fn i32 f() { let x: i32; while true { x = 1; break; } return x; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
}

TEST(FlowTest, ABreakBeforeTheAssignmentIsNotEnough) {
  SemaFixture f;
  f.source("fn i32 f() { let x: i32; while true { if false { break; } x = 1; } return x; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-use-before-assignment"));
}

TEST(FlowTest, ALoopWithNoExitMakesWhatFollowsUnreachableAndUnchecked) {
  SemaFixture f;
  f.source("fn i32 f() { let x: i32; while true { } return x; }\n");
  ASSERT_TRUE(f.build());
  // The `return` is unreachable, so reading an unwritten object there is not a
  // finding -- and the unreachable warning already says the statement is dead.
  EXPECT_EQ(f.errorCount(), 0u);
  EXPECT_TRUE(f.hasWarning("sema-unreachable-code"));
}

// A `break` belongs to the loop it is written in, and a stack is what decides
// that without a second scan of the body.
TEST(FlowTest, ABreakInANestedLoopBelongsToTheNestedLoop) {
  SemaFixture f;
  f.source("fn i32 f() { let x: i32; while true { while true { break; } x = 1; break; } return x; "
           "}\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
}

TEST(FlowTest, ContinueIsNotAnExitFromTheLoop) {
  SemaFixture f;
  f.source("fn i32 f(c: bool) { let x: i32; while c { if c { continue; } x = 1; } return x; }\n"
           "fn i32 main() { return f(true); }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-use-before-assignment"));
}

// --- the `for` and the short-circuit operators ---------------------------------

TEST(FlowTest, AForInitializerCountsAsAnAssignment) {
  SemaFixture f;
  f.source("fn i32 f(n: i32) { let total: i32; for let i: i32 = 0; i < n; i += 1 { total = i; } "
           "return 0; }\n"
           "fn i32 main() { return f(3); }\n");
  ASSERT_TRUE(f.build());
  // `total` is assigned on every path the read is on, and the `for` binding is
  // scoped to the loop -- resolution, not this pass, owns that.
  EXPECT_EQ(f.errorCount(), 0u);
}

TEST(FlowTest, AForWhoseStepReadsAnUnassignedBindingIsRefused) {
  SemaFixture f;
  f.source("fn i32 f(n: i32) { let x: i32; for let i: i32 = 0; i < n; i += x { } return 0; }\n"
           "fn i32 main() { return f(3); }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-use-before-assignment"));
}

TEST(FlowTest, TheRightOfAShortCircuitMayNotRunSoItProvesNothing) {
  SemaFixture f;
  f.source("fn i32 f(c: bool) { let x: i32; let ok: bool = c && (x = 1) == 1; return x; }\n"
           "fn i32 main() { return f(true); }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-use-before-assignment"));
}

TEST(FlowTest, OnlyOneArmOfAConditionalRunsSoBothMustAssign) {
  {
    SemaFixture f;
    f.source("fn i32 f(c: bool) { let x: i32; let y: i32 = c ? (x = 1) : (x = 2); return x; }\n"
             "fn i32 main() { return f(true); }\n");
    ASSERT_TRUE(f.build());
    EXPECT_EQ(f.errorCount(), 0u);
  }
  {
    SemaFixture f;
    f.source("fn i32 f(c: bool) { let x: i32; let y: i32 = c ? (x = 1) : 0; return x; }\n"
             "fn i32 main() { return f(true); }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-use-before-assignment"));
  }
}

// --- scopes ------------------------------------------------------------------

TEST(FlowTest, AShadowingBindingHasItsOwnAnswer) {
  SemaFixture f;
  f.source("fn i32 f() { let x: i32; { let x: i32 = 1; return x; } return 0; }\n"
           "fn i32 main() { return f(); }\n");
  ASSERT_TRUE(f.build());
  // The outer `x` is never read, so its being unassigned is not a finding.
  EXPECT_EQ(f.errorCount(), 0u);
}

TEST(FlowTest, AssigningTheOuterBindingThroughAnInnerScopeCounts) {
  SemaFixture f;
  f.source("fn i32 f() { let x: i32; { x = 1; } return x; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
}

TEST(FlowTest, ADifferentFunctionIsADifferentAnswer) {
  SemaFixture f;
  f.source("fn i32 f() { let x: i32; x = 1; return x; }\n"
           "fn i32 g() { let x: i32; x = 2; return x; }\n"
           "fn i32 main() { return f() + g(); }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
}

} // namespace
} // namespace minc::test
