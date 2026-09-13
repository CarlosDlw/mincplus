// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// `mincc pp`: show what the preprocessor did.
//
// The command exists so the stage is exercised by the compiler itself instead of
// by tooling-only code that rots. Every mode prints *provenance* -- where a macro
// was defined, which include was elided by the guard optimization, what an
// invocation expanded to -- because that is the information the language server
// will need and the information a human cannot get from the output alone.
#pragma once

#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

#include "driver/cli.h"
#include "support/term/terminal.h"

namespace minc::driver {

struct PpRequest {
  std::vector<std::string> inputs;
  std::vector<std::pair<std::string, std::string>> defines; // `-D name[=body]`
  std::vector<std::string> undefines;                       // `-U name`
  std::vector<std::string> includeDirs;                     // `-I dir`
  std::vector<std::string> systemDirs;                      // `-isystem dir`
  bool showDefines = false;
  bool showIncludes = false;
  bool showDeps = false;
  // `--at [file:]line`: how the expansions on that line were produced.
  std::string at;
  support::ColorMode color = support::ColorMode::Plain;
};

// Returns the process exit code. Nothing here exits, and nothing here writes to
// stderr directly except through `printError`, so a test can drive the whole
// command with two string streams.
[[nodiscard]] int ppInputs(const PpRequest& request, std::ostream& out, std::ostream& err);
[[nodiscard]] int runPp(const CliOptions& options);

} // namespace minc::driver
