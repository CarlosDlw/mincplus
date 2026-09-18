// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// `support/process`, and the half of its contract a test can exercise without a
// program to run: what happens when there is nothing to start.
//
// The other half -- a program that *does* run, and the status it comes back with,
// including the two statuses Unix reserves -- is checked where it is visible to a
// user, by building and running a real program (`driver/build_command_test.cc`).
// That is the division on purpose: this file is about the layer's *refusals*,
// which need no toolchain, and that one is about the answer a user reads.
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "support/process/process.h"

namespace minc {
namespace {

// One directory per test, removed on the way out, so a test that leaves a file
// behind on a failure does not affect the next run. The pattern is the driver
// tests' (`build_command_test.cc`), kept small because only one file is needed.
class ScratchDir {
public:
  ScratchDir() {
    std::error_code error;
    const std::filesystem::path base = std::filesystem::temp_directory_path(error);
    if (error) {
      return;
    }
    std::random_device device;
    for (int attempt = 0; attempt < 32; ++attempt) {
      const std::filesystem::path candidate =
          base / ("minc-process-" + std::to_string(device()) + "-" + std::to_string(attempt));
      if (std::filesystem::create_directory(candidate, error)) {
        path_ = candidate;
        return;
      }
    }
  }
  ~ScratchDir() {
    if (!path_.empty()) {
      std::error_code ignored;
      (void)std::filesystem::remove_all(path_, ignored);
    }
  }
  ScratchDir(const ScratchDir&) = delete;
  ScratchDir& operator=(const ScratchDir&) = delete;

  [[nodiscard]] bool valid() const {
    return !path_.empty();
  }
  [[nodiscard]] std::string file(std::string_view name) const {
    return (path_ / name).string();
  }
  [[nodiscard]] std::string write(std::string_view name, std::string_view text) const {
    const std::string full = file(name);
    std::ofstream out(full, std::ios::binary);
    out << text;
    return full;
  }

private:
  std::filesystem::path path_;
};

TEST(ProcessTest, AnEmptyProgramNameIsRefused) {
  // A caller bug, answered rather than passed to the platform: the platform's own
  // message for an empty name is about a file, and the mistake here is that no file
  // was named at all.
  const support::ProcessOutcome outcome = support::runAndWait("", {});
  EXPECT_FALSE(outcome.started);
  EXPECT_FALSE(outcome.exited);
  EXPECT_FALSE(outcome.error.empty());
}

TEST(ProcessTest, AProgramThatIsNotThereDoesNotStart) {
  ScratchDir scratch;
  ASSERT_TRUE(scratch.valid());

  const support::ProcessOutcome outcome =
      support::runAndWait(scratch.file("nowhere-at-all"), {"--argument"});
  EXPECT_FALSE(outcome.started) << "nothing ran, so nothing started";
  EXPECT_FALSE(outcome.exited) << "and nothing exited either";
  EXPECT_FALSE(outcome.error.empty()) << "the platform's reason is the sentence to print";
  // The two facts are separate, and this is the pair that used to be one: a caller
  // that answers with `status` whenever `started` is false would report `0` here,
  // which is a program that ran and succeeded.
  EXPECT_EQ(outcome.status, 0);
}

TEST(ProcessTest, AFileThatIsNotAProgramDoesNotStart) {
  // A real file that is not an executable image. Distinct from the case above: the
  // path resolves, the platform opens it, and the *platform* refuses -- which is
  // the refusal `run` has to attribute to the platform rather than to a missing
  // file.
  ScratchDir scratch;
  ASSERT_TRUE(scratch.valid());
  const std::string text = scratch.write("not-a-program.txt", "this is not an executable\n");

  const support::ProcessOutcome outcome = support::runAndWait(text, {});
  EXPECT_FALSE(outcome.started);
  EXPECT_FALSE(outcome.exited);
  EXPECT_FALSE(outcome.error.empty());
}

TEST(ProcessTest, ADirectoryDoesNotStart) {
  // The third refusal, and the one that proves the layer is not answering by
  // looking at a file's existence: a directory exists, and it is not a program.
  ScratchDir scratch;
  ASSERT_TRUE(scratch.valid());

  const support::ProcessOutcome outcome = support::runAndWait(scratch.file("."), {});
  EXPECT_FALSE(outcome.started);
  EXPECT_FALSE(outcome.error.empty());
}

} // namespace
} // namespace minc
