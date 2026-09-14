// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The `ir` subcommand's contract: which stream carries what, and which exit
// code -- tested directly instead of by spawning a process.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "driver/exit_code.h"
#include "driver/ir_command.h"
#include "sema/target.h"

namespace minc::driver {
namespace {

struct IrRun {
  int code = 0;
  std::string out;
  std::string err;
};

[[nodiscard]] IrRun run(const std::vector<std::string>& inputs,
                        sema::TargetInfo target = sema::defaultTarget()) {
  IrRequest request;
  request.inputs = inputs;
  request.target = target;
  request.diagnosticColor = support::ColorMode::Plain;

  std::ostringstream out;
  std::ostringstream err;
  const int code = irInputs(request, out, err);
  return IrRun{code, out.str(), err.str()};
}

class TempFile {
public:
  TempFile(const std::string& name, std::string_view bytes) {
    path_ = (std::filesystem::temp_directory_path() / name).string();
    std::ofstream out(path_, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  ~TempFile() {
    static_cast<void>(std::remove(path_.c_str()));
  }

  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  [[nodiscard]] const std::string& path() const {
    return path_;
  }

private:
  std::string path_;
};

TEST(IrCommandTest, ACleanUnitPrintsItsModuleToStdout) {
  TempFile file("minc_ir_clean.mx", "fn i32 main()\n{\n  return 7;\n}\n");
  const IrRun result = run({file.path()});

  EXPECT_EQ(result.code, exitCode(ExitCode::Ok));
  EXPECT_TRUE(result.err.empty());
  EXPECT_NE(result.out.find("define i32 @main()"), std::string::npos);
  EXPECT_NE(result.out.find("ret i32 7"), std::string::npos);
  // The module names its target, because a module is only meaningful against
  // one: sizes and alignments are the target's, not the host's.
  EXPECT_NE(result.out.find("target triple ="), std::string::npos);
}

TEST(IrCommandTest, AFailingUnitPrintsNoModuleAndFails) {
  TempFile file("minc_ir_bad.mx", "fn i32 main() { let x: i33 = 1; return 0; }\n");
  const IrRun result = run({file.path()});

  EXPECT_EQ(result.code, exitCode(ExitCode::Failure));
  EXPECT_NE(result.err.find("sema-unknown-type"), std::string::npos);
  // Nothing on stdout: a module built from a tree that had an error is a module
  // nobody decided the meaning of, and handing one to a linker is the outcome
  // the stage exists to prevent.
  EXPECT_TRUE(result.out.empty());
}

TEST(IrCommandTest, AParseErrorAlsoProducesNoModule) {
  TempFile file("minc_ir_parse.mx", "fn i32 main() { let x = ; return 0; }\n");
  const IrRun result = run({file.path()});

  EXPECT_EQ(result.code, exitCode(ExitCode::Failure));
  EXPECT_NE(result.err.find("parse-"), std::string::npos);
  EXPECT_TRUE(result.out.empty());
}

TEST(IrCommandTest, TheTargetIsTheModulesTarget) {
  const auto windows = sema::targetFromName(sema::kTripleWindowsAmd64);
  ASSERT_TRUE(windows.has_value());
  TempFile file("minc_ir_target.mx", "fn i32 main() { return 0; }\n");
  const IrRun result = run({file.path()}, *windows);

  EXPECT_EQ(result.code, exitCode(ExitCode::Ok));
  EXPECT_NE(result.out.find("x86_64-pc-windows-msvc"), std::string::npos);
}

TEST(IrCommandTest, AMissingFileIsADriverErrorAndNotACrash) {
  const IrRun result = run({"this-file-does-not-exist.mx"});
  EXPECT_EQ(result.code, exitCode(ExitCode::Failure));
  EXPECT_NE(result.err.find("this-file-does-not-exist.mx"), std::string::npos);
  EXPECT_TRUE(result.out.empty());
}

} // namespace
} // namespace minc::driver
