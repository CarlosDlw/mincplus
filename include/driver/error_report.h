// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The one way `mincc` writes a driver-level error.
//
// Every subcommand reports failures through these instead of formatting its own
// `mincc: error: ...`, so the prefix, the note, the hint and the exit code are
// identical whichever command hit the problem. Messages are plain ASCII: they
// must be readable on any console and in any log.
//
// Each function comes in two forms: one that writes to stderr (what the
// commands use) and one that takes the stream. The stream overloads exist so the
// exact format is pinned by a test rather than by looking at it, and so a
// command that is handed its streams can report through the same code path.
//
// A usage error has three parts, and the middle one is optional:
//
//   mincc: error: unrecognized option '--targt'
//   note: did you mean '--target'?
//   Try 'mincc build --help' for more information.
//
// The hint names the page that has the answer -- the command whose options were
// being typed, when there was one -- because "run --help" is a worse answer than
// the page the reader actually needs.
#pragma once

#include <iosfwd>
#include <optional>
#include <string_view>

#include "driver/command_spec.h"

namespace minc::driver {

// "mincc: error: <message>" on `err`.
void printError(std::ostream& err, std::string_view message);
// "note: did you mean '<suggestion>'?" -- printed only when there is one. The
// suggestion is a note and never an action: the exit code stays 2 (`suggest.h`).
void printSuggestion(std::ostream& err, std::string_view suggestion);
// The actionable hint that closes every usage error, naming the command's page
// when the command is known.
void printUsageHint(std::ostream& err, std::optional<Command> command = std::nullopt);
// printError + printSuggestion + printUsageHint, and the exit code to return.
[[nodiscard]] int usageError(std::ostream& err, std::string_view message,
                             std::string_view suggestion = {},
                             std::optional<Command> command = std::nullopt);

// The same four, writing to stderr.
void printError(std::string_view message);
void printSuggestion(std::string_view suggestion);
void printUsageHint(std::optional<Command> command = std::nullopt);
[[nodiscard]] int usageError(std::string_view message, std::string_view suggestion = {},
                             std::optional<Command> command = std::nullopt);

} // namespace minc::driver
