// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The one way `mincc` writes a driver-level error.
//
// Every subcommand reports failures through these instead of formatting its own
// `mincc: error: ...`, so the prefix, the hint, and the exit code are identical
// whichever command hit the problem. Messages are plain ASCII: they must be
// readable on any console and in any log.
#pragma once

#include <string_view>

namespace minc::driver {

// "mincc: error: <message>" on stderr.
void printError(std::string_view message);

// The actionable hint that closes every usage error.
void printUsageHint();

// printError + printUsageHint, and the exit code to return.
[[nodiscard]] int usageError(std::string_view message);

} // namespace minc::driver
