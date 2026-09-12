// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The `parse` subcommand's contract: which stream carries what, in what order,
// and which exit code -- tested directly instead of by spawning a process.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "driver/exit_code.h"
#include "driver/parse_command.h"
#include "tests/examples_dir.h"

namespace minc::driver {
namespace {

struct ParseRun {
  int code = 0;
  std::string out;
  std::string err;
};

[[nodiscard]] ParseRun runParse(const std::vector<std::string>& inputs, bool showTrivia = true) {
  ParseRequest request;
  request.inputs = inputs;
  request.showTrivia = showTrivia;
  // Color stays plain: with injected streams there is no terminal to ask, and
  // asking the real one would be the wrong question.
  request.dumpColor = support::ColorMode::Plain;
  request.diagnosticColor = support::ColorMode::Plain;

  std::ostringstream out;
  std::ostringstream err;
  const int code = parseInputs(request, out, err);
  return ParseRun{code, out.str(), err.str()};
}

[[nodiscard]] std::string tempPath(const std::string& name) {
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

TEST(ParseCommandTest, ExamplePrintsATreeOnStdout) {
  const ParseRun run = runParse({exampleFile("001_main_func.mx")});

  EXPECT_EQ(run.code, exitCode(ExitCode::Ok));
  EXPECT_TRUE(run.err.empty()) << run.err;
  EXPECT_NE(run.out.find("== "), std::string::npos);
  EXPECT_NE(run.out.find("File@0.."), std::string::npos);
  EXPECT_NE(run.out.find("FnDecl@"), std::string::npos);
  EXPECT_NE(run.out.find("0 errors"), std::string::npos);
}

TEST(ParseCommandTest, NoTriviaHidesWhitespaceAndComments) {
  const ParseRun withTrivia = runParse({exampleFile("002_variables.mx")});
  const ParseRun withoutTrivia = runParse({exampleFile("002_variables.mx")}, /*showTrivia=*/false);

  EXPECT_NE(withTrivia.out.find("Whitespace@"), std::string::npos);
  EXPECT_NE(withTrivia.out.find("LineComment@"), std::string::npos);

  EXPECT_EQ(withoutTrivia.out.find("Whitespace@"), std::string::npos);
  EXPECT_EQ(withoutTrivia.out.find("Newline@"), std::string::npos);
  EXPECT_EQ(withoutTrivia.out.find("LineComment@"), std::string::npos);
  // The code itself is still there.
  EXPECT_NE(withoutTrivia.out.find("FnDecl@"), std::string::npos);
}

TEST(ParseCommandTest, SyntaxErrorsGoToStderrAndFailTheRun) {
  const TempFile file("mincplus_parse_command_bad.mx", "fn i32 main() { let x = ; }\n");
  const ParseRun run = runParse({file.path()});

  EXPECT_EQ(run.code, exitCode(ExitCode::Failure));
  // The tree is still printed: a malformed file still has a tree to inspect,
  // which is the whole point of recovery.
  EXPECT_NE(run.out.find("File@0.."), std::string::npos);
  EXPECT_NE(run.out.find("Error"), std::string::npos);
  EXPECT_NE(run.err.find("error[parse-"), std::string::npos);
  EXPECT_NE(run.err.find("^"), std::string::npos);
}

TEST(ParseCommandTest, LexicalErrorsAreReportedToo) {
  const TempFile file("mincplus_parse_command_lex.mx", "let s = \"abc\n");
  const ParseRun run = runParse({file.path()});

  EXPECT_EQ(run.code, exitCode(ExitCode::Failure));
  EXPECT_NE(run.err.find("error[lex-unterminated-string]"), std::string::npos);
}

TEST(ParseCommandTest, MissingFileIsAnError) {
  const ParseRun run = runParse({"definitely-not-here.mx"});

  EXPECT_EQ(run.code, exitCode(ExitCode::Failure));
  EXPECT_NE(run.err.find("mincc: error:"), std::string::npos);
  EXPECT_TRUE(run.out.empty());
}

TEST(ParseCommandTest, EveryExampleSucceeds) {
  const std::vector<std::string> files{
      exampleFile("001_main_func.mx"), exampleFile("002_variables.mx"), exampleFile("003_types.mx"),
      exampleFile("004_operators.mx"), exampleFile("005_literals.mx")};
  const ParseRun run = runParse(files);

  EXPECT_EQ(run.code, exitCode(ExitCode::Ok)) << run.err;
  EXPECT_TRUE(run.err.empty()) << run.err;
  // Each file gets its own header, so a long command line stays readable.
  for (const std::string& file : files) {
    EXPECT_NE(run.out.find(file), std::string::npos) << file;
  }
}

// Without inputs the command is a usage error before it opens anything. Like
// `lex`, the check lives in the entry point rather than in the injectable body,
// so the hint goes straight to stderr -- there is no stream to inject here.
TEST(ParseCommandTest, NoInputsIsAUsageError) {
  const CliOptions options;
  EXPECT_EQ(runParse(options), exitCode(ExitCode::Usage));
}

} // namespace
} // namespace minc::driver
