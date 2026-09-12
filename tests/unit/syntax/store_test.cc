// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The keyed store: revision-keyed retention and the cross-file sharing that is
// the reason the node cache is owned here rather than by a single tree.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "lex/token_stream.h"
#include "support/mem/arena.h"
#include "support/span/file_id.h"
#include "syntax/store.h"

namespace minc::syntax {
namespace {

constexpr const char* kProgram = "fn i32 main() { let a: i32 = 1; return a; }\n";

TEST(TreeStoreTest, ParsesAndFindsByFile) {
  support::Arena arena;
  TreeStore store(arena);

  const lex::TokenStream stream = lex::TokenStream::lex(/*file=*/7u, kProgram);
  const SyntaxTree* tree = store.parse(stream, /*revision=*/0u);
  ASSERT_NE(tree, nullptr);
  EXPECT_TRUE(tree->validate());
  EXPECT_EQ(tree->file(), 7u);
  EXPECT_EQ(tree->revision(), 0u);

  EXPECT_EQ(store.size(), 1u);
  EXPECT_EQ(store.find(7u), tree);
  EXPECT_EQ(store.find(8u), nullptr);
}

// The point of one cache per store: the same bytes parsed as a different file
// reuse the nodes instead of duplicating them. Without that, every file in a
// multi-file command line pays for its own copy of every common subtree.
TEST(TreeStoreTest, IdenticalTreesShareNodesAcrossFiles) {
  support::Arena arena;
  TreeStore store(arena);

  const SyntaxTree* first = store.parse(lex::TokenStream::lex(/*file=*/1u, kProgram), 0u);
  ASSERT_NE(first, nullptr);
  const std::size_t afterFirst = store.cache().nodeCount();
  ASSERT_GT(afterFirst, 0u);

  const SyntaxTree* second = store.parse(lex::TokenStream::lex(/*file=*/2u, kProgram), 0u);
  ASSERT_NE(second, nullptr);
  EXPECT_TRUE(second->validate()) << "the shared tree must still be consistent";
  EXPECT_EQ(second->stats().nodeCount, first->stats().nodeCount);
  // Every node and token of the second tree was already in the cache, so no new
  // entry was created. This is the assertion the design's claim rests on.
  EXPECT_EQ(store.cache().nodeCount(), afterFirst);
  EXPECT_EQ(second->root().green(), first->root().green());
}

// A bumped revision invalidates rather than repairs, because byte offsets do
// not survive an edit.
TEST(TreeStoreTest, NewerRevisionReplacesTheOlderTree) {
  support::Arena arena;
  TreeStore store(arena);

  const SyntaxTree* first = store.parse(lex::TokenStream::lex(/*file=*/1u, kProgram), 0u);
  ASSERT_NE(first, nullptr);
  // Copied out before the re-parse, because `first` is dangling afterwards --
  // which is the documented rule that identity is the content, never a pointer.
  const std::size_t firstBytes = first->text().size();
  const std::size_t firstTokens = first->stats().tokenCount;

  const SyntaxTree* second =
      store.parse(lex::TokenStream::lex(/*file=*/1u, "fn i32 main() { return 0; }\n"), 1u);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(store.size(), 1u) << "the stale revision must not be retained";
  EXPECT_EQ(store.find(1u), second);
  EXPECT_EQ(second->revision(), 1u);
  // The tree really describes the new bytes, not the old ones at the same
  // address: the allocator is free to hand back the same node.
  EXPECT_EQ(second->text(), std::string_view("fn i32 main() { return 0; }\n"));
  EXPECT_LT(second->text().size(), firstBytes);
  EXPECT_LT(second->stats().tokenCount, firstTokens);
}

TEST(TreeStoreTest, DropAndClearReportWhatTheyDid) {
  support::Arena arena;
  TreeStore store(arena);

  ASSERT_NE(store.parse(lex::TokenStream::lex(/*file=*/1u, kProgram), 0u), nullptr);
  ASSERT_NE(store.parse(lex::TokenStream::lex(/*file=*/2u, kProgram), 0u), nullptr);
  EXPECT_EQ(store.size(), 2u);

  EXPECT_TRUE(store.drop(1u));
  EXPECT_FALSE(store.drop(1u)) << "dropping a file twice must not claim to have worked";
  EXPECT_EQ(store.find(1u), nullptr);
  EXPECT_NE(store.find(2u), nullptr);

  store.clear();
  EXPECT_EQ(store.size(), 0u);
  EXPECT_EQ(store.find(2u), nullptr);
  EXPECT_EQ(store.cache().nodeCount(), 0u);
}

} // namespace
} // namespace minc::syntax
