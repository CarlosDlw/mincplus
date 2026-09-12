// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include "support/term/terminal.h"

namespace minc::support {
namespace {

// The NO_COLOR rule is about presence, not value: an empty value still means
// "no color". Getting that wrong would make `NO_COLOR=` silently do nothing.
TEST(TerminalTest, NoColorDisablesColorWheneverItIsPresent) {
  EXPECT_FALSE(colorDisabledByEnvironmentValue(nullptr));
  EXPECT_TRUE(colorDisabledByEnvironmentValue(""));
  EXPECT_TRUE(colorDisabledByEnvironmentValue("1"));
  EXPECT_TRUE(colorDisabledByEnvironmentValue("0"));
  EXPECT_TRUE(colorDisabledByEnvironmentValue("false"));
}

TEST(TerminalTest, OnlyTheExactDumbTermIsDumb) {
  EXPECT_FALSE(terminalIsDumbForValue(nullptr));
  EXPECT_FALSE(terminalIsDumbForValue(""));
  EXPECT_FALSE(terminalIsDumbForValue("xterm-256color"));
  EXPECT_FALSE(terminalIsDumbForValue("dumb-terminal"));
  EXPECT_TRUE(terminalIsDumbForValue("dumb"));
}

TEST(TerminalTest, ColorModeFollowsTheAnswer) {
  EXPECT_EQ(colorModeFrom(true), ColorMode::Ansi);
  EXPECT_EQ(colorModeFrom(false), ColorMode::Plain);
}

// Asking twice must not change the answer. On Windows the first call turns on
// virtual-terminal processing, a side effect that must not be mistaken for a
// change in capability.
TEST(TerminalTest, SupportsColorIsStableWithinAProcess) {
  const bool outFirst = stdoutSupportsColor();
  EXPECT_EQ(stdoutSupportsColor(), outFirst);
  EXPECT_EQ(stdoutSupportsColor(), outFirst);

  const bool errFirst = stderrSupportsColor();
  EXPECT_EQ(stderrSupportsColor(), errFirst);
}

} // namespace
} // namespace minc::support
