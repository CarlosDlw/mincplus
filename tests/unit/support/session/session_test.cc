// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "support/session/session.h"
#include "support/source/source_file.h"

namespace minc::support {
namespace {

TEST(SessionTest, OwnsCoherentState) {
  Session session;
  const FileId id = session.addFile("a.mx", "fn i32 main() {}").value();
  const SymId symbol = session.symbols().intern("main");
  session.diags().error(Span(id, 3, 6), "boom");

  EXPECT_EQ(session.sources().fileCount(), 1u);
  EXPECT_EQ(session.symbols().lookup(symbol), "main");
  EXPECT_EQ(session.diags().size(), 1u);
  EXPECT_TRUE(session.diags().hasErrors());
  EXPECT_NE(session.sources().find(id), nullptr);
}

TEST(SessionTest, RejectedSourceIsNotStoredAndIsNotADiagnostic) {
  Session session;
  std::string truncated = "ab";
  truncated.push_back(static_cast<char>(0xC3));

  EXPECT_FALSE(session.addFile("bad.mx", truncated).hasValue());
  EXPECT_TRUE(session.sources().empty());
  // The source boundary reports failures by return value, not through the
  // diagnostic bag, so a rejected file produces no diagnostics.
  EXPECT_EQ(session.diags().size(), 0u);
}

TEST(SessionTest, UpdateFileKeepsIdAndBumpsRevision) {
  Session session;
  const FileId id = session.addFile("m.mx", "one\ntwo\n").value();
  ASSERT_TRUE(session.revisionOf(id).has_value());
  const std::uint32_t before = *session.revisionOf(id);

  ASSERT_TRUE(session.updateFile(id, "alpha\nbeta\ngamma\n").hasValue());

  const SourceFile* file = session.sources().find(id);
  ASSERT_NE(file, nullptr);
  EXPECT_EQ(file->id, id);
  EXPECT_EQ(file->text, "alpha\nbeta\ngamma\n");
  EXPECT_EQ(file->lineCount(), 4u); // the line table was rebuilt
  EXPECT_EQ(*session.revisionOf(id), before + 1);
}

TEST(SessionTest, FailedUpdateLeavesTheFileUntouched) {
  Session session;
  const FileId id = session.addFile("m.mx", "keep\n").value();
  const std::uint32_t before = *session.revisionOf(id);

  std::string withNul("x\0y", 3);
  EXPECT_FALSE(session.updateFile(id, withNul).hasValue());

  const SourceFile* file = session.sources().find(id);
  ASSERT_NE(file, nullptr);
  EXPECT_EQ(file->text, "keep\n");
  EXPECT_EQ(file->revision, before);
}

TEST(SessionTest, UnknownFileHasNoRevision) {
  Session session;
  EXPECT_FALSE(session.updateFile(42, "x").hasValue());
  EXPECT_FALSE(session.revisionOf(42).has_value());
}

TEST(SessionTest, ArenaIsReachableThroughTheSession) {
  Session session;
  int* value = session.arena().create<int>(7);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(*value, 7);
}

} // namespace
} // namespace minc::support
