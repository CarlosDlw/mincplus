// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

#include "support/mem/arena.h"

namespace minc::support {
namespace {

struct Pair {
  int a = 0;
  int b = 0;
};

TEST(ArenaTest, CreateAndRelease) {
  Arena arena(256);
  Pair* p = arena.create<Pair>();
  ASSERT_NE(p, nullptr);
  p->a = 1;
  p->b = 2;
  EXPECT_EQ(p->a + p->b, 3);
  EXPECT_GT(arena.bytesUsed(), 0u);
  EXPECT_GE(arena.blockCount(), 1u);

  arena.release();
  EXPECT_EQ(arena.bytesUsed(), 0u);
  EXPECT_EQ(arena.blockCount(), 0u);
  EXPECT_EQ(arena.capacityBytes(), 0u);
}

TEST(ArenaTest, RewindKeepsBlocksForReuse) {
  Arena arena(256);
  for (int i = 0; i < 8; ++i) {
    ASSERT_NE(arena.create<Pair>(), nullptr);
  }
  const std::size_t blocks = arena.blockCount();
  const std::size_t capacity = arena.capacityBytes();
  ASSERT_GT(blocks, 0u);

  arena.rewind();
  EXPECT_EQ(arena.bytesUsed(), 0u);
  EXPECT_EQ(arena.blockCount(), blocks); // memory is retained, not re-acquired
  EXPECT_EQ(arena.capacityBytes(), capacity);

  for (int i = 0; i < 8; ++i) {
    ASSERT_NE(arena.create<Pair>(), nullptr);
  }
  EXPECT_EQ(arena.blockCount(), blocks); // reused without growing
}

TEST(ArenaTest, AlignmentRespected) {
  Arena arena(128);
  auto* c = static_cast<char*>(arena.allocate(1));
  auto* u64 =
      static_cast<std::uint64_t*>(arena.allocate(sizeof(std::uint64_t), alignof(std::uint64_t)));
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(u64) % alignof(std::uint64_t), 0u);
  (void)c;
}

TEST(ArenaTest, MaxAlignmentIsHonoured) {
  Arena arena(1024);
  (void)arena.allocate(1, 1); // force a non-zero offset
  void* p = arena.allocate(32, Arena::kMaxAlignment);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % Arena::kMaxAlignment, 0u);
}

TEST(ArenaTest, OverAlignedRequestIsRefused) {
  Arena arena(256);
  EXPECT_EQ(arena.allocate(16, Arena::kMaxAlignment * 2), nullptr);
  // create() propagates the refusal instead of handing back a mis-aligned T*.
  struct alignas(Arena::kMaxAlignment * 2) Wide {
    int value;
  };
  EXPECT_EQ(arena.create<Wide>(), nullptr);
}

TEST(ArenaTest, NonPowerOfTwoAlignmentRoundsUp) {
  Arena arena(256);
  auto* p = static_cast<char*>(arena.allocate(3, 3));
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 4u, 0u);
}

TEST(ArenaTest, LargeRequestGrowsBlock) {
  Arena arena(64);
  void* p = arena.allocate(4096);
  EXPECT_NE(p, nullptr);
  EXPECT_GE(arena.blockCount(), 1u);
}

TEST(ArenaTest, ImpossibleRequestReturnsNull) {
  Arena arena(64);
  EXPECT_EQ(arena.allocate(std::numeric_limits<std::size_t>::max()), nullptr);
  EXPECT_EQ(arena.bytesUsed(), 0u); // nothing was charged for a failed request
}

TEST(ArenaTest, ArrayDefaultConstructs) {
  Arena arena(256);
  int* items = arena.createArray<int>(4);
  ASSERT_NE(items, nullptr);
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(items[i], 0);
    items[i] = i;
  }
  EXPECT_EQ(items[3], 3);
  EXPECT_EQ(arena.createArray<int>(0), nullptr);
}

TEST(ArenaTest, ArraySizeOverflowIsRefused) {
  Arena arena(256);
  EXPECT_EQ(arena.createArray<std::uint64_t>(std::numeric_limits<std::size_t>::max()), nullptr);
}

} // namespace
} // namespace minc::support
