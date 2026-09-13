// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Every `.mx` file in `examples/` must type-check cleanly.
//
// The examples are the language's de facto specification for the surface that
// exists, so a checker change that rejects one must fail here rather than be
// noticed later by hand. This is the last stage in the chain the lexer's, the
// parser's and the resolver's example suites start: the same files, held to the
// strictest of the guarantees.
//
// The whole front end runs for each file -- preprocess, parse, lower, validate,
// resolve, check -- with no lowering of any option, so the examples are checked
// against the shipped defaults.
#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "ast/lower.h"
#include "ast/validate.h"
#include "lex/token_stream.h"
#include "parse/parse_error.h"
#include "pp/preprocessor.h"
#include "resolve/resolve.h"
#include "sema/sema.h"
#include "sema/sema_error.h"
#include "sema/type_store.h"
#include "sema/typed_ast.h"
#include "support/expected/fallible.h"
#include "support/session/session.h"
#include "support/source/file_io.h"
#include "support/source/source_file.h"
#include "syntax/store.h"
#include "syntax/tree.h"
#include "tests/examples_dir.h"

namespace minc::test {
namespace {

// Sorted so the test is deterministic across platforms and file systems.
[[nodiscard]] std::vector<std::filesystem::path> exampleFiles() {
  std::vector<std::filesystem::path> files;
  std::error_code ec;
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(kExamplesDir, ec)) {
    if (entry.is_regular_file(ec) && entry.path().extension() == ".mx") {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

TEST(ExamplesSemaTest, EveryExampleTypeChecksWithoutErrors) {
  const std::vector<std::filesystem::path> files = exampleFiles();
  ASSERT_GE(files.size(), 5u) << "examples/ is missing files";

  for (const std::filesystem::path& path : files) {
    const std::string name = path.filename().string();
    const support::Fallible<std::string> text = support::readFileBytes(path.string());
    ASSERT_TRUE(text.hasValue()) << name << ": " << text.error();

    // Loaded through a `Session`, so the example is held to the same boundary as
    // real input: size limit, BOM, NUL, and UTF-8 validity.
    support::Session session;
    const auto id = session.addFile(name, text.value());
    ASSERT_TRUE(id.hasValue()) << name << ": " << id.error();
    const support::SourceFile* file = session.sources().find(id.value());
    ASSERT_NE(file, nullptr) << name;

    pp::Preprocessor preprocessor(session, pp::PPOptions{});
    const pp::PPResult result = preprocessor.run(file->id);
    for (const pp::PPError& error : result.errors) {
      ADD_FAILURE() << name << ": pp " << pp::toString(error.code) << ": " << error.message;
    }

    const lex::TokenStream stream = pp::preprocessedStream(result);
    syntax::TreeStore trees(session.arena());
    const syntax::SyntaxTree* tree = trees.parse(stream, file->revision);
    ASSERT_NE(tree, nullptr) << name;
    for (const parse::ParseError& error : tree->errors()) {
      ADD_FAILURE() << name << ": parse " << error.codeName() << ": " << error.message;
    }

    const ast::OriginTable origins{&stream};
    ast::LowerOutput lowered = ast::lowerFile(*tree, session.symbols(), origins);
    for (const ast::AstError& error : lowered.errors) {
      ADD_FAILURE() << name << ": lower " << ast::toString(error.code) << ": " << error.message;
    }
    for (const ast::AstError& error : ast::validate(lowered.file)) {
      ADD_FAILURE() << name << ": validate " << ast::toString(error.code) << ": " << error.message;
    }

    const resolve::ResolveOutput resolved =
        resolve::resolveUnit(lowered.file, session.symbols(), resolve::ResolveOptions{});
    for (const resolve::ResolveError& error : resolved.errors) {
      ADD_FAILURE() << name << ": resolve " << resolve::toString(error.code) << ": "
                    << error.message;
    }

    sema::TypeStore types;
    const sema::SemaOutput checked =
        sema::checkUnit(lowered.file, resolved.map, session.symbols(), types, sema::SemaOptions{});
    for (const sema::SemaError& error : checked.errors) {
      ADD_FAILURE() << name << ": sema " << sema::toString(error.code) << ": " << error.message;
    }
    // Reported individually *and* counted, so a new error cannot hide behind a
    // summary line.
    EXPECT_EQ(checked.errors.size(), 0u) << name;

    // And the artifact the next stage consumes is complete for every expression.
    for (std::uint32_t i = 0; i < lowered.file.nodeCount(); ++i) {
      EXPECT_TRUE(checked.typed.typeOf(ast::AstId{i}).valid())
          << name << ": node " << i << " has no type";
    }
  }
}

} // namespace
} // namespace minc::test
