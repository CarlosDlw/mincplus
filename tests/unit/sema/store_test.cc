// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The compilation's central checker: one type store, one answer per revision,
// and the properties a later stage or the language server relies on.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

#include "sema/sema.h"
#include "sema/sema_fixture.h"
#include "sema/type.h"

namespace minc::test {
namespace {

TEST(SemaStoreTest, ARevisionIsTheWholeKey) {
  SemaFixture f;
  f.source("fn i32 main() { let x: i32 = 1; return x; }\n");
  ASSERT_TRUE(f.build());

  sema::Context context;
  const sema::SemaOutput* first = context.check(0, 0, f.lowered(), f.map(), f.symbols());
  ASSERT_NE(first, nullptr);
  const sema::SemaOutput* again = context.check(0, 0, f.lowered(), f.map(), f.symbols());

  EXPECT_EQ(context.stats().hits, 1u);
  EXPECT_EQ(context.stats().misses, 1u);
  // The second answer is the *same* object, not a recomputation that happens to
  // agree -- which is what makes "ask again for free" true.
  EXPECT_EQ(first, again);

  (void)context.check(0, 1, f.lowered(), f.map(), f.symbols());
  // A new revision is a new answer, even for identical text: byte offsets do not
  // survive an edit, and a type comes out of a body.
  EXPECT_EQ(context.stats().hits, 1u);
  EXPECT_EQ(context.stats().misses, 2u);
}

TEST(SemaStoreTest, AStructurallyIdenticalUnitStillRechecksOnANewRevision) {
  SemaFixture before;
  before.source("fn i32 main() { return 1; }\n");
  SemaFixture after;
  after.source("fn i32 main() { let x: i64 = 1; return 0; }\n");
  ASSERT_TRUE(before.build());
  ASSERT_TRUE(after.build());

  sema::Context context(sema::targetInfo(sema::kDefaultTarget));
  (void)context.check(0, 0, before.lowered(), before.map(), before.symbols());
  (void)context.check(0, 1, after.lowered(), after.map(), after.symbols());

  // Unlike resolution, there is no signature-level shortcut to take: a body edit
  // can change a type, so the item tree is deliberately not consulted.
  EXPECT_EQ(context.stats().hits, 0u);
  EXPECT_EQ(context.stats().misses, 2u);
}

TEST(SemaStoreTest, TheTypeStoreIsTheCompilations) {
  SemaFixture first;
  first.source("fn i32 one() { return 0; }\n");
  SemaFixture second;
  second.source("fn i64 two() { return 0; }\n");
  ASSERT_TRUE(first.build());
  ASSERT_TRUE(second.build());

  sema::Context context;
  const sema::SemaOutput* a = context.check(0, 0, first.lowered(), first.map(), first.symbols());
  const sema::SemaOutput* b = context.check(1, 0, second.lowered(), second.map(), second.symbols());
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);

  // One store for the compilation: `i32` written in the first unit is the same
  // `TypeId` as `i32` written in the second, which is the whole reason the store
  // is not per file.
  EXPECT_EQ(a->typed.functionTable.front().returnType, sema::kTypeI32);
  EXPECT_EQ(b->typed.functionTable.front().returnType, sema::kTypeI64);
  EXPECT_EQ(context.types().spelling(a->typed.functionTable.front().returnType), "i32");
  // Two files, two entries, and adding the second did not move the first.
  EXPECT_EQ(context.size(), 2u);
  EXPECT_EQ(context.find(0), a);
}

TEST(SemaStoreTest, DropAndClear) {
  SemaFixture f;
  f.source("fn i32 main() { return 0; }\n");
  ASSERT_TRUE(f.build());

  sema::Context context;
  (void)context.check(0, 0, f.lowered(), f.map(), f.symbols());
  EXPECT_EQ(context.size(), 1u);
  EXPECT_NE(context.find(0), nullptr);

  EXPECT_TRUE(context.drop(0));
  EXPECT_FALSE(context.drop(0)); // dropping nothing is not success
  EXPECT_EQ(context.find(0), nullptr);

  (void)context.check(0, 0, f.lowered(), f.map(), f.symbols());
  context.clear();
  EXPECT_EQ(context.size(), 0u);
}

TEST(SemaStoreTest, TheTargetBelongsToTheStore) {
  SemaFixture f;
  f.source("fn i32 main() { let x: long = 1; return 0; }\n");
  ASSERT_TRUE(f.build());

  sema::Context sysv(sema::targetInfo(sema::Target::SystemVAmd64));
  const sema::SemaOutput* a = sysv.check(0, 0, f.lowered(), f.map(), f.symbols());
  ASSERT_NE(a, nullptr);
  // The context's store decided `long`, and it is the one the answer indexes.
  EXPECT_EQ(sysv.types().target().longBits, 64u);

  sema::Context windows(sema::targetInfo(sema::Target::WindowsX64));
  (void)windows.check(0, 0, f.lowered(), f.map(), f.symbols());
  EXPECT_EQ(windows.types().target().longBits, 32u);
}

TEST(SemaStoreTest, LimitsAreStillEnforcedThroughTheContext) {
  SemaFixture f;
  f.source("fn i32 main() { let x: i32 = 1; return x; }\n");
  ASSERT_TRUE(f.build());

  sema::Context context;
  sema::SemaOptions options;
  options.maxTypes = 0;
  const sema::SemaOutput* answer = context.check(0, 0, f.lowered(), f.map(), f.symbols(), options);
  ASSERT_NE(answer, nullptr);
  ASSERT_FALSE(answer->errors.empty());
  EXPECT_EQ(sema::toString(answer->errors.front().code), "sema-limit-types");
  EXPECT_EQ(context.types().maxTypes(), 0u); // lowered, and visible
}

} // namespace
} // namespace minc::test
