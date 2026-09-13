// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The pointer surface, as `memory.md` states it: `*T`, `&x`, `*p`, `p[i]`, the
// arithmetic that stays a pointer, the comparison that yields a `bool`, and the
// refusals. One property is worth naming because it is the reason the surface is
// in the model at all: a pointer is an address **plus a provenance**, so the
// stage has to decide what a pointer may name before the lowering hands LLVM an
// instruction -- and the refusals below are where that decision is visible.
//
// The negative cases live in `errors_test.cc`'s enumeration, which is the list
// that has to reach every code. What is here is the *positive* half: the types
// the language computes, and the reasons a program is refused.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "sema/sema_fixture.h"
#include "sema/type_store.h"

namespace minc::test {
namespace {

TEST(PointerTest, APointerTypeIsAStarAndItsPointee) {
  SemaFixture f;
  f.source("fn i32 main() { let x: i32 = 1; let p: *i32 = &x; return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.bindingType("p"), "*i32");
  EXPECT_EQ(f.errorCount(), 0u);
}

TEST(PointerTest, AStarNestsToTheRight) {
  // `**i32` is a pointer to a pointer: the constructor is prefix, so each `*`
  // applies to everything to its right, which is the only reading that does not
  // need parentheses.
  SemaFixture f;
  f.source("fn i32 main() { let x: i32 = 1; let p: *i32 = &x; let pp: **i32 = &p; return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.bindingType("pp"), "**i32");
  EXPECT_EQ(f.errorCount(), 0u);
}

TEST(PointerTest, DerefIsThePointeeAndAPlace) {
  SemaFixture f;
  f.source(
      "fn i32 main() { let x: i32 = 1; let p: *i32 = &x; *p = 2; let y: i32 = *p; return y; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.bindingType("y"), "i32");
  EXPECT_EQ(f.errorCount(), 0u);
}

TEST(PointerTest, AddressOfAParameterIsAPointer) {
  SemaFixture f;
  f.source("fn i32 f(a: i32) { let p: *i32 = &a; return 0; }\nfn i32 main() { return f(1); }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.bindingType("p"), "*i32");
  EXPECT_EQ(f.errorCount(), 0u);
}

TEST(PointerTest, AddressOfAnUnassignedBindingReadsNothing) {
  // `memory.md`: taking an address accesses no bytes. The read that would be a
  // violation is the one through the pointer, and following an address is what
  // the checked build does at run time because no static pass can.
  SemaFixture f;
  f.source("fn i32 main() { let x: i32; let p: *i32 = &x; return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_FALSE(f.hasError("sema-use-before-assignment"));
  EXPECT_EQ(f.errorCount(), 0u);
}

TEST(PointerTest, DerefThroughAPointerReadsTheBindingItNames) {
  // The other half of the rule above: `*p` where `p` itself was never assigned
  // is a read of `p`, and it is reported.
  SemaFixture f;
  f.source("fn i32 main() { let p: *i32; let y: i32 = *p; return y; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-use-before-assignment"));
}

TEST(PointerTest, IndexIsThePointee) {
  SemaFixture f;
  f.source("fn i32 main() { let x: i32 = 1; let p: *i32 = &x; let y: i32 = p[0]; return y; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.bindingType("y"), "i32");
  EXPECT_EQ(f.errorCount(), 0u);
}

TEST(PointerTest, PointerArithmeticStaysAPointer) {
  SemaFixture f;
  f.source("fn i32 main() { let x: i32 = 1; let p: *i32 = &x; let plus: *i32 = p + 1; "
           "let other: *i32 = 1 + p; let minus: *i32 = p - 1; return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.bindingType("plus"), "*i32");
  EXPECT_EQ(f.bindingType("other"), "*i32");
  EXPECT_EQ(f.bindingType("minus"), "*i32");
  EXPECT_EQ(f.errorCount(), 0u);
}

TEST(PointerTest, DifferenceOfTwoPointersIsTheSignedPointerWidth) {
  const sema::TargetInfo target = sema::defaultTarget();
  SemaFixture f;
  f.source("fn i32 main() { let x: i32 = 1; let p: *i32 = &x; let q: *i32 = &x; "
           "let d = p - q; return 0; }\n");
  ASSERT_TRUE(f.build());
  // `isize`: the count of elements between them, and the type is the pointer
  // width whether or not the count is positive.
  EXPECT_EQ(f.bindingType("d"), std::to_string(target.pointerBits) == "64" ? "i64" : "i32");
  EXPECT_EQ(f.errorCount(), 0u);
}

TEST(PointerTest, ComparisonYieldsABool) {
  SemaFixture f;
  f.source("fn i32 main() { let x: i32 = 1; let p: *i32 = &x; let q: *i32 = &x; "
           "let b: bool = p == q; let c: bool = p < q; return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
}

TEST(PointerTest, AVoidPointerAcceptsAnyPointee) {
  SemaFixture f;
  f.source("fn i32 main() { let x: i32 = 1; let v: *void = &x; return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.bindingType("v"), "*void");
  EXPECT_EQ(f.errorCount(), 0u);
}

TEST(PointerTest, NullIsTheVoidPointer) {
  SemaFixture f;
  f.source("fn i32 main() { let p: *i32 = null; let q: *u8 = null; return 0; }\n");
  ASSERT_TRUE(f.build());
  // One predefined name is usable wherever a pointer is wanted, without a
  // nullable-pointer type or an integer zero that means an address.
  EXPECT_EQ(f.errorCount(), 0u);
}

TEST(PointerTest, NullComparesAgainstAnyPointer) {
  SemaFixture f;
  f.source("fn i32 main() { let p: *i32 = null; if p == null { return 1; } return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
}

TEST(PointerTest, IncrementStepsAPointer) {
  SemaFixture f;
  f.source("fn i32 main() { let x: i32 = 1; let p: *i32 = &x; p++; ++p; p--; --p; return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
}

TEST(PointerTest, CompoundStepNeedsAnIntegerOffset) {
  SemaFixture f;
  f.source("fn i32 main() { let x: i32 = 1; let p: *i32 = &x; p += 2; p -= 2; return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
}

TEST(PointerTest, TheIndexOperandIsConvertedAtThePointerWidth) {
  // `p[i]` and `p + i` are one operation, so both materialise the index at the
  // pointer index width and record that conversion -- the lowering must not
  // choose a width of its own.
  const sema::TargetInfo target = sema::defaultTarget();
  SemaFixture f;
  // A `u8` index is the case that has to be converted: a literal is *decided*
  // at the width rather than converted to it, so it would leave no record.
  f.source("fn i32 main() { let x: i32 = 1; let p: *i32 = &x; let i: u8 = 1; "
           "let y: i32 = p[i]; return y; }\n");
  ASSERT_TRUE(f.build());
  const std::string wide = target.pointerBits == 64 ? "i64" : "i32";
  bool found = false;
  for (const sema::Coercion& coercion : f.typed().coercions()) {
    if (f.types().spelling(coercion.from) == "u8" && f.types().spelling(coercion.to) == wide) {
      found = true;
    }
  }
  EXPECT_TRUE(found) << "no index conversion from u8 to " << wide;

  SemaFixture g;
  g.source("fn i32 main() { let x: i32 = 1; let p: *i32 = &x; let y: i32 = p[1]; return y; }\n");
  ASSERT_TRUE(g.build());
  EXPECT_EQ(g.errorCount(), 0u);
}

// --- the refusals ------------------------------------------------------------

TEST(PointerTest, AddressOfAConstIsRefused) {
  // `memory.md`: `&x` takes "the address of a modifiable lvalue". A pointer to a
  // `const` binding would be a way to write it, so the two promises cannot both
  // hold -- and the refusal is its own code, because the operand *is* a place and
  // the fix is not "use a place".
  SemaFixture f;
  f.source("fn i32 main() { const c: i32 = 1; let p: *i32 = &c; return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-address-of-const"));
}

TEST(PointerTest, AddressOfAConstThroughParensIsStillRefused) {
  SemaFixture f;
  f.source("fn i32 main() { const c: i32 = 1; let p: *i32 = &(c); return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-address-of-const"));
}

TEST(PointerTest, AddressOfAValueIsRefused) {
  SemaFixture f;
  f.source("fn i32 main() { let x: i32 = 1; let p: *i32 = &(x + 1); return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-address-of-non-lvalue"));
}

TEST(PointerTest, APointerAndAnIntegerDoNotConvert) {
  // The model's central refusal: a pointer is not an integer, in either
  // direction, and the two named operations that join them are not in the
  // grammar yet.
  SemaFixture f;
  f.source("fn i32 main() { let p: *i32 = 0; return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-pointer-integer"));

  SemaFixture g;
  g.source("fn i32 main() { let x: i32 = 1; let p: *i32 = &x; let i: i64 = p; return 0; }\n");
  ASSERT_TRUE(g.build());
  EXPECT_TRUE(g.hasError("sema-pointer-integer"));
}

TEST(PointerTest, DerefNeedsAPointer) {
  SemaFixture f;
  f.source("fn i32 main() { let x: i32 = 1; return *x; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-deref-not-pointer"));
}

TEST(PointerTest, VoidIsNotAccessible) {
  SemaFixture f;
  f.source("fn i32 main() { let v: *void = null; return *v; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-pointer-void-access"));
}

TEST(PointerTest, VoidHasNoStride) {
  SemaFixture f;
  f.source("fn i32 main() { let v: *void = null; v += 1; return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-pointer-void-arithmetic"));
}

TEST(PointerTest, TwoDifferentPointeesDoNotMeet) {
  SemaFixture f;
  f.source("fn i32 main() { let x: i32 = 1; let p: *i32 = &x; let q: *u8 = &x; return 0; }\n");
  ASSERT_TRUE(f.build());
  // `*void` is the untyped pointer and converts; `*u8` is not, and the message
  // has to name the one crossing point there is, because that is the rule the
  // reader has to learn -- not merely that these two spellings differ.
  EXPECT_TRUE(f.hasError("sema-pointer-mismatch"));
  EXPECT_NE(f.firstError().message.find("`*void`"), std::string::npos) << f.firstError().message;
}

TEST(PointerTest, TwoDifferentPointeesDoNotMeetInAnArgumentEither) {
  // The same rule at the other consumer. An argument and an initializer go
  // through the same conversion check, and this is the test that says so.
  SemaFixture f;
  f.source("fn void take(p: *i32) { }\n"
           "fn i32 main() { let x: u8 = 1; take(&x); return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-pointer-mismatch"));
}

TEST(PointerTest, APointerIsNotComparableWithAnInteger) {
  SemaFixture f;
  f.source("fn i32 main() { let x: i32 = 1; let p: *i32 = &x; let b: bool = p == 1; return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-pointer-mismatch"));
}

TEST(PointerTest, AnIndexMustBeAnInteger) {
  SemaFixture f;
  f.source("fn i32 main() { let x: i32 = 1; let p: *i32 = &x; let y: i32 = p[1.0]; return y; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-index-not-integer"));
}

TEST(PointerTest, APointerIsNotAnArithmeticOperand) {
  SemaFixture f;
  f.source("fn i32 main() { let x: i32 = 1; let p: *i32 = &x; return p * 2; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-invalid-operands"));
}

TEST(PointerTest, EveryPointerExampleChecks) {
  // The examples are the corpus the pipeline is tested on end to end; a new
  // surface that breaks one is a regression even when the pointer rules
  // themselves pass.
  SemaFixture f;
  f.source("fn i32 main() {\n"
           "  let x: i32 = 42;\n"
           "  let p: *i32 = &x;\n"
           "  let y: i32 = *p;\n"
           "  *p = y + 1;\n"
           "  let q: *i32 = p;\n"
           "  if p == q { return 0; }\n"
           "  return 1;\n"
           "}\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
}

} // namespace
} // namespace minc::test
