// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The one way `mincc` writes a driver-level error.
//
// Every subcommand reports failures through these instead of formatting its own
// `mincc: error: ...`, so the prefix, the hint, and the exit code are identical
// whichever command hit the problem. Messages are plain ASCII: they must be
// readable on any console and in any log.
//
// Each function comes in two forms: one that writes to stderr (what the
// commands use) and one that takes the stream. The stream overloads exist so
// the exact format is pinned by a test rather than by looking at it, and so a
// command that is handed its streams can report through the same code path.
#pragma once

#include <iosfwd>
#include <string_view>

namespace minc::driver {

// "mincc: error: <message>" on `err`.
void printError(std::ostream& err, std::string_view message);
// The actionable hint that closes every usage error.
void printUsageHint(std::ostream& err);
// printError + printUsageHint, and the exit code to return.
[[nodiscard]] int usageError(std::ostream& err, std::string_view message);

// The same three, writing to stderr.
void printError(std::string_view message);
void printUsageHint();
[[nodiscard]] int usageError(std::string_view message);

} // namespace minc::driver
