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
#include <vector>

#include <gtest/gtest.h>

#include "tests/examples_dir.h"

namespace minc {
namespace {

// The directories allowed to see LLVM. `src/backend` is listed before it exists
// on purpose: the day it is created it must not have to remember to update a
// test, and the list is the *policy* rather than a census of what is there.
[[nodiscard]] bool mayIncludeLlvm(const std::filesystem::path& relative) {
  // Relative to `src/`, which is where the walk starts.
  const std::string text = relative.generic_string();
  return text.rfind("ir/", 0) == 0 || text.rfind("backend/", 0) == 0;
}

[[nodiscard]] bool isSourceFile(const std::filesystem::path& path) {
  const std::string extension = path.extension().string();
  return extension == ".cc" || extension == ".h" || extension == ".cpp" || extension == ".hpp";
}

TEST(LlvmIsolationTest, OnlyTheIrAndBackendModulesIncludeLlvm) {
  // Derived from the generated examples path rather than passed in: one
  // generated path is already configured for every generator and platform, and
  // the compiler's sources are its sibling. The walk is rooted at `src/` and not
  // at the project, because the rule is about the *targets* -- a test linking
  // LLVM to check the IR is allowed, and this test's own file necessarily spells
  // the strings it looks for.
  const std::filesystem::path root =
      std::filesystem::path(test::kExamplesDir).parent_path() / "src";
  ASSERT_TRUE(std::filesystem::exists(root)) << root;

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
        offenders.push_back(relative.generic_string() + ": " + line);
      }
    }
  }

  for (const std::string& offender : offenders) {
    ADD_FAILURE() << "LLVM is included outside src/ir: " << offender;
  }
}

} // namespace
} // namespace minc
