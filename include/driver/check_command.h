// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// `mincc check <files...>`: the whole front end, and the verdict.
//
// It runs lex, preprocess, parse, lower, validate, resolve and type-check, and
// nothing else: no code is emitted, so it is the command a build script runs
// when it wants "does this compile?" without paying for a backend. It is also
// the command that proves the `sema` stage, the way `resolve` proves resolution.
//
// Preprocessing is not optional and not re-implemented, for the reason `resolve`
// gives: a type in a header is a type, and a literal in a macro body is a
// literal. The typing of the unit is produced by the compilation's central
// checker (`sema::Context`), not by a one-off call, so the cache the language
// server will live on is exercised by the command that proves the stage.
//
// Output:
//   * the type table, so a reader can see what the compilation decided a type is
//     and how wide it is (this is where `long` on `--target windows-x64` differs
//     from the System V default);
//   * one summary line per file -- scopes, defs, refs, types, errors, warnings;
//   * with `--ast`, the typed tree instead: every node with the type it was
//     given, which is what makes a surprising conversion visible where it
//     happens.
//
// Diagnostics go to stderr, the exit code follows `exit_code.h`, and a unit with
// one error in any stage fails the command.
#pragma once

#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

#include "driver/cli.h"
#include "sema/target.h"
#include "support/term/terminal.h"

namespace minc::driver {

struct CheckRequest {
  std::vector<std::string> inputs; // paths, or "-" for standard input
  std::vector<std::pair<std::string, std::string>> defines;
  std::vector<std::string> undefines;
  std::vector<std::string> includeDirs;
  std::vector<std::string> systemDirs;
  // The ABI the type layout and the C spellings are read against.
  sema::Target target = sema::kDefaultTarget;
  // `--ast`: print the typed tree instead of the per-file summary.
  bool showAst = false;
  // `--types`: print only the type table.
  bool showTypes = false;
  // `-Wconversion`: warn when an implicit conversion may lose information.
  bool warnConversion = false;
  // `-Wunused`, `-Wshadow`: forwarded to resolution, which is where those two
  // are decided. A `check` that swallowed them would make the warnings a user
  // asked for depend on which command they happened to run.
  bool warnUnused = false;
  bool warnShadow = false;
  support::ColorMode diagnosticColor = support::ColorMode::Plain;
};

// The command minus the choice of streams, so the contract -- which stream
// carries what and which exit code -- is tested directly instead of by spawning
// a process.
//
// Precondition: `request.inputs` is not empty.
[[nodiscard]] int checkInputs(const CheckRequest& request, std::ostream& out, std::ostream& err);

// As the driver invokes it: `-` reads standard input, and the streams pick their
// own color mode from the real terminal.
[[nodiscard]] int runCheck(const CliOptions& options);

} // namespace minc::driver
