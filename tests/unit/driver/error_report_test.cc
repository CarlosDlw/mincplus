// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "driver/error_report.h"
#include "driver/exit_code.h"
#include "driver/version.h"

namespace minc::driver {
namespace {

// The exact text, not a substring: scripts and users both read this, and the
// point of routing every failure through one function is that it cannot vary.
TEST(ErrorReportTest, ErrorLineIsStable) {
  std::ostringstream err;
  printError(err, "something went wrong");
  EXPECT_EQ(err.str(), std::string(kProgName) + ": error: something went wrong\n");
}

TEST(ErrorReportTest, UsageErrorAddsTheHintAndReturnsUsage) {
  std::ostringstream err;
  const int code = usageError(err, "unrecognized option '--nope'");
  EXPECT_EQ(code, exitCode(ExitCode::Usage));

  std::string expected(kProgName);
  expected += ": error: unrecognized option '--nope'\n";
  expected += "Try '";
  expected += kProgName;
  expected += " --help' for more information.\n";
  EXPECT_EQ(err.str(), expected);
}

TEST(ErrorReportTest, HintNamesTheProgramThatCanHelp) {
  std::ostringstream err;
  printUsageHint(err);
  EXPECT_NE(err.str().find(kProgName), std::string::npos);
  EXPECT_NE(err.str().find("--help"), std::string::npos);
}

TEST(ErrorReportTest, PrintErrorAloneCarriesNoHint) {
  std::ostringstream withHint;
  std::ostringstream without;
  printError(without, "boom");
  EXPECT_EQ(usageError(withHint, "boom"), exitCode(ExitCode::Usage));
  EXPECT_NE(withHint.str().find("--help"), std::string::npos);
  EXPECT_EQ(without.str().find("--help"), std::string::npos);
}

// The messages are ASCII and lowercase so they read the same on any console and
// in any log file.
TEST(ErrorReportTest, MessagesAreAscii) {
  std::ostringstream err;
  printError(err, "cannot open 'file.mx'");
  for (const char c : err.str()) {
    const auto byte = static_cast<unsigned char>(c);
    EXPECT_TRUE(c == '\n' || (byte >= 0x20U && byte <= 0x7EU));
  }
}

} // namespace
} // namespace minc::driver
