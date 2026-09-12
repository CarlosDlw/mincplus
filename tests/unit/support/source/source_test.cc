// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "support/source/file_io.h"
#include "support/source/source_file.h"
#include "support/source/source_manager.h"

namespace minc::support {
namespace {

// "/tmp" does not exist on Windows, so tests must ask the platform.
[[nodiscard]] std::string tempPath(const std::string& name) {
  return (std::filesystem::temp_directory_path() / name).string();
}

void writeFile(const std::string& path, std::string_view bytes) {
  std::ofstream out(path, std::ios::binary);
  ASSERT_TRUE(out.good()) << path;
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] std::string utf16Sample() {
  std::string text = "\xFF\xFE"; // UTF-16 little-endian BOM
  text.push_back('f');
  text.push_back('\0');
  return text;
}

TEST(SourceFileTest, SliceClamps) {
  SourceManager sm;
  const FileId id = sm.addFile("a.mx", "hello").value();
  const SourceFile* f = sm.find(id);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->slice(1, 4), "ell");
  EXPECT_EQ(f->slice(0, 100), "hello");
  EXPECT_EQ(f->slice(4, 1), "");
}

TEST(SourceFileTest, SliceSpanValidatesFile) {
  SourceManager sm;
  const FileId a = sm.addFile("a.mx", "hello world").value();
  const FileId b = sm.addFile("b.mx", "other").value();
  const SourceFile* f = sm.find(a);
  ASSERT_NE(f, nullptr);
  auto ok = f->sliceSpan(Span(a, 6, 11));
  ASSERT_TRUE(ok.has_value());
  EXPECT_EQ(*ok, "world");
  EXPECT_FALSE(f->sliceSpan(Span(b, 0, 2)).has_value());
  EXPECT_FALSE(f->sliceSpan(Span{}).has_value());
}

// Regression: the line table used to be built from a moved-from string, which
// left every multi-line file looking like a single line.
TEST(SourceFileTest, TracksMultipleLines) {
  SourceManager sm;
  const FileId id = sm.addFile("m.mx", "int a;\nint b;\nint c;").value();
  const SourceFile* f = sm.find(id);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->lineCount(), 3u);
  EXPECT_EQ(f->lookup(7).line, 2u);
  EXPECT_EQ(f->lookup(7).col, 1u);
  EXPECT_EQ(f->lookup(14).line, 3u);
  ASSERT_TRUE(f->lineText(2).has_value());
  EXPECT_EQ(*f->lineText(2), "int b;");
}

TEST(SourceManagerTest, IdsAreStable) {
  SourceManager sm;
  const FileId a = sm.addFile("a.mx", "x").value();
  const FileId b = sm.addFile("b.mx", "y").value();
  EXPECT_EQ(a, 0u);
  EXPECT_EQ(b, 1u);
  EXPECT_EQ(sm.fileCount(), 2u);
  EXPECT_NE(sm.find(a), nullptr);
  EXPECT_EQ(sm.find(99), nullptr);
}

TEST(SourceManagerTest, StripsUtf8Bom) {
  SourceManager sm;
  std::string text = "\xEF\xBB\xBF"; // UTF-8 BOM
  text += "fn int main() {}";
  const FileId id = sm.addFile("bom.mx", text).value();
  const SourceFile* f = sm.find(id);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->text, "fn int main() {}");
}

TEST(SourceManagerTest, RejectsUtf16) {
  SourceManager sm;
  auto result = sm.addFile("wide.mx", utf16Sample());
  ASSERT_FALSE(result.hasValue());
  EXPECT_NE(result.error().find("UTF-16"), std::string::npos);
  EXPECT_EQ(sm.fileCount(), 0u); // a rejected file is not stored
}

TEST(SourceManagerTest, RejectsInvalidUtf8) {
  SourceManager sm;
  std::string truncated = "ab";
  truncated.push_back(static_cast<char>(0xC3));
  auto result = sm.addFile("bad.mx", truncated);
  ASSERT_FALSE(result.hasValue());
  EXPECT_NE(result.error().find("not valid UTF-8"), std::string::npos);
  EXPECT_NE(result.error().find("offset 2"), std::string::npos);
}

