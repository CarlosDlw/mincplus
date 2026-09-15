// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// `[N]T`: the type in a real program, the count in the identity, the subscript
// and its bounds, and the record the lowering reads.
//
// The tests here go through the whole front end (`SemaFixture`), so a spelling is
// read by the real parser and the real reader: `[0x10]i32` and `[16]i32` are the
// same type because the *source* says so, not because a test built the parts by
// hand. What the parts themselves do is `type_test.cc`'s.
#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "sema/sema_fixture.h"

namespace minc::test {
namespace {

TEST(ArrayTest, AnArrayTypeIsATypeInAProgram) {
  SemaFixture f;
  f.source("fn i32 main() { let a: [4]i32; return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u) << (f.errorCount() == 0 ? "" : f.firstError().message);
  // The canonical spelling of the type the binding names, written back as
  // `[4]i32` -- what a reader can type again.
  EXPECT_EQ(f.bindingType("a"), "[4]i32");
}

TEST(ArrayTest, TheCountIsAValueAndNotASpelling) {
  SemaFixture f;
  // Three spellings of one count, one of which is a leading zero: `.mx` has no
  // implicit octal, so `010` is ten here exactly as it is anywhere else in the
  // language. If the identity were the *text*, these would be three types.
  f.source("fn i32 main() { let a: [0x10]i32; let b: [16]i32; let c: [010]i32;\n"
           "              let d: [20]i32; return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
  EXPECT_EQ(f.bindingType("a"), "[16]i32");
  EXPECT_EQ(f.bindingType("b"), "[16]i32");
  EXPECT_EQ(f.bindingType("c"), "[10]i32");
  // ... and a count that is not 16 is a different type, which is the property
  // everything else rests on.
  EXPECT_EQ(f.bindingType("d"), "[20]i32");
}

TEST(ArrayTest, EveryArrayRefusalHasItsSentence) {
  struct Case {
    std::string_view source;
    std::string_view fragment;
  };
  const Case cases[] = {
      // `[]T` is the reserved slice spelling, and the sentence says what to write
      // today.
      {"fn i32 main() { let a: []i32; return 0; }\n", "reserved spelling of a slice"},
      // A count of zero is written and impossible: it is not the slice above.
      {"fn i32 main() { let a: [0]i32; return 0; }\n", "count of an array type is at least 1"},
      // An element that cannot be stored.
      {"fn i32 main() { let a: [4]void; return 0; }\n", "cannot be an array element"},
      // A count no 64-bit number can hold, and a count that fits but whose bytes
      // do not. Two sentences, because they are two different repairs.
      {"fn i32 main() { let a: [18446744073709551616]i32; return 0; }\n",
       "has to be a number that fits in 64 bits"},
      {"fn i32 main() { let a: [18446744073709551615]i64; return 0; }\n",
       "larger than this target can address"},
      // A constructor on the wrong side of the type it builds. The reader names
      // the side rather than saying the token is not a type.
      {"fn i32 main() { let a: i32[4]; return 0; }\n", "an array type is written `[N]T`"},
  };
  for (const Case& one : cases) {
    SemaFixture f;
    f.source(std::string(one.source));
    ASSERT_TRUE(f.build()) << one.source;
    EXPECT_EQ(f.errorCount(), 1u) << one.source;
    if (f.errorCount() == 0) {
      continue;
    }
    EXPECT_NE(f.firstError().message.find(one.fragment), std::string::npos)
        << one.source << ": " << f.firstError().message;
  }
}

TEST(ArrayTest, TheTwoLiteralFormsProduceTheSameValue) {
  // The typed form carries its type; the context form takes the one its consumer
  // gives it. Both are one value of `[4]i32`, which is the property that makes
  // the second form worth having at all -- and neither is deferred in the type
  // store, because a literal that cannot be an operand of any operator needs no
  // deferred kind (`arrays.md` decision 8, step 7).
  SemaFixture typed;
  typed.source("fn i32 main() { let a = [4]i32{1, 2, 3, 4}; return a[0]; }\n");
  ASSERT_TRUE(typed.build());
  EXPECT_EQ(typed.errorCount(), 0u) << typed.firstError().message;
  EXPECT_EQ(typed.bindingType("a"), "[4]i32");

  SemaFixture context;
  context.source("fn i32 main() { let a: [4]i32 = [1, 2, 3, 4]; return a[0]; }\n");
  ASSERT_TRUE(context.build());
  EXPECT_EQ(context.errorCount(), 0u) << context.firstError().message;
  EXPECT_EQ(context.bindingType("a"), "[4]i32");

  // `[_]` takes the count from the elements, and the elements are counted once:
  // that is the whole point of writing it.
  SemaFixture inferred;
  inferred.source("fn i32 main() { let d = [_]u8{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}; return d[0]; }\n");
  ASSERT_TRUE(inferred.build());
  EXPECT_EQ(inferred.errorCount(), 0u) << inferred.firstError().message;
  EXPECT_EQ(inferred.bindingType("d"), "[10]u8");

  // Nested, without naming the inner type: the element type of the binding is
  // what each row is checked against, one level down.
  SemaFixture nested;
  nested.source("fn i32 main() { let g: [2][3]i32 = [[1, 2, 3], [4, 5, 6]]; return g[1][2]; }\n");
  ASSERT_TRUE(nested.build());
  EXPECT_EQ(nested.errorCount(), 0u) << nested.firstError().message;
  EXPECT_EQ(nested.bindingType("g"), "[2][3]i32");
}

TEST(ArrayTest, AnInitializerRefusesWhatCWouldHaveFilledIn) {
  struct Case {
    std::string_view source;
    std::string_view fragment;
  };
  const Case cases[] = {
      // The length is exact. C's zero-fill is how an array ends up half written
      // with no diagnostic at all, and the sentence carries both repairs.
      {"fn i32 main() { let a = [4]i32{1, 2, 3}; return 0; }\n", "an array's length is exact"},
      {"fn i32 main() { let a: [4]i32 = [1, 2, 3, 4, 5, 6]; return 0; }\n",
       "an array's length is exact"},
      // A fill whose two numbers disagree: a second chance at an off-by-one.
      {"fn i32 main() { let a = [4]u8{0; 8}; return 0; }\n",
       "the two numbers have to be the same one"},
      // A count the compiler can read is what a fill is *for*: the value is
      // written once and the number says how many times.
      {"fn i32 main() { let n: i32 = 4; let a: [4]u8 = [0; n]; return 0; }\n",
       "has to be a number this compiler can read"},
      // An empty group has no meaning: zero elements is not a count, and the
      // fill is the one spelling of "many of the same".
      {"fn i32 main() { let a = [4]i32{}; return 0; }\n", "has no elements"},
      // No context: the type is not known, and the sentence names both fixes.
      {"fn i32 main() { let a = [1, 2, 3]; return 0; }\n",
       "annotate it (`let a: [3]i32 = [1, 2, 3];`)"},
      // The context is known and is not an array: two different objects and no
      // conversion between them.
      {"fn i32 main() { let a: i32 = [1, 2]; return 0; }\n",
       "an array does not convert to anything else"},
      // `_` needs a list, and it needs an initializer: it is a count that comes
      // from somewhere, and those are the two places it can come from.
      {"fn i32 main() { let a = [_]i32{0; 4}; return 0; }\n", "names the count"},
      {"fn i32 main() { let a: [_]i32 = [1, 2]; return 0; }\n",
       "`_` is the count an initializer takes from its own elements"},
  };
  for (const Case& one : cases) {
    SemaFixture f;
    f.source(std::string(one.source));
    ASSERT_TRUE(f.build()) << one.source;
    EXPECT_GE(f.errorCount(), 1u) << one.source;
    // The fragment is looked for in *every* sentence and not only the first: a
    // shape mistake can be reported next to the node it is about, and which of
    // the two the reader sees first is the position's business.
    bool found = false;
    for (const sema::SemaError& error : f.errors()) {
      if (error.message.find(one.fragment) != std::string::npos) {
        found = true;
      }
    }
    EXPECT_TRUE(found) << one.source << ": " << f.firstError().message;
  }
}

TEST(ArrayTest, ASubscriptNeedsAnIntegerAndAConstantOneHasToFit) {
  SemaFixture good;
  // A runtime index is checked against nothing at compile time: the extent is
  // what the checked build guards (`arrays.md` decision 7), and the type of the
  // element is the type of the access.
  good.source("fn i32 f(a: [4]i32, i: i32) { return a[i]; }\nfn i32 main() { return 0; }\n");
  ASSERT_TRUE(good.build());
  EXPECT_EQ(good.errorCount(), 0u) << good.firstError().message;
  EXPECT_EQ(good.typeOfSpelling("a[i]"), "i32"); // NOLINT(readability-identifier-length)

  struct Case {
    std::string_view index;
    bool outOfRange;
  };
  const Case cases[] = {
      {"0", false},
      {"3", false},
      {"4", true},
      {"7", true},
      {"-1", true},
      // A folded constant index is the same question as a literal one, and the
      // fold is the one the checker already does for every other constant.
      {"2 + 2", true},
      {"1 + 1", false},
      // An index that does not fit the array *and* is unsigned: the comparison is
      // made in the index's own signedness, so this is out of range and not -1.
      {"4294967295u", true},
  };
  for (const Case& one : cases) {
    SemaFixture f;
    f.source("fn i32 f(a: [4]i32) { return a[" + std::string(one.index) +
             "]; }\nfn i32 main() { return 0; }\n");
    ASSERT_TRUE(f.build()) << one.index;
    EXPECT_EQ(f.hasError("sema-index-out-of-range"), one.outOfRange)
        << one.index << ": " << (f.errorCount() == 0 ? "" : f.firstError().message);
  }

  SemaFixture bad;
  bad.source("fn i32 f(a: [4]i32, x: f64) { return a[x]; }\nfn i32 main() { return 0; }\n");
  ASSERT_TRUE(bad.build());
  EXPECT_TRUE(bad.hasError("sema-index-not-integer"));

  SemaFixture notAnArray;
  // `[]` on an integer: the message names both of the things it accepts, because
  // a reader who wrote `x[i]` meant one of exactly two shapes.
  notAnArray.source("fn i32 main() { let x: i32 = 1; return x[0]; }\n");
  ASSERT_TRUE(notAnArray.build());
  EXPECT_NE(notAnArray.firstError().message.find("an array or a pointer on the left"),
            std::string::npos)
      << notAnArray.firstError().message;
}

TEST(ArrayTest, AnArrayDoesNotDecayAndTheFixIsInTheSentence) {
  SemaFixture f;
  // The one conversion this language deliberately does not have. `a` is a value
  // and `*i32` is not it; the alternative C chose is why `sizeof` lies about a
  // parameter and why no C compiler can refuse `a[10]` (`arrays.md` decision 2).
  f.source("fn i32 main() { let a: [4]i32; let p: *i32 = a; return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-invalid-assignment"));
  const std::string message = f.firstError().message;
  EXPECT_NE(message.find("does not decay to a pointer"), std::string::npos) << message;
  EXPECT_NE(message.find("`&a[0]`"), std::string::npos) << message;

  // The two spellings that *are* pointers, and they are different types: the
  // element and the whole object.
  SemaFixture named;
  named.source("fn i32 main() { let a: [4]i32; let p: *i32 = &a[0]; let q: *[4]i32 = &a;\n"
               "              return 0; }\n");
  ASSERT_TRUE(named.build());
  EXPECT_EQ(named.errorCount(), 0u) << named.firstError().message;
}

TEST(ArrayTest, TheRecordCarriesTheExtentAndTheProvenance) {
  // A parameter's array is the caller's copy: the access is in bounds, and that
  // is a fact about the call rather than something this unit can prove -- so the
  // provenance is `foreign` and only the extent survives, because the count is in
  // the type (`arrays.md` decision 26).
  SemaFixture parameter;
  parameter.source("fn i32 f(a: [4]i32) { return a[1]; }\nfn i32 main() { return 0; }\n");
  ASSERT_TRUE(parameter.build());
  EXPECT_EQ(parameter.accessOf("a[1]"), "foreign i32 4") << parameter.accessOf("a[1]");

  // A local array is an object this unit named, so the checked build can bounds
  // check it in the module. The store below is what `arrays.md` decision 23 says
  // it is -- a legal write that assigns nothing -- so the read after it is still
  // reported as reading an unassigned object, which is why this test asserts the
  // record and not the error count.
  SemaFixture local;
  local.source("fn i32 main() { let t: [4]i32; t[0] = 1; return t[0]; }\n");
  ASSERT_TRUE(local.build());
  EXPECT_EQ(local.accessOf("t[0]"), "object i32 4") << local.accessOf("t[0]");

  // Through a pointer *to* the array, the extent is the type's and the provenance
  // is the pointer's -- the same asymmetry `p[i]` has.
  SemaFixture behind;
  behind.source("extern fn i32 g(p: *[4]i32);\nfn i32 main() { let p: *[4]i32 = null;\n"
                "              return (*p)[2]; }\n");
  ASSERT_TRUE(behind.build());
  EXPECT_EQ(behind.accessOf("(*p)[2]"), "foreign i32 4") << behind.accessOf("(*p)[2]");
}

TEST(ArrayTest, AConstPointerStillWritesThroughItsValue) {
  // The asymmetry `memory.md` decision 15 states and `arrays.md` decision 24
  // keeps: `const` protects the *name*. `p[i]` is `*(p + i)`, a write through a
  // pointer value, so it is allowed -- while `p = ...` is not.
  SemaFixture f;
  f.source("fn i32 main() { const p: *i32 = null; p[0] = 1; return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_FALSE(f.hasError("sema-assign-to-const")) << f.firstError().message;

  SemaFixture reassign;
  reassign.source("fn i32 main() { const p: *i32 = null; p = null; return 0; }\n");
  ASSERT_TRUE(reassign.build());
  EXPECT_TRUE(reassign.hasError("sema-assign-to-const"));
}

} // namespace
} // namespace minc::test
