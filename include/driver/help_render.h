// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Rendering the command table into text, and nothing else.
//
// Every function here is pure: it is handed a width and a color mode and returns
// a string. There is no terminal, no stream, no exit code and no clock, which is
// what makes the output testable at the level of a string -- including the two
// things that are otherwise only visible by looking: that no line is wider than
// the width asked for, and that the output is ASCII.
//
// The tables being rendered live in `command_spec.h`; this file decides only
// *where* a fact goes on the page.
#pragma once

#include <string>
#include <string_view>

#include "driver/command_spec.h"
#include "support/term/terminal.h"

namespace minc::driver {

// Width in columns, and whether escape sequences may be used. Both are answers
// to questions asked elsewhere -- `support/term` asks the terminal, the parser
// asks the command line -- so a renderer has no environment of its own to read.
struct PageStyle {
  unsigned width = support::kDefaultTerminalWidth;
  support::ColorMode color = support::ColorMode::Plain;
};

// The program's own page: what this is, the commands and their one-line
// summaries, the options every command accepts, the environment variables, and
// the exit-status contract. Deliberately short -- a command's options are on
// that command's page, and repeating them here is how the old text became a wall.
[[nodiscard]] std::string renderOverview(PageStyle style);

// One command's page: summary, usage, description, its option groups, examples,
// and what to read next.
[[nodiscard]] std::string renderCommand(Command command, PageStyle style);

// The facts a version line needs, supplied by the caller so this file stays
// pure: the host is the build's own triple (`sema/host.h`), the LLVM version is
// the backend's answer, and the two of them are what a bug report is missing when
// they are absent. `default target:` is read from `sema` here rather than passed
// in, because it is a table constant and not a runtime fact.
struct VersionFacts {
  std::string_view program;
  std::string_view version;
  std::string_view hostTriple;
  std::string_view llvmVersion;
};

// One line, or the block when `verbose`: version, host, default target, LLVM.
[[nodiscard]] std::string renderVersion(const VersionFacts& facts, bool verbose);

} // namespace minc::driver
