// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The last step of help: ask the terminal what it is, render, write, and say
// what the exit code should be.
//
// Everything above this is pure -- `command_spec.h` is data and `help_render.h`
// turns data into a string -- so this is the only file in the help path that
// knows about a stream. That is what makes `mincc --help` testable as a string
// and this file small enough to read in one sitting.
//
// The text is ASCII-only on purpose: Windows consoles render UTF-8
// unpredictably under a legacy code page, and a help page is the wrong place to
// find out. A test enforces it.
#pragma once

#include <optional>
#include <string>

#include "driver/command_spec.h"
#include "support/term/terminal.h"

namespace minc::driver {

// "Usage: mincc <command> [options] [files...]"
[[nodiscard]] std::string usageLine();

// The overview, or one command's page when a topic is given. Default width and
// no color, for a test or for anything that formats help itself.
[[nodiscard]] std::string helpText(std::optional<Command> topic = std::nullopt);

// "mincc 0.1.0"
[[nodiscard]] std::string versionLine();

// The block a bug report needs: version, host, default target, LLVM. The host and
// the LLVM version are the backend's answers, because `llvm/*` may not be
// included here.
[[nodiscard]] std::string versionBlock();

// Render to stdout and return the process exit code. `topic` empty means the
// overview; a topic is what `mincc help <command>` and `mincc <command> --help`
// both end up asking for.
[[nodiscard]] int runHelp(support::ColorChoice choice, std::optional<Command> topic);

// The same overview on **stderr**, with exit 2: the command line named nothing,
// so the text is an answer to a mistake rather than a delivered request. A script
// that runs `mincc` with an empty variable must not read that as success.
[[nodiscard]] int runBareInvocation(support::ColorChoice choice);

[[nodiscard]] int runVersion(support::ColorChoice choice, bool verbose);

} // namespace minc::driver
