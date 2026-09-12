// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// `mincc parse <files...>`: show what the parser builds.
//
// It lexes, parses, and builds the syntax tree, then prints the tree on stdout
// and any lexical or syntax diagnostics on stderr. No preprocessing and no
// semantic analysis, so what it prints is exactly the syntax layer -- which is
// what makes it useful for reviewing a grammar change and for writing a
// regression test from real output.
#pragma once

#include <iosfwd>
#include <string>
#include <vector>

#include "driver/cli.h"
#include "support/term/terminal.h"

namespace minc::driver {

// Everything the command needs, with the colors decided by the caller: with an
// injected stream there is no terminal to ask, and asking the real one would be
// the wrong question.
struct ParseRequest {
  std::vector<std::string> inputs; // paths, or "-" for standard input
  bool showTrivia = true;
  support::ColorMode dumpColor = support::ColorMode::Plain;       // tree -> `out`
  support::ColorMode diagnosticColor = support::ColorMode::Plain; // diagnostics -> `err`
};

// The command minus the choice of streams: loads each input, parses it, writes
// the tree to `out` and any diagnostics to `err`, and returns the process exit
// code.
//
// Split out from runParse() so the contract -- which stream carries what, in
// what order, and which exit code -- is tested directly instead of by spawning
// a process and reading the console.
//
// Precondition: `request.inputs` is not empty.
[[nodiscard]] int parseInputs(const ParseRequest& request, std::ostream& out, std::ostream& err);

// As the driver invokes it: `-` reads standard input, and each stream picks its
// own color mode from the real terminal.
[[nodiscard]] int runParse(const CliOptions& options);

} // namespace minc::driver
