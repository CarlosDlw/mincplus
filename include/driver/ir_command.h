// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// `mincc ir <files...>`: the front end, and the LLVM module it lowers to.
//
// It is the command that proves the `ir` stage the way `check` proves `sema`:
// it runs the same pipeline, lowers each unit that checked clean, prints the
// module, and *scans* it against the rules the language promised to obey. A
// violation of one of those rules is not a diagnostic about the program -- it is
// this compiler being wrong -- so the command reports it as an internal error
// and fails, which is what makes `make examples` a real test of the lowering and
// not just of the printer.
//
// A unit with any error produces nothing on stdout. That is the same contract
// `check` keeps and it matters more here: a module built from a tree that had an
// error is a module nobody decided the meaning of, and handing one to a linker
// is the outcome the whole stage is arranged to prevent.
#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

#include "driver/cli.h"
#include "sema/target.h"
#include "support/limits.h"
#include "support/term/terminal.h"

namespace minc::driver {

struct IrRequest {
  std::vector<std::string> inputs; // paths, or "-" for standard input
  std::vector<std::pair<std::string, std::string>> defines;
  std::vector<std::string> undefines;
  std::vector<std::string> includeDirs;
  std::vector<std::string> systemDirs;
  sema::TargetInfo target = sema::defaultTarget();
  // Forwarded to the front end, so a warning a user asked for does not depend
  // on which command they happened to run. The exit code ignores warnings, but a
  // diagnostic a user asked for and did not get is worse than a noisy one.
  bool warnConversion = false;
  bool warnCast = false;
  bool warnProvenance = false;
  bool warnUnused = false;
  bool warnShadow = false;
  support::ColorMode diagnosticColor = support::ColorMode::Plain;
  // `-ferror-limit`: how many errors are shown before rendering stops. See
  // `RenderOptions::errorLimit`.
  std::size_t errorLimit = support::kMaxDiagnostics;
  // `-g`. Prints the same module with debug metadata attached, which is how the
  // line table is reviewed as text rather than through a debugger.
  bool debugInfo = false;
  // The checked build's guards (`-fcheck`/`-fno-check`). Off in a hand-built
  // request, like `BuildRequest`'s, and **on** for the command itself: `runIr`
  // resolves it the way `build` resolves `-O0`, because the whole point of this
  // command is that the module it prints is the module a build emits -- and a
  // reader hunting a trap they saw has to be looking at the module that carries
  // it.
  bool checks = false;
};

// The command minus the choice of streams, so the contract -- which stream
// carries what and which exit code -- is tested directly instead of by spawning
// a process.
//
// Precondition: `request.inputs` is not empty.
[[nodiscard]] int irInputs(const IrRequest& request, std::ostream& out, std::ostream& err);

// As the driver invokes it: `-` reads standard input, and the streams pick their
// own color mode from the real terminal.
[[nodiscard]] int runIr(const CliOptions& options);

} // namespace minc::driver
