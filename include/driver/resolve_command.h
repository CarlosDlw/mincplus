// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// `mincc resolve <files...>`: show what name resolution did.
//
// It runs the whole front end -- lex, preprocess, parse, lower, validate,
// resolve -- and prints the scopes and definitions on stdout with every
// diagnostic on stderr. It is the command that proves the two stages after the
// parser: `--refs` shows that every name use has an answer, `--unresolved` shows
// the reasons, `--ast` shows that lowering is what the design says it is, and
// `--at` is go-to-definition.
//
// Preprocessing is not optional and not re-implemented: `resolve` runs the same
// front end `parse` does, because a name in a macro body is a name.
#pragma once

#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

#include "driver/cli.h"
#include "support/term/terminal.h"

namespace minc::driver {

struct ResolveRequest {
  std::vector<std::string> inputs; // paths, or "-" for standard input
  std::vector<std::pair<std::string, std::string>> defines;
  std::vector<std::string> undefines;
  std::vector<std::string> includeDirs;
  std::vector<std::string> systemDirs;
  bool showRefs = false;
  bool showUnresolved = false;
  bool showAst = false;
  bool warnUnused = false;
  bool warnShadow = false;
  // `[file:]line:col`; empty means the tables.
  std::string at;
  support::ColorMode diagnosticColor = support::ColorMode::Plain;
};

// The command minus the choice of streams, so the contract -- which stream
// carries what and which exit code -- is tested directly instead of by spawning
// a process.
//
// Precondition: `request.inputs` is not empty.
[[nodiscard]] int resolveInputs(const ResolveRequest& request, std::ostream& out,
                                std::ostream& err);

// As the driver invokes it: `-` reads standard input, and the streams pick their
// own color mode from the real terminal.
[[nodiscard]] int runResolve(const CliOptions& options);

} // namespace minc::driver
