// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// `[]T`: the type, the four forms a view is taken with, the bounds that are
// checked, and the six refusals that keep a view from meaning something else.
//
// Everything here goes through the whole front end (`SemaFixture`), so what is
// under test is the *program*: `a[1..3]` is read by the real parser, typed by the
// real checker, and asked what type it has. The layout of the descriptor -- two
// words, aligned like a pointer -- is `store_test.cc`'s, and the module it
// becomes is `ir/slice_test.cc`'s.
#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "sema/sema_fixture.h"

namespace minc::test {
namespace {

// The four forms, on the three bases. One fixture per case so a failure names
// the form rather than the file, and every case asserts the *type* rather than
// only that the program was accepted: `[]i32` from an array and `[]i32` from a
// pointer being one type is the property everything downstream rests on.
TEST(SliceTest, TheFourFormsAreOneType) {
  const std::string_view forms[] = {"a[..]", "a[0..]", "a[..4]", "a[1..3]"};
  for (const std::string_view form : forms) {
    SemaFixture f;
    f.source("fn i32 main() { let a: [4]i32 = [1, 2, 3, 4]; let s: []i32 = " + std::string(form) +
             "; return 0; }\n");
    ASSERT_TRUE(f.build()) << form;
    EXPECT_EQ(f.errorCount(), 0u) << form << ": " << f.firstError().message;
    EXPECT_EQ(f.bindingType("s"), "[]i32") << form;
  }
}

TEST(SliceTest, TheThreeBasesAreOneType) {
  // An array, a slice of one, and a pointer into one. The three are different
  // *extents* and the same type, which is the whole point of the descriptor: the
  // length became a value rather than a spelling.
  SemaFixture f;
  f.source("fn i32 main() {\n"
           "  let a: [4]i32 = [1, 2, 3, 4];\n"
           "  let fromArray: []i32 = a[0..2];\n"
           "  let fromSlice: []i32 = fromArray[0..1];\n"
           "  let p: *i32 = &a[0];\n"
           "  let fromPointer: []i32 = p[0..2];\n"
           "  return 0;\n"
           "}\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u) << f.firstError().message;
  EXPECT_EQ(f.bindingType("fromArray"), "[]i32");
  EXPECT_EQ(f.bindingType("fromSlice"), "[]i32");
  EXPECT_EQ(f.bindingType("fromPointer"), "[]i32");
}

TEST(SliceTest, AViewOfAViewIsTheSameElement) {
  // `s2 = s1[1..3]` is a view of a view, and its type is `[]u8` because its
  // *element* is a `u8` -- not `[]u8` and not an array. A reader who expected a
  // nested view would be expecting the type of `s1[1..3][0]`.
  SemaFixture f;
  f.source("fn i32 main() { let a: [8]u8 = [1, 2, 3, 4, 5, 6, 7, 8];\n"
           "  let s1: []u8 = a[2..6]; let s2: []u8 = s1[1..3]; return s2[0]; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u) << f.firstError().message;
  EXPECT_EQ(f.bindingType("s2"), "[]u8");
}

TEST(SliceTest, TheEndOfAViewIsOnePastTheLastElement) {
  // The convention, at the two places it decides something: `a[0..4]` on a
  // `[4]i32` is the whole object and not an error, and `a[4..4]` is the empty
  // view at its end. Both are the *arithmetic* of the descriptor's length being
  // `end - begin`, and a language that made `a[0..4]` an error would have no way
  // to write "the whole thing" or "nothing at all".
  SemaFixture whole;
  whole.source(
      "fn i32 main() { let a: [4]i32 = [1, 2, 3, 4]; let s: []i32 = a[0..4]; return 0; }\n");
  ASSERT_TRUE(whole.build());
  EXPECT_EQ(whole.errorCount(), 0u) << whole.firstError().message;

  SemaFixture empty;
  empty.source(
      "fn i32 main() { let a: [4]i32 = [1, 2, 3, 4]; let s: []i32 = a[4..4]; return 0; }\n");
  ASSERT_TRUE(empty.build());
  EXPECT_EQ(empty.errorCount(), 0u) << empty.firstError().message;
}

TEST(SliceTest, TheConstantBoundsAreCheckedAgainstTheObject) {
  // The one check C's type system cannot make and this one can: the count is in
  // the type and the bound is a number, so a view past the end is arithmetic on
  // two constants. `a[0..5]` is refused at the *end*, and the sentence names the
  // bound that is wrong rather than the form.
  SemaFixture past;
  past.source(
      "fn i32 main() { let a: [4]i32 = [1, 2, 3, 4]; let s: []i32 = a[0..5]; return 0; }\n");
  ASSERT_TRUE(past.build());
  EXPECT_TRUE(past.hasError("sema-index-out-of-range"));
  EXPECT_NE(past.firstError().message.find("the end 5"), std::string::npos)
      << past.firstError().message;

  SemaFixture below;
  below.source(
      "fn i32 main() { let a: [4]i32 = [1, 2, 3, 4]; let s: []i32 = a[-1..2]; return 0; }\n");
  ASSERT_TRUE(below.build());
  EXPECT_TRUE(below.hasError("sema-index-out-of-range")) << below.firstError().message;
}

TEST(SliceTest, ABoundIsAnIntegerAtIndexWidth) {
  // A bound of any integer width is accepted, exactly as an index is: the
  // conversion to the index width is the checker's job and the lowering reads it
  // from the record. A bound that is not an integer is refused with the same code
  // an index gets, because it is the same question.
  SemaFixture narrow;
  narrow.source("fn i32 main() { let a: [4]i32 = [1, 2, 3, 4]; let end: u8 = 3;\n"
                "  let s: []i32 = a[0..end]; return 0; }\n");
  ASSERT_TRUE(narrow.build());
  EXPECT_EQ(narrow.errorCount(), 0u) << narrow.firstError().message;

  SemaFixture decimal;
  decimal.source(
      "fn i32 main() { let a: [4]i32 = [1, 2, 3, 4]; let s: []i32 = a[0.0..2]; return 0; }\n");
  ASSERT_TRUE(decimal.build());
  EXPECT_TRUE(decimal.hasError("sema-index-not-integer")) << decimal.firstError().message;
}

TEST(SliceTest, EveryRefusalHasItsSentence) {
  struct Case {
    std::string_view source;
    std::string_view code;
    std::string_view fragment;
  };
  const Case cases[] = {
      // A view of a number: no elements, no extent.
      {"fn i32 main() { let x: i32 = 1; let s: []i32 = x[..]; return 0; }\n",
       "sema-slice-not-viewable", "no extent to measure a bound against"},
      // A view of `void`: an object with no size has no elements to walk.
      {"fn i32 main() { let p: *void = null; let s: []u8 = p[0..2]; return 0; }\n",
       "sema-pointer-void-access", "`*void` cannot be viewed"},
      // A pointer with one bound written. The only base whose extent is in no
      // type, so the only one that cannot infer the other half.
      {"fn i32 main() { let a: [4]i32 = [1, 2, 3, 4]; let p: *i32 = &a[0];\n"
       "  let s: []i32 = p[..2]; return 0; }\n",
       "sema-slice-pointer-needs-both-bounds", "both bounds are written"},
      // Both bounds written and in the wrong order -- and each one *inside* the
      // object, which is why this is not the out-of-range sentence.
      {"fn i32 main() { let a: [4]i32 = [1, 2, 3, 4]; let s: []i32 = a[3..1]; return 0; }\n",
       "sema-slice-bounds-reversed", "first bound is a beginning"},
      // A slice has no literal: it is a view, so the storage has to exist first.
      {"fn i32 main() { let s: []i32 = []i32{1, 2, 3}; return 0; }\n", "sema-initializer-shape",
       "a slice has no literal"},
      // No decay in either direction: an array is not a view, and a view is not
      // an array.
      {"fn i32 main() { let a: [4]i32 = [1, 2, 3, 4]; let s: []i32 = a; return 0; }\n",
       "sema-invalid-assignment", "`[4]i32` cannot be used as `[]i32`"},
      {"fn i32 f(s: []i32) { return 0; }\n"
       "fn i32 main() { let a: [4]i32 = [1, 2, 3, 4]; return f(a); }\n",
       "sema-invalid-assignment", "as this argument"},
      // A view is a value and not a place: `&a[0..2]` would be a pointer to a
      // descriptor built where it was written.
      {"fn i32 main() { let a: [4]i32 = [1, 2, 3, 4]; let p: *[]i32 = &a[0..2]; return 0; }\n",
       "sema-address-of-non-lvalue", "has no address"},
      // The element rule the array shares, asked of the view.
      {"fn i32 main() { let s: []void; return 0; }\n", "sema-malformed-type",
       "cannot be a slice element"},
  };
  for (const Case& one : cases) {
    SemaFixture f;
    f.source(std::string(one.source));
    ASSERT_TRUE(f.build()) << one.source;
    EXPECT_TRUE(f.hasError(one.code)) << one.source << ": " << f.firstError().message;
    if (f.errorCount() == 0) {
      continue;
    }
    EXPECT_NE(f.firstError().message.find(one.fragment), std::string::npos)
        << one.source << ": " << f.firstError().message;
  }
}

TEST(SliceTest, TheBoundarySaysWhatToWriteInstead) {
  // The two directions of an `extern` signature, each with the sentence that
  // names the boundary rather than the gap: a descriptor's layout is this
  // compiler's, and the two words a foreign callee can read are a pointer and a
  // length.
  SemaFixture parameter;
  parameter.source("extern fn i32 f(s: []i32);\nfn i32 main() { return 0; }\n");
  ASSERT_TRUE(parameter.build());
  EXPECT_TRUE(parameter.hasError("sema-extern-aggregate"));
  EXPECT_NE(parameter.firstError().message.find("a pointer and a length"), std::string::npos)
      << parameter.firstError().message;

  SemaFixture result;
  result.source("extern fn []i32 g();\nfn i32 main() { return 0; }\n");
  ASSERT_TRUE(result.build());
  EXPECT_TRUE(result.hasError("sema-extern-aggregate"));
  EXPECT_NE(result.firstError().message.find("a pointer and a length"), std::string::npos)
      << result.firstError().message;

  // ... and what *is* allowed: a pointer to the descriptor, which is a pointer
  // like any other and whose pointee the checker need not know the layout of.
  SemaFixture indirect;
  indirect.source("fn i32 main() { let a: [4]i32 = [1, 2, 3, 4]; let s: []i32 = a[..];\n"
                  "  let p: *[]i32 = &s; return 0; }\n");
  ASSERT_TRUE(indirect.build());
  EXPECT_EQ(indirect.errorCount(), 0u) << indirect.firstError().message;
}

TEST(SliceTest, ASliceIsAnObjectAndNotAnArray) {
  // The two are different types with the same element, so every place that asks
  // "which aggregate is this" answers differently: an array can be an element of
  // an array, a slice can be the element too, and neither is the other.
  SemaFixture f;
  f.source("fn i32 main() {\n"
           "  let table: [2][4]i32 = [[1, 2, 3, 4], [5, 6, 7, 8]];\n"
           "  let row: []i32 = table[0][0..2];\n"
           "  let rows: [][4]i32 = table[0..1];\n"
           "  return row[0] + rows[0][0];\n"
           "}\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u) << f.firstError().message;
  EXPECT_EQ(f.bindingType("row"), "[]i32");
  EXPECT_EQ(f.bindingType("rows"), "[][4]i32");
}

TEST(SliceTest, AViewIsReadAsAValueAndItsBaseAsAPlace) {
  // Definite assignment, on the one base where the distinction shows. An array
  // is a place: taking a view of it reads no byte, so `a[0..2]` is legal before
  // any element has been written -- which is what makes `a[..]` usable right
  // after the declaration. A *slice* is a value, so viewing one reads it, and an
  // unassigned slice is the ordinary read-before-assignment.
  SemaFixture place;
  place.source("fn i32 main() { let a: [4]i32; let s: []i32 = a[0..2]; return 0; }\n");
  ASSERT_TRUE(place.build());
  EXPECT_EQ(place.errorCount(), 0u) << place.firstError().message;

  SemaFixture value;
  value.source("fn i32 main() { let s: []i32; let t: []i32 = s[0..1]; return 0; }\n");
  ASSERT_TRUE(value.build());
  EXPECT_TRUE(value.hasError("sema-use-before-assignment")) << value.firstError().message;
}

} // namespace
} // namespace minc::test
