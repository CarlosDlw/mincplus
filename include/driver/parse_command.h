// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// `mincc parse <files...>`: show what the parser builds.
//
// It runs the front end -- lex, preprocess, parse -- and prints the tree on
// stdout with every diagnostic on stderr. No semantic analysis, so what it
// prints is the syntax layer alone, which is what makes it useful for reviewing
// a grammar change and for writing a regression test from real output.
//
// Preprocessing is not optional, and that is the point: the tree of a file is
// the tree of its *translation unit*. `mincc lex` is the raw, per-file view, and
// `mincc pp` is the token-level view of the same unit.
#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

#include "driver/cli.h"
#include "support/limits.h"
#include "support/term/terminal.h"

namespace minc::driver {

// Everything the command needs, with the colors decided by the caller: with an
// injected stream there is no terminal to ask, and asking the real one would be
// the wrong question.
struct ParseRequest {
  std::vector<std::string> inputs; // paths, or "-" for standard input
  // `-D`/`-U`/`-I`: the preprocessor's inputs, and therefore the front end's.
  // They live here rather than in a `pp`-only request because a `#define` is a
  // property of the translation unit and not of one command that prints it.
  std::vector<std::pair<std::string, std::string>> defines;
  std::vector<std::string> undefines;
  std::vector<std::string> includeDirs;
  std::vector<std::string> systemDirs; // `-isystem`: `-I`, but the files are system headers
  bool showTrivia = true;
  support::ColorMode dumpColor = support::ColorMode::Plain;       // tree -> `out`
  support::ColorMode diagnosticColor = support::ColorMode::Plain; // diagnostics -> `err`
  // `-ferror-limit`: how many errors are shown before rendering stops, counted
  // across the whole invocation. See `RenderOptions::errorLimit`.
  std::size_t errorLimit = support::kMaxDiagnostics;
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
