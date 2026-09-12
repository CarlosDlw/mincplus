// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The command layer itself: which stream carries what, in what order, and which
// exit code comes back.
//
// The pieces are covered by the lexer suites; this is the only place that
// checks the composition, and doing it through injected streams rather than a
// spawned process keeps it in the normal test run on every platform.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "driver/cli.h"
#include "driver/exit_code.h"
#include "driver/lex_command.h"
#include "support/term/terminal.h"
#include "tests/examples_dir.h"

namespace minc::driver {
namespace {

// "/tmp" does not exist on Windows, so tests ask the platform.
std::string tempPath(const std::string& name) {
  return (std::filesystem::temp_directory_path() / name).string();
}

// Deletes the file on scope exit, so a failing assertion cannot leave litter
// behind for the next run to trip over.
class TempFile {
public:
  TempFile(const std::string& name, std::string_view bytes) : path_(tempPath(name)) {
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

[[nodiscard]] std::string exampleFile(const std::string& name) {
  return (std::filesystem::path(test::kExamplesDir) / name).string();
}

struct LexRun {
  int code = 0;
  std::string out;
  std::string err;
};

LexRun runLex(const std::vector<std::string>& inputs,
              support::ColorMode dumpColor = support::ColorMode::Plain) {
  LexRequest request;
  request.inputs = inputs;
  request.dumpColor = dumpColor;

  std::ostringstream out;
  std::ostringstream err;
  LexRun run;
  run.code = lexInputs(request, out, err);
  run.out = out.str();
  run.err = err.str();
  return run;
}

TEST(LexCommandTest, CleanInputDumpsToStdoutAndSaysNothing) {
  const LexRun run = runLex({exampleFile("002_variables.mx")});
  EXPECT_EQ(run.code, exitCode(ExitCode::Ok));
  EXPECT_TRUE(run.err.empty());
  EXPECT_NE(run.out.find("== "), std::string::npos);
  EXPECT_NE(run.out.find("KwLet"), std::string::npos);
  EXPECT_NE(run.out.find("tokens by kind:"), std::string::npos);
}

TEST(LexCommandTest, MissingFileFailsAndLeavesStdoutAlone) {
  const LexRun run = runLex({"mincplus-no-such-file.mx"});
  EXPECT_EQ(run.code, exitCode(ExitCode::Failure));
  EXPECT_TRUE(run.out.empty());
  EXPECT_NE(run.err.find("mincc: error:"), std::string::npos);
  EXPECT_NE(run.err.find("cannot open"), std::string::npos);
}

TEST(LexCommandTest, DirectoryInputFailsWithAReadableReason) {
  const LexRun run = runLex({std::filesystem::temp_directory_path().string()});
  EXPECT_EQ(run.code, exitCode(ExitCode::Failure));
  EXPECT_NE(run.err.find("directory"), std::string::npos);
}

// A lexical error goes to stderr as a diagnostic and to stdout in the flag
// column of the table: the two views must agree about the same token.
TEST(LexCommandTest, LexicalErrorsGoToStderrAndFailTheRun) {
  const TempFile file("mincplus_lex_command_bad.mx", "let s = \"abc\n");
  const LexRun run = runLex({file.path()});

  EXPECT_EQ(run.code, exitCode(ExitCode::Failure));
  EXPECT_NE(run.out.find("StringLiteral"), std::string::npos);
  EXPECT_NE(run.out.find("unterminated-string"), std::string::npos);
  EXPECT_NE(run.err.find("error[lex-unterminated-string]"), std::string::npos);
  EXPECT_NE(run.err.find('^'), std::string::npos);
}

// Each file's table must be printed before that file's diagnostics, so a long
// multi-file run stays readable.
TEST(LexCommandTest, TableComesBeforeItsOwnDiagnostics) {
  const TempFile file("mincplus_lex_command_order.mx", "let s = \"abc\n");
  LexRequest request;
  request.inputs = {file.path()};

  std::ostringstream both;
  EXPECT_EQ(lexInputs(request, both, both), exitCode(ExitCode::Failure));

  const std::string text = both.str();
  const std::size_t table = text.find("StringLiteral");
  const std::size_t diagnostic = text.find("lex-unterminated-string");
  ASSERT_NE(table, std::string::npos);
  ASSERT_NE(diagnostic, std::string::npos);
  EXPECT_LT(table, diagnostic);
}

TEST(LexCommandTest, EveryInputIsProcessedAndFailureIsSticky) {
  const TempFile file("mincplus_lex_command_multi.mx", "let x = 1;\n");
  const LexRun run =
      runLex({exampleFile("001_main_func.mx"), file.path(), "mincplus-no-such-file.mx"});

  EXPECT_EQ(run.code, exitCode(ExitCode::Failure));
  EXPECT_NE(run.out.find("001_main_func.mx"), std::string::npos);
  EXPECT_NE(run.out.find("mincplus_lex_command_multi.mx"), std::string::npos);
  EXPECT_NE(run.err.find("mincplus-no-such-file.mx"), std::string::npos);
}

TEST(LexCommandTest, RequestedColorReachesTheDump) {
  const LexRun plain = runLex({exampleFile("001_main_func.mx")});
  const LexRun colored = runLex({exampleFile("001_main_func.mx")}, support::ColorMode::Ansi);

  EXPECT_EQ(plain.out.find('\x1b'), std::string::npos);
  EXPECT_NE(colored.out.find('\x1b'), std::string::npos);
}

// Without inputs the command is a usage error before it opens anything. The
// hint is written straight to stderr beside this output, which is the point:
// there is no stream to inject at this level.
TEST(LexCommandTest, NoInputsIsAUsageError) {
  const CliOptions options;
  EXPECT_EQ(runLex(options), exitCode(ExitCode::Usage));
}

} // namespace
} // namespace minc::driver
