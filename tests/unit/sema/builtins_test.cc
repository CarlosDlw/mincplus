// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// A builtin call, checked like a call.
//
// Two things are being tested here and they are different questions. The first is
// that a builtin's *mistakes* are the mistakes a function's are: the same codes,
// the same sentences, one diagnostic each. The second is that the families are
// complete -- one row, every integer width -- and that the test enumerates the
// widths rather than trusting the row, because "one row covers twelve types" is a
// claim until something walks it.
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "sema/sema_fixture.h"

namespace minc::test {
namespace {

TEST(BuiltinsTest, TheResultWidthIsTheArgumentsWidth) {
  // The one property that makes a family a family: no conversion, no promotion,
  // and no second place the width could come from.
  {
    SemaFixture f;
    f.source("fn i32 main() { let x: u8 = 3; let r = clz(x); return 0; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_EQ(f.errorCount(), 0u);
    EXPECT_EQ(f.bindingType("r"), "u8");
  }
  {
    SemaFixture f;
    f.source("fn i32 main() { let x: u64 = 3; let r = popcount(x); return 0; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_EQ(f.errorCount(), 0u);
    EXPECT_EQ(f.bindingType("r"), "u64");
  }
  {
    // A signed argument stays signed: the operation is on the bit pattern, and
    // `clz(-1)` is `clz(0xFFFFFFFF)`, not an error and not a conversion.
    SemaFixture f;
    f.source("fn i32 main() { let x: i16 = -1; let r = clz(x); return 0; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_EQ(f.errorCount(), 0u);
    EXPECT_EQ(f.bindingType("r"), "i16");
  }
}

TEST(BuiltinsTest, ALiteralWithNoContextTakesItsDefault) {
  // `clz(1)` is an `i32` operation, and this is the language's ordinary rule for
  // a literal nothing decides -- the same answer `1` gets anywhere else. The
  // family does not invent a second rule, and the reader who wants 64 bits writes
  // a 64-bit argument.
  SemaFixture f;
  f.source("fn i32 main() { let r = clz(1); return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
  EXPECT_EQ(f.bindingType("r"), "i32");
}

TEST(BuiltinsTest, AWrongCountIsTheSentenceAFunctionGets) {
  {
    SemaFixture f;
    f.source("fn i32 main() { let r = clz(); return 0; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-argument-count"));
    EXPECT_EQ(f.errorCount(), 1u);
    // The same words, produced by the same function: a builtin is a call.
    EXPECT_NE(f.firstError().message.find("this function takes 1 argument(s)"), std::string::npos);
  }
  {
    SemaFixture f;
    f.source("fn i32 main() { let r = clz(1, 2); return 0; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-argument-count"));
    EXPECT_EQ(f.errorCount(), 1u);
    EXPECT_NE(f.firstError().message.find("but 2 were given"), std::string::npos);
  }
}

TEST(BuiltinsTest, ACountDisagreementDoesNotHideAWrongArgument) {
  // The rule a user function's call already follows: a wrong count must not
  // silence a wrong argument, so the argument is still checked and both are
  // reported.
  SemaFixture f;
  f.source("fn i32 main() { let r = clz(1.0, 2.0); return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-argument-count"));
  EXPECT_TRUE(f.hasError("sema-invalid-operands"));
}

TEST(BuiltinsTest, AnArgumentTheEqualsIntegerFamilyRefuses) {
  SemaFixture f;
  f.source("fn i32 main() { let r = clz(1.0); return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-invalid-operands"));
  EXPECT_EQ(f.errorCount(), 1u);
  EXPECT_NE(f.firstError().message.find("must be an integer type"), std::string::npos);
}

TEST(BuiltinsTest, AWidthTheOperationDoesNotExistForIsRefusedHere) {
  // `llvm.bswap` is *invalid* -- not undefined -- for an odd number of bytes, so
  // this has to be a sentence at the call site: the alternative is a program the
  // checker let pass and a module LLVM refuses to verify.
  {
    SemaFixture f;
    f.source("fn i32 main() { let x: u8 = 1; let r = bswap(x); return 0; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-builtin-width"));
    EXPECT_EQ(f.errorCount(), 1u);
    EXPECT_NE(f.firstError().message.find("whole number of bytes"), std::string::npos);
  }
  {
    SemaFixture f;
    f.source("fn i32 main() { let x: u16 = 1; let r = bswap(x); return 0; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_EQ(f.errorCount(), 0u);
    EXPECT_EQ(f.bindingType("r"), "u16");
  }
}

TEST(BuiltinsTest, ABuiltinIsAnOperationAndNotAValue) {
  SemaFixture f;
  f.source("fn i32 main() { let f = clz; return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-builtin-not-a-value"));
  EXPECT_EQ(f.errorCount(), 1u);
  EXPECT_NE(f.firstError().message.find("can only be called"), std::string::npos);
}

TEST(BuiltinsTest, ACallThatNeverComesBackEndsTheFunction) {
  // `__builtin_trap()` is typed `!`, and that one fact is the whole reason the
  // flow pass can see it: `sema` asks an expression's *type* whether control
  // continues, so a row answers by being typed and not by being special-cased.
  {
    SemaFixture f;
    f.source("fn i32 fail() { __builtin_trap(); }\nfn i32 main() { return 0; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_EQ(f.errorCount(), 0u);
  }
  {
    // The same body without the trap is still a missing return, which is what
    // makes the test above about the *type* and not about the walk being lenient.
    SemaFixture f;
    f.source("fn i32 fail() { }\nfn i32 main() { return 0; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-missing-return"));
  }
  {
    // A `!` body whose promise is kept by a loop is what `never.md` already
    // allows; the trap is the other way to keep it.
    SemaFixture f;
    f.source("fn ! fail() { __builtin_trap(); }\nfn i32 main() { return 0; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_EQ(f.errorCount(), 0u);
  }
}

TEST(BuiltinsTest, EveryIntegerWidthOfTheFamilyTypeChecks) {
  // "One row, twelve types" is a claim until something walks it. `isize`/`usize`
  // are in the list on purpose: they are *target* widths, and the same row has to
  // serve both without the table knowing which machine it is on. They are spelled
  // by the decided width -- `isize` on this target *is* `i64` -- which is why the
  // expected answer is written out rather than being the source word.
  struct Width {
    std::string written;
    std::string decided;
  };
  const std::vector<Width> widths = {{"i8", "i8"},   {"i16", "i16"},   {"i32", "i32"},
                                     {"i64", "i64"}, {"i128", "i128"}, {"isize", "i64"},
                                     {"u8", "u8"},   {"u16", "u16"},   {"u32", "u32"},
                                     {"u64", "u64"}, {"u128", "u128"}, {"usize", "u64"}};
  // `const char*` and not `std::string` spelled as a list: a range-for over an
  // initializer list of string literals builds a temporary `std::string` per
  // element, which GCC's `-Wrange-loop-construct` reports.
  for (const Width& width : widths) {
    for (const char* const name : {"clz", "ctz", "popcount"}) {
      SemaFixture f;
      f.source("fn i32 main() { let x: " + width.written + " = 1; let r = " + name +
               "(x); return 0; }\n");
      ASSERT_TRUE(f.build()) << name << " " << width.written;
      EXPECT_EQ(f.errorCount(), 0u) << name << " " << width.written;
      EXPECT_EQ(f.bindingType("r"), width.decided) << name << " " << width.written;
    }
    {
      // The rotate takes the value *and* a count, and the count is its own
      // integer type -- this language converts no integer implicitly, so forcing
      // the count's type would make every count written as a literal an error.
      // The result is the *value's* width, which is what this asserts.
      SemaFixture f;
      f.source("fn i32 main() { let x: " + width.written +
               " = 1; let r = rotl(x, 3); return 0; }\n");
      ASSERT_TRUE(f.build()) << width.written;
      EXPECT_EQ(f.errorCount(), 0u) << width.written;
      EXPECT_EQ(f.bindingType("r"), width.decided) << width.written;
    }
  }
}

TEST(BuiltinsTest, APreludeNameIsShadowableInAFunctionAndNotAtFileScope) {
  // The two spelling classes, from the checker's side. A prelude name is an
  // ordinary binding in the file scope: a local shadows it, and a file-scope
  // declaration of it is a redeclaration -- which is the same treatment `true`
  // already gets, and the reason `clz` needs no reservation.
  {
    SemaFixture f;
    f.source("fn i32 main() { let clz: i32 = 1; return clz; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_EQ(f.errorCount(), 0u);
  }
  {
    // A file-scope declaration of a name the table binds is a redeclaration, and
    // the check is `resolve`'s -- it is the stage that owns names, and the one
    // that sees every declaration -- so what is asserted is that the checker
    // produces nothing at all: the pipeline stopped a stage earlier.
    SemaFixture f;
    f.source("let clz: i32 = 1;\nfn i32 main() { return 0; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_EQ(f.errorCount(), 0u);
    EXPECT_TRUE(f.hasResolveError("resolve-redeclaration"));
  }
}

} // namespace
} // namespace minc::test
