// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// `!`, the bottom type: the promise, the proof, and what the type does for free.
//
// Two things are under test and they are different. One is the *proof*: a
// function that returns `!` says control never comes back, and a body that could
// come back is refused -- that is the only part of this feature a type system
// cannot check by itself. The other is everything the *type* buys without a
// single extra rule: a call to a `!` function is `!`, so the code after it is
// unreachable, a `?:` with a `!` arm takes the other arm's type, and the operand
// of a `return` in a `void` function may be one.
#include <gtest/gtest.h>

#include <cstddef>
#include <string>

#include "sema/sema_fixture.h"

namespace minc::test {
namespace {

// The declaration every test below needs, spelled once: `!` in the return-type
// position, `extern` because the body is somewhere this compiler cannot see.
constexpr const char* kExit = "extern fn ! exit(code: i32);\n";

TEST(NeverTest, ACallEndsTheStatementSoTheBodyCannotFallOffItsEnd) {
  // The false positive the feature removes: `terminates` used to look only at the
  // *shape* of a statement, so a call was never a way out and a function ending
  // in `exit(1)` was told it could reach the end. That is a program the checker
  // refused and the language has to compile.
  SemaFixture f;
  f.source(std::string(kExit) + "fn i32 f() {\n  exit(1);\n}\n");
  ASSERT_TRUE(f.build());

  EXPECT_FALSE(f.hasError("sema-missing-return")) << f.dump();
  EXPECT_EQ(f.errorCount(), 0u);
}

TEST(NeverTest, WhatFollowsANeverStatementIsUnreachable) {
  // The same fact, seen from the other side: `checkBlock` marks what follows a
  // statement that never completes, and a call to a `!` function is now one. Both
  // shapes of statement get it -- an expression statement and a binding whose
  // initializer never produces a value.
  {
    SemaFixture f;
    f.source(std::string(kExit) + "fn void f() {\n  exit(1);\n  exit(2);\n}\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasWarning("sema-unreachable-code")) << f.dump();
    EXPECT_EQ(f.warningCount(), 1u) << f.dump();
  }
  {
    SemaFixture f;
    f.source(std::string(kExit) + "fn void f() {\n  let x: i32 = exit(1);\n  exit(2);\n}\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasWarning("sema-unreachable-code")) << f.dump();
    EXPECT_EQ(f.warningCount(), 1u) << f.dump();
  }
}

TEST(NeverTest, AConditionalWithANeverArmTakesTheOtherArmsType) {
  // This is the part an annotation beside the signature could not do at all. `!`
  // is a *type*, so the conditional asks the ordinary question -- what do the two
  // arms have in common -- and the answer is the arm that can actually produce a
  // value. The `!` arm converts into it, so nothing about the conditional has to
  // know the bottom type exists.
  SemaFixture f;
  f.source(std::string(kExit) + "fn i32 pick(c: bool) {\n"
                                "  let v = c ? 1 : exit(2);\n"
                                "  return v;\n"
                                "}\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u) << f.dump();
  EXPECT_EQ(f.bindingType("v"), "i32") << f.dump();
}

TEST(NeverTest, BothArmsOfANeverConditionalAreNever) {
  // Both arms `!` is the same rule rather than a second one: a conditional whose
  // every path refuses to produce a value does not produce one, and the whole
  // expression is the bottom type -- which is what makes the enclosing statement
  // divergent too.
  SemaFixture f;
  f.source(std::string(kExit) + "fn i32 main(c: bool) {\n"
                                "  c ? exit(1) : exit(2);\n"
                                "}\n");
  ASSERT_TRUE(f.build());
  EXPECT_FALSE(f.hasError("sema-missing-return")) << f.dump();
}

TEST(NeverTest, APromiseNeedsSomethingThatKeepsIt) {
  // The proof, and the two ways a body can break it. The sentences differ
  // because the fixes do: a stray `return` moves or goes away, while a body that
  // simply runs out is missing the loop or the call it meant to end with.
  {
    SemaFixture f;
    f.source("fn ! f() {\n  return;\n}\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-never-returns")) << f.dump();
    EXPECT_EQ(f.errorCount(), 1u) << f.dump();
  }
  {
    SemaFixture f;
    f.source("fn ! f() {\n}\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-never-body-completes")) << f.dump();
    EXPECT_EQ(f.errorCount(), 1u) << f.dump();
  }
}

TEST(NeverTest, EachWayOfKeepingThePromiseIsAccepted) {
  const char* const kBodies[] = {
      "  while true {\n  }\n",
      "  exit(1);\n",
      "  if (c) {\n    exit(0);\n  } else {\n    exit(1);\n  }\n",
      "  if (c) {\n    while true {\n    }\n  } else {\n    exit(1);\n  }\n",
      // A `return` whose operand never produces a value is not a return: control
      // never gets as far as handing one back.
      "  return exit(3);\n",
  };
  for (const char* body : kBodies) {
    SemaFixture f;
    f.source(std::string(kExit) + "fn ! f(c: bool) {\n" + body + "}\n");
    ASSERT_TRUE(f.build()) << body;
    EXPECT_FALSE(f.hasError("sema-never-returns")) << body << ": " << f.dump();
    EXPECT_FALSE(f.hasError("sema-never-body-completes")) << body << ": " << f.dump();
  }
}

TEST(NeverTest, AnUnreachableReturnIsNotAReachableOne) {
  // `while true {} return;` keeps the promise: the `return` is never executed, so
  // reporting it would refuse a program that is fine. This is what separates "the
  // body has a `return`" -- a syntactic check -- from "the body can return", which
  // is the question the promise is actually about.
  SemaFixture f;
  f.source("fn ! f() {\n  while true {\n  }\n  return;\n}\n");
  ASSERT_TRUE(f.build());
  EXPECT_FALSE(f.hasError("sema-never-returns")) << f.dump();
  EXPECT_FALSE(f.hasError("sema-never-body-completes")) << f.dump();
  EXPECT_TRUE(f.hasWarning("sema-unreachable-code")) << f.dump();
}

TEST(NeverTest, ABadReturnIsOneSentenceAndNotTwo) {
  // `return 1;` in a `fn !` is a broken promise *and* a type mismatch, and only
  // the promise is worth saying: the operand has no type to be checked against, so
  // the reader gets the sentence about the statement they wrote.
  SemaFixture f;
  f.source("fn ! f() {\n  return 1;\n}\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-never-returns")) << f.dump();
  EXPECT_FALSE(f.hasError("sema-return-mismatch")) << f.dump();
  EXPECT_FALSE(f.hasError("sema-return-missing-value")) << f.dump();
  EXPECT_EQ(f.errorCount(), 1u) << f.dump();
}

TEST(NeverTest, AVoidFunctionMayReturnANeverOperand) {
  // `return exit(1);` in a `void` function returns exactly nothing, which is what
  // `void` means -- so it is legal, and it is the *same* rule as `die();` without
  // the `return`. Anything with a value is still refused: a `void` function has
  // nowhere to put one.
  {
    SemaFixture f;
    f.source(std::string(kExit) + "fn void f() {\n  return exit(1);\n}\n");
    ASSERT_TRUE(f.build());
    EXPECT_FALSE(f.hasError("sema-return-void-value")) << f.dump();
    EXPECT_EQ(f.errorCount(), 0u) << f.dump();
  }
  {
    SemaFixture f;
    f.source("fn void f() {\n  return 1;\n}\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-return-void-value")) << f.dump();
  }
}

TEST(NeverTest, TheTypeCanBeAValueWhereverAValueIsExpected) {
  // A `!` expression is usable anywhere an ordinary value is: as an argument, as
  // the right side of an assignment, as a `const` initializer. Every one of them
  // converts, vacuously, which is the whole meaning of the type -- and the value
  // that would have been produced is a `poison` in the module and not a hole.
  SemaFixture f;
  f.source(std::string(kExit) + "fn void take(n: i32) {\n}\n"
                                "fn i32 main() {\n"
                                "  take(exit(1));\n"
                                "  let a: i32;\n"
                                "  a = exit(2);\n"
                                "  const c: i32 = exit(3);\n"
                                "  return c;\n"
                                "}\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u) << f.dump();
}

TEST(NeverTest, TheWordCannotBeWrittenOutsideAReturnType) {
  // The written-word rule, which is Rust's on stable: `!` is a return type, and
  // an object or a parameter of that type is refused *with the word in the
  // sentence*, because there is no value for one to hold and the reader should
  // not have to work out which of the two value-less types they typed.
  {
    SemaFixture f;
    f.source("fn i32 main() {\n  let x: ! = 1;\n  return 0;\n}\n");
    ASSERT_TRUE(f.build());
    ASSERT_TRUE(f.hasError("sema-type-not-value")) << f.dump();
    EXPECT_NE(f.firstError().message.find("`!` is not a type an object can have"),
              std::string::npos);
  }
  {
    SemaFixture f;
    f.source("fn i32 g(x: !) {\n  return 0;\n}\n");
    ASSERT_TRUE(f.build());
    ASSERT_TRUE(f.hasError("sema-type-not-value")) << f.dump();
    EXPECT_NE(f.firstError().message.find("`!` is not a type a parameter can have"),
              std::string::npos);
  }
}

TEST(NeverTest, ABindingInferredFromANeverCallAsksForAType) {
  // `let x = die();` has no type to be: the binding would be an object of type
  // `!`, and the program never reaches it. The message says the fix, which is one
  // word the reader meant to write anyway.
  SemaFixture f;
  f.source(std::string(kExit) + "fn i32 main() {\n  let x = exit(1);\n  return 0;\n}\n");
  ASSERT_TRUE(f.build());
  ASSERT_TRUE(f.hasError("sema-type-not-value")) << f.dump();
  EXPECT_NE(f.firstError().message.find("write the type the value would have had"),
            std::string::npos);
}

TEST(NeverTest, MainCannotReturnNever) {
  // The entry point is the one name with a reserved shape, and a `!` `main` is
  // the one way to write a program that never returns a status -- which the
  // platform's `main` cannot accept.
  SemaFixture f;
  f.source(std::string(kExit) + "fn ! main() {\n  exit(0);\n}\n");
  ASSERT_TRUE(f.build());
  ASSERT_TRUE(f.hasError("sema-main-signature")) << f.dump();
  EXPECT_NE(f.firstError().message.find("returns `!`"), std::string::npos);
}

TEST(NeverTest, ThePromiseIsWrittenAndNeverInferred) {
  // A function whose *body* never returns is not `!` for its callers: nothing
  // told the compiler it may assume so, and inferring it would make the type
  // depend on a body the caller may not even have. The optimizer can see the
  // body and does the inference itself; the type system asks for the word.
  SemaFixture f;
  f.source("fn void die() {\n  while true {\n  }\n}\n"
           "fn i32 main() {\n  die();\n}\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-missing-return")) << f.dump();
}

} // namespace
} // namespace minc::test
