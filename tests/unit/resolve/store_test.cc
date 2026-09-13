// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

#include "resolve/resolve_fixture.h"
#include "resolve/store.h"

namespace minc::test {
namespace {

TEST(StoreTest, ABodyEditReusesTheResolution) {
  ResolveFixture before;
  before.source("fn i32 main()\n{\n  return 1;\n}\n");
  ResolveFixture after;
  after.source("fn i32 main()\n{\n  let x = 41;\n  return x + 1;\n}\n");
  ASSERT_TRUE(before.build());
  ASSERT_TRUE(after.build());

  resolve::ResolveStore store;
  const resolve::ResolveOutput* first = store.outputFor(0, 0, before.lowered(), before.symbols());
  ASSERT_NE(first, nullptr);
  const resolve::ResolveOutput* second = store.outputFor(0, 0, after.lowered(), after.symbols());

  // Same revision, same signatures: whatever changed in the file, it was not the
  // set of visible names, so the scopes still stand.
  EXPECT_EQ(store.stats().hits, 1u);
  EXPECT_EQ(store.stats().misses, 1u);
  EXPECT_EQ(first, second);
}

TEST(StoreTest, ASignatureEditResolvesAgain) {
  ResolveFixture before;
  before.source("fn i32 one() { return 0; }\n");
  ResolveFixture after;
  after.source("fn i32 oneMore() { return 0; }\n");
  ASSERT_TRUE(before.build());
  ASSERT_TRUE(after.build());

  resolve::ResolveStore store;
  (void)store.outputFor(0, 0, before.lowered(), before.symbols());
  (void)store.outputFor(0, 0, after.lowered(), after.symbols());

  EXPECT_EQ(store.stats().hits, 0u);
  EXPECT_EQ(store.stats().misses, 2u);
}

TEST(StoreTest, ANewRevisionResolvesAgain) {
  ResolveFixture f;
  f.source("fn i32 main() { return 0; }\n");
  ASSERT_TRUE(f.build());

  resolve::ResolveStore store;
  (void)store.outputFor(0, 0, f.lowered(), f.symbols());
  (void)store.outputFor(0, 1, f.lowered(), f.symbols());

  // Byte offsets do not survive an edit, so the revision alone is enough to make
  // the entry stale even when the text happens to be identical.
  EXPECT_EQ(store.stats().hits, 0u);
  EXPECT_EQ(store.stats().misses, 2u);
}

TEST(StoreTest, DropAndClear) {
  ResolveFixture f;
  f.source("fn i32 main() { return 0; }\n");
  ASSERT_TRUE(f.build());

  resolve::ResolveStore store;
  (void)store.outputFor(0, 0, f.lowered(), f.symbols());
  EXPECT_EQ(store.size(), 1u);
  EXPECT_NE(store.find(0), nullptr);

  EXPECT_TRUE(store.drop(0));
  EXPECT_FALSE(store.drop(0)); // dropping nothing is not success
  EXPECT_EQ(store.find(0), nullptr);
  EXPECT_TRUE(store.size() == 0);

  (void)store.outputFor(0, 0, f.lowered(), f.symbols());
  store.clear();
  EXPECT_EQ(store.size(), 0u);
  EXPECT_EQ(store.stats().hits, 0u);
}

TEST(StoreTest, DifferentFilesDoNotEvictEachOther) {
  ResolveFixture first;
  first.source("fn i32 one() { return 0; }\n");
  ResolveFixture second;
  second.source("fn i32 another() { return 0; }\n");
  ASSERT_TRUE(first.build());
  ASSERT_TRUE(second.build());

  resolve::ResolveStore store;
  const resolve::ResolveOutput* a = store.outputFor(0, 0, first.lowered(), first.symbols());
  const resolve::ResolveOutput* b = store.outputFor(1, 0, second.lowered(), second.symbols());

  EXPECT_EQ(store.size(), 2u);
  // The pointers stay valid as more files are added: the entries live in the
  // map's nodes, so adding one never moves another.
  EXPECT_NE(a, nullptr);
  EXPECT_NE(b, nullptr);
  EXPECT_NE(store.find(0), nullptr);
  EXPECT_NE(store.find(1), nullptr);
}

} // namespace
} // namespace minc::test
