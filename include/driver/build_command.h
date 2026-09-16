// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// `mincc build` and `mincc run`: the first commands that produce something other
// than a diagnostic, and the first that reach a linker.
//
// `run` is not a separate pipeline. It is `build` into a temporary executable,
// then an `exec` of that binary -- one code path, so a bug cannot exist in `run`
// and not in `build`. `codegen.md` § *The one question that decides the shape*
// has the argument for why `run` is not an in-process JIT; the short version is
// that a program with a wild pointer must kill the *program*, not the compiler.
//
// The shape of the stage is a straight line and everything unusual is on one of
// its four corners:
//
// 1. **The front end runs once, for every input.** The same `FrontEnd` `check`
//    and `ir` use, so a file `check` rejects cannot be accepted here.
// 2. **A unit with any error produces no object.** A module was never built, so
//    there is nothing to emit -- and a link that half-succeeds would be a
//    mangled executable that reports success.
// 3. **The invariant scan runs before emission, not after.** A module that
//    violates a rule of the language is a *miscompile* waiting to happen; scanning
//    it below `codegen` would mean the object was already written.
// 4. **Every temporary lives in a `TempDir`.** Deterministic paths in a
//    world-writable directory are how one compiler's object becomes another's.
#pragma once

#include <cstddef>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "backend/codegen.h"
#include "driver/cli.h"
#include "sema/target.h"
#include "support/limits.h"
#include "support/term/terminal.h"

namespace minc::driver {

// What the command produces. One enum and not `backend::EmitKind`, because the
// *command's* choices and the *emitter's* are different questions: `exe` is an
// object plus a link, so three command values map onto two emitter values with
// the link implied. Collapsing them would make "did the user want a link?" a
// question about an emitter enum.
enum class OutputKind : std::uint8_t {
  Executable,
  Object,
  Assembly,
};

[[nodiscard]] const char* toString(OutputKind kind);
// `exe`/`obj`/`asm`, or nothing for a spelling this compiler does not state.
[[nodiscard]] std::optional<OutputKind> outputKindFromName(std::string_view name);

struct BuildRequest {
  std::vector<std::string> inputs; // paths, or "-" for standard input
  std::vector<std::pair<std::string, std::string>> defines;
  std::vector<std::string> undefines;
  std::vector<std::string> includeDirs;
  std::vector<std::string> systemDirs;
  sema::TargetInfo target = sema::defaultTarget();
  // Forwarded to the front end, so a warning a user asked for does not depend on
  // which command they ran.
  bool warnConversion = false;
  bool warnCast = false;
  bool warnProvenance = false;
  bool warnUnused = false;
  bool warnShadow = false;
  support::ColorMode diagnosticColor = support::ColorMode::Plain;
  // `-ferror-limit`: how many errors are shown before rendering stops. See
  // `RenderOptions::errorLimit`.
  std::size_t errorLimit = support::kMaxDiagnostics;

  // What to produce, and how.
  OutputKind kind = OutputKind::Executable;
  backend::OptLevel level = backend::OptLevel::O0;
  // Empty means the default for `kind`: one object per input, or an executable
  // named by the target's platform.
  std::string output;
  bool debugInfo = false;
  bool verbose = false;
  std::vector<std::string> libraryDirs;
  std::vector<std::string> libraries;
  std::string linker;
  std::string sysroot;

  // What `run` does after the build, and with what. Empty and unused for `build`.
  bool run = false;
  std::vector<std::string> programArguments;
};

// Compiles and (unless `kind` is `Object`/`Assembly`) links. Returns the process
// exit code: 0 on success, 1 when a diagnostic was printed, 2 for a bad command
// line. Writes nothing to stdout: the artifacts are files, and only `-v`'s trace
// goes to stderr.
//
// Precondition: `request.inputs` is not empty.
[[nodiscard]] int buildInputs(const BuildRequest& request, std::ostream& err);

// Builds, then runs the result and reports the program's own status.
[[nodiscard]] int runInputs(const BuildRequest& request, std::ostream& err);

// As the driver invokes them.
[[nodiscard]] int runBuild(const CliOptions& options);
[[nodiscard]] int runRun(const CliOptions& options);

} // namespace minc::driver
