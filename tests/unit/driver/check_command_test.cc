// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The `check` subcommand's contract: which stream carries what, in what order,
// and which exit code -- tested directly instead of by spawning a process.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "driver/check_command.h"
#include "driver/exit_code.h"
#include "sema/target.h"
#include "tests/examples_dir.h"

namespace minc::driver {
namespace {

struct CheckRun {
  int code = 0;
  std::string out;
  std::string err;
};

[[nodiscard]] CheckRun run(const std::vector<std::string>& inputs, bool showAst = false,
                           bool showTypes = false, bool stats = false,
                           sema::TargetInfo target = sema::defaultTarget(),
                           bool warnConversion = false) {
  CheckRequest request;
  request.inputs = inputs;
  request.showAst = showAst;
  request.showTypes = showTypes;
  request.stats = stats;
  request.target = target;
  request.warnConversion = warnConversion;
  // With injected streams there is no terminal to ask, and asking the real one
  // would be the wrong question.
  request.diagnosticColor = support::ColorMode::Plain;

  std::ostringstream out;
  std::ostringstream err;
  const int code = checkInputs(request, out, err);
  return CheckRun{code, out.str(), err.str()};
}

// Deletes the file on scope exit, so a failing assertion cannot leave litter
// behind for the next run to trip over.
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

TEST(CheckCommandTest, ACleanUnitIsSilent) {
  TempFile file("minc_check_clean.mx", "fn i32 main() { let x: u8 = 1; return 0; }\n");
  const CheckRun result = run({file.path()});

  // The exit code is the answer. A file that type-checks prints nothing at all,
  // which is what makes the command usable from a build script and from a
  // terminal that is not being read line by line.
  EXPECT_EQ(result.code, exitCode(ExitCode::Ok));
  EXPECT_TRUE(result.out.empty());
  EXPECT_TRUE(result.err.empty());
}

TEST(CheckCommandTest, AnErrorGoesToStderrAndIsStillSilentOnStdout) {
  TempFile file("minc_check_bad.mx", "fn i32 main() { let x: i33 = 1; return x; }\n");
  const CheckRun result = run({file.path()});

  EXPECT_EQ(result.code, exitCode(ExitCode::Failure));
  EXPECT_NE(result.err.find("sema-unknown-type"), std::string::npos);
  // Diagnostics are the only output by default: no table, no summary -- what a
  // compiler does, so a script can read stderr and a human can read the caret.
  EXPECT_TRUE(result.out.empty());
}

TEST(CheckCommandTest, StatsAddsOneSummaryLinePerFile) {
  TempFile clean("minc_check_stats.mx", "fn i32 main() { let x: u8 = 1; return 0; }\n");
  const CheckRun ok = run({clean.path()}, /*showAst=*/false, /*showTypes=*/false,
                          /*stats=*/true);
  EXPECT_EQ(ok.code, exitCode(ExitCode::Ok));
  ASSERT_EQ(ok.out.size(), ok.out.find('\n') + 1); // exactly one line
  EXPECT_NE(ok.out.find("scopes 2"), std::string::npos);
  EXPECT_NE(ok.out.find("functions 1"), std::string::npos);
  EXPECT_NE(ok.out.find("0 error(s), 0 warning(s)"), std::string::npos);

  TempFile bad("minc_check_stats_bad.mx", "fn i32 main() { let x: i33 = 1; return x; }\n");
  const CheckRun failed = run({bad.path()}, /*showAst=*/false, /*showTypes=*/false,
                              /*stats=*/true);
  EXPECT_EQ(failed.code, exitCode(ExitCode::Failure));
  // The line reports the verdict even when the verdict is bad.
  EXPECT_NE(failed.out.find("1 error(s)"), std::string::npos);
}

TEST(CheckCommandTest, AParseErrorIsNotReReportedAsATypingError) {
  TempFile file("minc_check_parse.mx", "fn i32 main() { let x = ; return 0; }\n");
  const CheckRun result = run({file.path()});

  EXPECT_EQ(result.code, exitCode(ExitCode::Failure));
  EXPECT_NE(result.err.find("parse-"), std::string::npos);
  EXPECT_EQ(result.err.find("sema-"), std::string::npos);
}
TEST(CheckCommandTest, TheTargetChangesWhatALongIs) {
  TempFile file("minc_check_target.mx", "fn i32 main() { let x: long = 1; return 0; }\n");

  const CheckRun sysv = run({file.path()}, /*showAst=*/false, /*showTypes=*/true);
  EXPECT_EQ(sysv.code, exitCode(ExitCode::Ok));
  EXPECT_NE(sysv.out.find("long=64"), std::string::npos);

  const std::optional<sema::TargetInfo> windowsTarget =
      sema::targetFromName(sema::kTripleWindowsAmd64);
  ASSERT_TRUE(windowsTarget.has_value());
  const CheckRun windows = run({file.path()}, /*showAst=*/false, /*showTypes=*/true,
                               /*stats=*/false, *windowsTarget);
  EXPECT_EQ(windows.code, exitCode(ExitCode::Ok));
  EXPECT_NE(windows.out.find("long=32"), std::string::npos);
}

TEST(CheckCommandTest, TypesPrintsTheTableAndNothingElse) {
  TempFile file("minc_check_types.mx", "fn i32 main() { return 0; }\n");
  const CheckRun result = run({file.path()}, /*showAst=*/false, /*showTypes=*/true);

  EXPECT_EQ(result.code, exitCode(ExitCode::Ok));
  EXPECT_NE(result.out.find("# types "), std::string::npos);
  // The target line names the triple, because the triple *is* the identity.
  EXPECT_NE(result.out.find(std::string(sema::kDefaultTriple)), std::string::npos);
  EXPECT_NE(result.out.find("i32"), std::string::npos);
  // No per-file summary: the table was what was asked for.
  EXPECT_EQ(result.out.find("0 error(s)"), std::string::npos);
}

TEST(CheckCommandTest, AstPrintsEveryNodeWithItsType) {
  TempFile file("minc_check_ast.mx", "fn i32 main() { let x: u8 = 7; return 0; }\n");
  const CheckRun result = run({file.path()}, /*showAst=*/true);

  EXPECT_EQ(result.code, exitCode(ExitCode::Ok));
  EXPECT_NE(result.out.find("typed AST"), std::string::npos);
  EXPECT_NE(result.out.find("FnDecl"), std::string::npos);
  // The literal adopted the context's type, which is what makes a surprising
  // conversion visible at the node that performed it.
  EXPECT_NE(result.out.find("u8 [const] =7"), std::string::npos);
}

TEST(CheckCommandTest, ConversionIsAWarningAndKeepsTheExitCode) {
  const std::string_view source =
      "fn i32 main() { let wide: i32 = 1; let narrow: i8 = wide; return 0; }\n";
  TempFile quiet("minc_check_quiet.mx", source);
  const CheckRun without = run({quiet.path()});
  EXPECT_EQ(without.code, exitCode(ExitCode::Ok));
  EXPECT_EQ(without.err.find("sema-implicit-conversion"), std::string::npos);

  TempFile loud("minc_check_loud.mx", source);
  const CheckRun with = run({loud.path()}, /*showAst=*/false, /*showTypes=*/false, /*stats=*/true,
                            sema::defaultTarget(), /*warnConversion=*/true);
  // A warning is not a failure: scripts that treat exit 1 as "does not compile"
  // must keep working.
  EXPECT_EQ(with.code, exitCode(ExitCode::Ok));
  EXPECT_NE(with.err.find("sema-implicit-conversion"), std::string::npos);
  EXPECT_NE(with.out.find("1 warning(s)"), std::string::npos);
}

TEST(CheckCommandTest, EveryExampleChecks) {
  namespace fs = std::filesystem;
  std::vector<std::string> files;
  std::error_code ec;
  for (const fs::directory_entry& entry : fs::directory_iterator(test::kExamplesDir, ec)) {
    if (entry.is_regular_file(ec) && entry.path().extension() == ".mx") {
      files.push_back(entry.path().string());
    }
  }
  ASSERT_FALSE(files.empty()) << "examples/ is missing files";

  const CheckRun result = run(files, /*showAst=*/false, /*showTypes=*/false, /*stats=*/true);
  EXPECT_EQ(result.code, exitCode(ExitCode::Ok)) << result.err;
  EXPECT_EQ(result.err.find("error["), std::string::npos) << result.err;
  // One summary line per input, so a file silently skipped would be visible.
  std::size_t summaries = 0;
  for (std::size_t at = result.out.find("# "); at != std::string::npos;
       at = result.out.find("# ", at + 1)) {
    ++summaries;
  }
  EXPECT_EQ(summaries, files.size());
}

TEST(CheckCommandTest, AMissingFileIsADriverErrorAndNotACrash) {
  const CheckRun result = run({"this-file-does-not-exist.mx"});
  EXPECT_EQ(result.code, exitCode(ExitCode::Failure));
  EXPECT_NE(result.err.find("this-file-does-not-exist.mx"), std::string::npos);
}

} // namespace
} // namespace minc::driver
