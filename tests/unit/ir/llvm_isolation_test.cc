// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The isolation rule, as a test rather than as a comment.
//
// `src/ir` (and `src/backend`, when it exists) are the only targets that may
// include `llvm/*`. That rule is what lets every stage above this one be built
// and tested with no LLVM in sight, and a rule that lives only in a comment is a
// rule that gets broken by the first person who wants a `DataLayout` in `sema`.
//
// So it is checked mechanically, over the source tree, where a violation fails
// CI instead of review. The check reads the *text*, because that is what the
// compiler sees: an include is an include whether or not it is reached.
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "tests/examples_dir.h"

namespace minc {
namespace {

// The directories allowed to see LLVM, relative to whichever root the walk
// starts at. The list is the *policy* rather than a census of what is there: a
// new file under `ir/` or `backend/` is allowed by construction, and a new
// directory has to say why it needs LLVM.
[[nodiscard]] bool mayIncludeLlvm(const std::filesystem::path& relative) {
  const std::string text = relative.generic_string();
  return text.rfind("ir/", 0) == 0 || text.rfind("backend/", 0) == 0 || text == "ir" ||
         text == "backend";
}

[[nodiscard]] bool isSourceFile(const std::filesystem::path& path) {
  const std::string extension = path.extension().string();
  return extension == ".cc" || extension == ".h" || extension == ".cpp" || extension == ".hpp";
}

// Walks one tree and collects every file that includes LLVM without the right to.
[[nodiscard]] std::vector<std::string> scanTree(const std::filesystem::path& root,
                                                const std::string& label) {
  std::vector<std::string> offenders;
  std::error_code ec;
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::recursive_directory_iterator(root, ec)) {
    if (!entry.is_regular_file(ec) || !isSourceFile(entry.path())) {
      continue;
    }
    const std::filesystem::path relative = std::filesystem::relative(entry.path(), root, ec);
    if (mayIncludeLlvm(relative)) {
      continue;
    }
    std::ifstream in(entry.path(), std::ios::binary);
    if (!in.good()) {
      continue;
    }
    std::string line;
    while (std::getline(in, line)) {
      if (line.find("#include \"llvm/") != std::string::npos ||
          line.find("#include <llvm/") != std::string::npos) {
        offenders.push_back(label + relative.generic_string() + ": " + line);
      }
    }
  }
  return offenders;
}

TEST(LlvmIsolationTest, OnlyTheIrAndBackendModulesIncludeLlvm) {
  // Derived from the generated examples path rather than passed in: one
  // generated path is already configured for every generator and platform, and
  // the compiler's sources are its sibling. The project root is not walked as a
  // whole because the rule is about the *targets* and about what a target can
  // reach -- a test linking LLVM to check the IR is allowed, and this test's own
  // file necessarily spells the strings it looks for.
  const std::filesystem::path project = std::filesystem::path(test::kExamplesDir).parent_path();
  const std::filesystem::path sources = project / "src";
  const std::filesystem::path headers = project / "include";
  ASSERT_TRUE(std::filesystem::exists(sources)) << sources;
  ASSERT_TRUE(std::filesystem::exists(headers)) << headers;

  // **Both trees**, and the second one is the half that used to be a comment:
  // `ir.h` is included by the driver, so a `llvm::Module&` accessor added to it
  // would put LLVM in the driver's translation units without a single file under
  // `src/` changing. The boundary is only real if the *interface* is checked, so
  // `include/ir/storage.h` exists precisely to be the one header that crosses it,
  // and this walk is what keeps it the only one.
  std::vector<std::string> offenders = scanTree(sources, "src/");
  for (std::string& offender : scanTree(headers, "include/")) {
    offenders.push_back(std::move(offender));
  }

  for (const std::string& offender : offenders) {
    ADD_FAILURE() << "LLVM is included outside ir/ and backend/: " << offender;
  }
}

} // namespace
} // namespace minc
