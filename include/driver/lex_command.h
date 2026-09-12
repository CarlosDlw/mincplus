// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// `mincc lex <files...>`: show what the lexer sees.
//
// This is the first subcommand with real behaviour, and it is deliberately the
// narrowest one: it runs no preprocessing and no parsing, so what it prints is
// exactly the module it names. That makes it useful for reviewing a lexer
// change, for writing a regression test from real output, and for the language
// server later -- all of which need the raw token stream, not a parse.
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
struct LexRequest {
  std::vector<std::string> inputs;                          // paths, or "-" for standard input
  support::ColorMode dumpColor = support::ColorMode::Plain; // token table -> `out`
  support::ColorMode diagnosticColor = support::ColorMode::Plain; // diagnostics -> `err`
};

// The command minus the choice of streams: loads each input, lexes it, writes
// the token table to `out` and any diagnostics to `err`, and returns the process
// exit code.
//
// Split out from runLex() so the command's contract is tested directly --
// which stream carries what, in what order, and which exit code -- instead of by
// spawning a process and reading the console.
//
// Precondition: `request.inputs` is not empty.
[[nodiscard]] int lexInputs(const LexRequest& request, std::ostream& out, std::ostream& err);

// As the driver invokes it: `-` reads standard input, and each stream picks its
// own color mode from the real terminal.
[[nodiscard]] int runLex(const CliOptions& options);

} // namespace minc::driver
