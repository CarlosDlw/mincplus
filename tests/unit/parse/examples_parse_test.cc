// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Every `.mx` file in `examples/` must parse cleanly.
//
// The examples are the language's de facto specification for the surface that
// exists, so a grammar change that breaks one must fail here rather than be
// noticed later by hand. This is the tree-level counterpart of the lexer's
// example suite: the same files, held to the stricter guarantee.
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "lex/token_stream.h"
#include "parse/parse_error.h"
#include "support/expected/fallible.h"
#include "support/mem/arena.h"
#include "support/source/source_file.h"
#include "support/source/source_manager.h"
#include "syntax/ast.h"
#include "syntax/builder.h"
#include "syntax/tree.h"
#include "tests/examples_dir.h"

namespace minc::syntax {
namespace {

// Sorted so the test is deterministic across platforms and file systems.
std::vector<std::filesystem::path> exampleFiles() {
  std::vector<std::filesystem::path> files;
  std::error_code ec;
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(test::kExamplesDir, ec)) {
    if (entry.is_regular_file(ec) && entry.path().extension() == ".mx") {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

std::string nameOf(const std::filesystem::path& path) {
  return path.filename().string();
}

TEST(ExamplesParseTest, EveryExampleParsesWithoutErrors) {
  const std::vector<std::filesystem::path> files = exampleFiles();
  ASSERT_GE(files.size(), 5u) << "examples/ is missing files";

  for (const std::filesystem::path& path : files) {
    const std::string name = nameOf(path);

    // Loaded through SourceManager, so the example is held to the same boundary
    // as real input: size limit, BOM, NUL, and UTF-8 validity.
    support::SourceManager sources;
    const support::Fallible<support::FileId> id = sources.loadFromDisk(path.string());
    ASSERT_TRUE(id.hasValue()) << name << ": " << id.error();
    const support::SourceFile* file = sources.find(id.value());
    ASSERT_NE(file, nullptr) << name;

    const lex::TokenStream stream = lex::TokenStream::lex(file->id, file->text);
    support::Arena arena;
    GreenCache cache(arena);
    const std::optional<SyntaxTree> tree = buildSyntaxTree(cache, stream, file->revision);
    ASSERT_TRUE(tree.has_value()) << name;

    // Report each error individually: a bare count would hide which one went
    // wrong and where.
    for (const parse::ParseError& error : tree->errors()) {
      ADD_FAILURE() << name << ": " << error.codeName() << ": " << error.message;
    }
    EXPECT_EQ(tree->errors().size(), 0u) << name;
    EXPECT_FALSE(tree->stats().bailedOut) << name;
    EXPECT_TRUE(tree->stats().lossless) << name;

    // The tree must still be a faithful copy of the source, and a tree a tool
    // can walk.
    EXPECT_TRUE(tree->validate()) << name;
    EXPECT_EQ(tree->reconstruct(), file->text) << name;

    // A file that is only declarations must still produce a program shape, so
    // a regression that parses nothing fails here rather than passing quietly.
    const SyntaxNode root = tree->root();
    EXPECT_EQ(root.kind(), parse::SyntaxKind::File) << name;
    EXPECT_FALSE(root.nodeChildren().empty()) << name;
  }
}

TEST(ExamplesParseTest, EveryExampleDeclaresMain) {
  for (const std::filesystem::path& path : exampleFiles()) {
    const std::string name = nameOf(path);

    support::SourceManager sources;
    const support::Fallible<support::FileId> id = sources.loadFromDisk(path.string());
    ASSERT_TRUE(id.hasValue()) << name << ": " << id.error();
    const support::SourceFile* file = sources.find(id.value());
    ASSERT_NE(file, nullptr) << name;

    const lex::TokenStream stream = lex::TokenStream::lex(file->id, file->text);
    support::Arena arena;
    GreenCache cache(arena);
    const std::optional<SyntaxTree> tree = buildSyntaxTree(cache, stream, file->revision);
    ASSERT_TRUE(tree.has_value()) << name;

    bool hasMain = false;
    for (const SyntaxNode& fn : tree->root().nodeChildren()) {
      if (fn.kind() != parse::SyntaxKind::FnDecl) {
        continue;
      }
      const std::optional<SyntaxNode> fnName = fn.childOfKind(parse::SyntaxKind::Name);
      if (fnName.has_value() && identifierText(*fnName) == "main") {
        hasMain = true;
      }
    }
    EXPECT_TRUE(hasMain) << name << " has no `main` function";
  }
}

} // namespace
} // namespace minc::syntax
