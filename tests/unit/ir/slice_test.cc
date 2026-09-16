// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// `[]T` in the module: the descriptor, the accesses through it, and how it
// crosses a call.
//
// The properties asserted here are the ones a *machine* would otherwise be the
// only witness to: the descriptor is two words and not a pointer, a slice crosses
// a call by value rather than as a copy, and an element access through one
// reaches the storage the view names. They are module assertions rather than
// execution tests because what is under test is the shape this stage emits -- the
// answers are `driver/build_command_test.cc`'s, where the program is linked and
// run.
#include <gtest/gtest.h>

#include <string>

#include "ir/ir_fixture.h"

namespace minc::test {
namespace {

TEST(SliceIrTest, TheDescriptorIsTwoWordsAndNotAnArray) {
  // `[]i32` in a signature is `{ ptr, i64 }`: the length is the *index* width,
  // which is the width a `getelementptr` index is made of and therefore the width
  // an index is compared against. A descriptor emitted as a pointer -- the habit
  // from C's `char *` -- would carry no length, and an emitted as an array would
  // carry a length that is a lie for every view but the whole object.
  IrFixture f;
  f.source("fn i32 first(s: []i32) { return s[0]; }\n"
           "fn i32 main() { let a: [4]i32 = [1, 2, 3, 4]; return first(a[..]); }\n");
  ASSERT_TRUE(f.build());
  ASSERT_TRUE(f.moduleBuilt());
  const std::string module = f.module();
  EXPECT_NE(module.find("define i32 @first({ ptr, i64 }"), std::string::npos) << module;
  EXPECT_EQ(f.violations(), 0u);
}

TEST(SliceIrTest, ASliceCrossesACallByValue) {
  // The one place the two aggregate kinds part company. An *array* is passed as a
  // pointer to a copy the caller makes and returned through an `sret`
  // destination, because it can be a megabyte. A descriptor is two words and
  // crosses as itself: the callee gets the value, so there is no `alloca` of the
  // caller's making, no `byval`, and the return is a value and not a `void` with
  // a leading pointer.
  IrFixture f;
  f.source("fn []i32 take(s: []i32) { return s; }\n"
           "fn i32 main() { let a: [4]i32 = [1, 2, 3, 4]; let s: []i32 = take(a[..]);\n"
           "  return s[0]; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_TRUE(f.moduleBuilt());
  const std::string module = f.module();
  EXPECT_NE(module.find("define { ptr, i64 } @take({ ptr, i64 }"), std::string::npos) << module;
  // `sret` is the array's shape, and this signature must not have it.
  EXPECT_EQ(module.find("sret"), std::string::npos) << module;
  // The copy an array argument makes is `argumentCopy`, and a slice must not
  // become one: the only `alloca` for the parameter is the binding's own slot,
  // which is what `&s` needs.
  EXPECT_NE(module.find("%s = alloca { ptr, i64 }"), std::string::npos) << module;
  EXPECT_EQ(f.violations(), 0u);
}

TEST(SliceIrTest, AnElementAccessReachesTheStorageTheViewNames) {
  // `s[i]` is a load through the descriptor's first word, one `getelementptr` and
  // one `load` -- and the `getelementptr` is *plain*: the view's length is a value
  // this stage does not compare the index against, and `inbounds` would be a
  // promise the language never asked the program to keep.
  IrFixture f;
  f.source("fn i32 at(s: []i32, i: i32) { return s[i]; }\n"
           "fn i32 main() { let a: [4]i32 = [1, 2, 3, 4]; return at(a[..], 1); }\n");
  ASSERT_TRUE(f.build());
  ASSERT_TRUE(f.moduleBuilt());
  const std::string module = f.module();
  EXPECT_NE(module.find("extractvalue { ptr, i64 }"), std::string::npos) << module;
  EXPECT_NE(module.find("getelementptr i32, ptr"), std::string::npos) << module;
  EXPECT_EQ(module.find("getelementptr inbounds"), std::string::npos) << module;
  EXPECT_EQ(f.violations(), 0u);
}

TEST(SliceIrTest, TheLengthIsTheDifferenceAndNotALoad) {
  // The four forms, in one module: a written end is subtracted, a missing one is
  // the extent the base has -- the array's count as a constant, the slice's own
  // length as the descriptor's second word. `a[..]` of an array is therefore *no
  // length computation at all*, and `s[2..]` is a subtraction.
  IrFixture f;
  f.source("fn i32 whole(a: [4]i32) { let s: []i32 = a[..]; return s[0]; }\n"
           "fn i32 from(s: []i32) { let t: []i32 = s[2..]; return t[0]; }\n"
           "fn i32 main() { let a: [4]i32 = [1, 2, 3, 4]; return whole(a) + from(a[..]); }\n");
  ASSERT_TRUE(f.build());
  ASSERT_TRUE(f.moduleBuilt());
  const std::string module = f.module();
  // The whole object: the length is the constant 4, and no word is read for it.
  EXPECT_NE(module.find(", i64 4, 1"), std::string::npos) << module;
  // ... and `a[..]`'s begin is a `getelementptr` with a zero offset, which LLVM
  // does not need a subtraction for.
  EXPECT_NE(module.find("getelementptr [4 x i32], ptr %a, i64 0, i64 0"), std::string::npos)
      << module;
  // From a slice: the second word, minus the begin.
  EXPECT_NE(module.find("sub i64"), std::string::npos) << module;
  EXPECT_EQ(f.violations(), 0u);
}

TEST(SliceIrTest, AViewOfAViewWalksFromTheViewsOwnStart) {
  // The footgun the Swift `ArraySlice` shipped with: an index into a view is an
  // index into *the view*, so `s[1..3][0]` is `s[1]` and not `s[0]`. Emitted as
  // one `getelementptr` from the inner view's pointer, which is what makes the
  // two different numbers.
  IrFixture f;
  f.source("fn i32 second(s: []i32) { let t: []i32 = s[1..3]; return t[0]; }\n"
           "fn i32 main() { let a: [4]i32 = [1, 2, 3, 4]; return second(a[..]); }\n");
  ASSERT_TRUE(f.build());
  ASSERT_TRUE(f.moduleBuilt());
  const std::string module = f.module();
  // Two address computations, and the second one starts from the first one's
  // result: the view's data walked by 1, then *that* walked by 0 -- which is the
  // difference between `s[1]` and `s[0]`.
  EXPECT_NE(module.find("getelementptr i32, ptr %slice.data, i64 1"), std::string::npos) << module;
  EXPECT_NE(module.find("getelementptr i32, ptr %slice.ptr, i64 0"), std::string::npos) << module;
  EXPECT_EQ(f.violations(), 0u);
}

TEST(SliceIrTest, TheDebugTypeShowsBothWords) {
  // A debugger that showed only the pointer -- the C habit -- would hide the one
  // number a slice adds. The record has the two members, named as the record's
  // own vocabulary names them.
  IrFixture f;
  f.debugInfo();
  f.source("fn i32 first(s: []i32) { return s[0]; }\n"
           "fn i32 main() { let a: [4]i32 = [1, 2, 3, 4]; return first(a[..]); }\n");
  ASSERT_TRUE(f.build());
  ASSERT_TRUE(f.moduleBuilt());
  const std::string module = f.module();
  EXPECT_NE(module.find("!DICompositeType(tag: DW_TAG_structure_type"), std::string::npos)
      << module;
  EXPECT_NE(module.find("\"ptr\""), std::string::npos) << module;
  EXPECT_NE(module.find("\"len\""), std::string::npos) << module;
}

TEST(SliceIrTest, ASliceAccessCarriesNoExtentAndTheScanSaysSo) {
  // The access record's extent is what the checked build would bound-check
  // against, and a view's length is a *value*: this stage cannot see it, so the
  // record says "not known" and no guard is invented. The property asserted here
  // is the honest one -- an index through a slice produces no promise, and the
  // module still has no assumption violations.
  IrFixture f;
  f.source("fn i32 at(s: []i32, i: i32) { return s[i]; }\n"
           "fn i32 main() { let a: [4]i32 = [1, 2, 3, 4]; return at(a[..], 2); }\n");
  ASSERT_TRUE(f.build());
  ASSERT_TRUE(f.moduleBuilt());
  ASSERT_EQ(f.typed().typed.accesses().size(), 1u);
  const sema::AccessObligation& access = f.typed().typed.accesses().front();
  // The element, and the extent the checked build would use -- which for a view
  // is "not known" and not the descriptor's length: the length is a value, and
  // pretending otherwise would be a guard against a number this stage guessed.
  EXPECT_EQ(access.type, sema::kTypeI32);
  EXPECT_EQ(access.extent, 0u);
  EXPECT_EQ(f.violations(), 0u);
}

} // namespace
} // namespace minc::test
