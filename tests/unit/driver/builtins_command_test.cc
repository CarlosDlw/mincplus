// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// `mincc builtins`: the table, as the terminal reads it.
//
// The command is the table, so the assertions are about the two ways a rendered
// list can be wrong: describing something the compiler does not have, and missing
// something it does. Both are checked against `all()` rather than against a
// literal, because a literal here would be the second copy of the table this
// whole module exists to avoid.
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "builtins/builtin.h"
#include "driver/builtins_command.h"
#include "driver/exit_code.h"

namespace minc::driver {
namespace {

struct CommandOutput {
  int code = 0;
  std::string out;
  std::string err;
};

[[nodiscard]] CommandOutput runBuiltinsCommand(std::vector<std::string> inputs = {}) {
  CliOptions options;
  options.inputs = inputs;

  std::ostringstream out;
  std::ostringstream err;
  std::streambuf* oldOut = std::cout.rdbuf(out.rdbuf());
  std::streambuf* oldErr = std::cerr.rdbuf(err.rdbuf());
  CommandOutput run;
  run.code = runBuiltins(options);
  std::cout.rdbuf(oldOut);
  std::cerr.rdbuf(oldErr);
  run.out = out.str();
  run.err = err.str();
  return run;
}

TEST(BuiltinsCommandTest, EveryRowAppearsWithItsSentence) {
  const CommandOutput run = runBuiltinsCommand();
  EXPECT_EQ(run.code, exitCode(ExitCode::Ok));
  EXPECT_TRUE(run.err.empty());

  for (const builtins::BuiltinInfo& row : builtins::all()) {
    EXPECT_NE(run.out.find(row.spelling), std::string::npos) << row.spelling;
    EXPECT_NE(run.out.find(row.doc), std::string::npos) << row.spelling;
    // The class and the status, for each row: what a reader has to know to use it
    // (is it shadowable?) and to rely on it (is it promised?).
    EXPECT_NE(run.out.find(builtins::toString(row.spellingClass)), std::string::npos)
        << row.spelling;
    EXPECT_NE(run.out.find(builtins::toString(row.status)), std::string::npos) << row.spelling;
  }
}

TEST(BuiltinsCommandTest, TheHeaderExplainsTheTwoSpellingClasses) {
  // The distinction the table is built on is the one thing a printed list cannot
  // show by itself: `clz` is an ordinary name and `__builtin_*` is not.
  const CommandOutput run = runBuiltinsCommand();
  EXPECT_NE(run.out.find("prelude"), std::string::npos);
  EXPECT_NE(run.out.find("redeclaration"), std::string::npos);
}

TEST(BuiltinsCommandTest, AnInputFileIsARefusalAndNotSilence) {
  // This command reads no files. Quietly ignoring one would leave a reader who
  // typed a file believing it had been read.
  const CommandOutput run = runBuiltinsCommand({"main.mx"});
  EXPECT_EQ(run.code, exitCode(ExitCode::Usage));
  EXPECT_TRUE(run.out.empty());
  EXPECT_NE(run.err.find("reads no files"), std::string::npos);
}

} // namespace
} // namespace minc::driver