TEST(SourceManagerTest, RejectsNulByte) {
  SourceManager sm;
  std::string binary("a\0b", 3);
  auto result = sm.addFile("bin.mx", binary);
  ASSERT_FALSE(result.hasValue());
  EXPECT_NE(result.error().find("NUL"), std::string::npos);
}

TEST(SourceManagerTest, AcceptsMultibyteText) {
  SourceManager sm;
  const FileId id = sm.addFile("e.mx", "caf\xC3\xA9").value();
  const SourceFile* f = sm.find(id);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->text, "caf\xC3\xA9");
}

TEST(SourceManagerTest, AcceptsCrlfText) {
  SourceManager sm;
  const FileId id = sm.addFile("w.mx", "a\r\nb\r\n").value();
  const SourceFile* f = sm.find(id);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->lineCount(), 3u);
}

TEST(SourceManagerTest, LoadMissingFileErrors) {
  SourceManager sm;
  auto r = sm.loadFromDisk("/nonexistent/mincplus-test-file.mx");
  EXPECT_FALSE(r.hasValue());
  EXPECT_NE(r.error().find("cannot"), std::string::npos);
}

TEST(SourceManagerTest, LoadRoundTrip) {
  const std::string path = tempPath("mincplus_source_test.mx");
  writeFile(path, "fn int main() { return 0; }");

  SourceManager sm;
  auto r = sm.loadFromDisk(path);
  ASSERT_TRUE(r.hasValue());
  const SourceFile* f = sm.find(r.value());
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->text, "fn int main() { return 0; }");
  std::remove(path.c_str());
}

TEST(SourceManagerTest, LoadDirectoryErrors) {
  SourceManager sm;
  auto r = sm.loadFromDisk(std::filesystem::temp_directory_path().string());
  EXPECT_FALSE(r.hasValue());
  EXPECT_NE(r.error().find("directory"), std::string::npos);
}

TEST(FileIoTest, MissingFileErrors) {
  auto r = readFileBytes("/nonexistent/mincplus-test-file.mx");
  EXPECT_FALSE(r.hasValue());
}

TEST(FileIoTest, EmptyPathErrors) {
  auto r = readFileBytes("");
  EXPECT_FALSE(r.hasValue());
}

TEST(FileIoTest, ReadsExactBytes) {
  const std::string path = tempPath("mincplus_io_test.bin");
  std::string payload("a\0b\xFF", 4);
  writeFile(path, payload);

  auto r = readFileBytes(path);
  ASSERT_TRUE(r.hasValue());
  EXPECT_EQ(r.value(), payload);
  std::remove(path.c_str());
}

TEST(SourceManagerTest, ReplaceTextBumpsRevisionAndRebuildsLines) {
  SourceManager sm;
  const FileId id = sm.addFile("m.mx", "a\n").value();
  const SourceFile* file = sm.find(id);
  ASSERT_NE(file, nullptr);
  EXPECT_EQ(file->revision, 0u);
  EXPECT_EQ(file->lineCount(), 2u);

  ASSERT_TRUE(sm.replaceText(id, "x\ny\nz").hasValue());
  file = sm.find(id);
  ASSERT_NE(file, nullptr);
  EXPECT_EQ(file->revision, 1u);
  EXPECT_EQ(file->lineCount(), 3u);
  EXPECT_EQ(file->text, "x\ny\nz");
}

TEST(SourceManagerTest, ReplaceTextRejectsBadInputAndKeepsTheOldText) {
  SourceManager sm;
  const FileId id = sm.addFile("m.mx", "good").value();
  std::string withNul("no\0way", 6);

  EXPECT_FALSE(sm.replaceText(id, withNul).hasValue());
  const SourceFile* file = sm.find(id);
  ASSERT_NE(file, nullptr);
  EXPECT_EQ(file->text, "good");
  EXPECT_EQ(file->revision, 0u);
}

TEST(SourceManagerTest, ReplaceUnknownIdFails) {
  SourceManager sm;
  EXPECT_FALSE(sm.replaceText(7, "x").hasValue());
}

} // namespace
} // namespace minc::support
