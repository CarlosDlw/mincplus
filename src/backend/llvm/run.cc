// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Running a program, which is what `run` is.
//
// The program is the binary `build` produced, executed the way the operating
// system executes one. That is the whole design and the whole argument
// (`codegen.md`, § *The one question that decides the shape*): an in-process JIT
// would run the program's code in the compiler's address space, so a wild
// pointer would kill the compiler instead of the program, `exit()` would tear
// down the compiler's process, and `run` would be a second implementation of
// `build`'s semantics with a chance to disagree with it.
//
// The spawn and the wait are `support/process`'s, and the header of that module
// carries why they are not LLVM's: `llvm::sys::ExecuteAndWait` reports a child
// that exited 126 or 127 the same way it reports one that never started, because
// the Unix side of it uses those two statuses as its own channel for a failed
// `exec`. `run` cannot afford that ambiguity -- a program's status is the one
// thing it exists to hand back -- so the platform is asked directly, by a module
// that may contain the platform branch and whose callers never learn which OS
// answered.
#include <string>
#include <utility>
#include <vector>

#include "backend/codegen.h"
#include "support/process/process.h"

namespace minc::backend {

RunResult runProgram(const RunOptions& options) {
  RunResult result;
  if (options.program.empty()) {
    result.spawnFailed = true;
    result.error = "no program to run";
    return result;
  }

  // `argv[0]` is the program's own path, exactly as if it had been invoked
  // directly (the support layer's contract). A program that prints `argv[0]`
  // prints what the shell would have given it, and everything after `--` is an
  // ordinary argument: the one thing `run` must never do is look at it, so
  // `mincc run p.mx -- -o --emit` passes two arguments to a program.
  const support::ProcessOutcome outcome = support::runAndWait(options.program, options.arguments);

  if (!outcome.started) {
    // Nothing ran. `error` is the platform's own reason, and a platform that gave
    // none is still a failure to start rather than a silent success.
    result.spawnFailed = true;
    result.error = outcome.error.empty() ? "the program could not be executed" : outcome.error;
    return result;
  }
  if (!outcome.exited) {
    // It ran and did not end by exiting: a signal, or -- on Windows -- an
    // unhandled exception. "Did not exit" is the portable truth, and the platform
    // layer answers it as a fact rather than as a number this stage has to
    // interpret (`abnormalTermination` used to be that interpretation, and it is
    // gone with the layer that made it necessary).
    result.crashed = true;
    result.error = outcome.error;
    return result;
  }
  // The program's status, verbatim: `run` is a launcher, not an interpreter, and
  // a harness that compares exit codes must see the program's. That includes the
  // two statuses a Unix child is *conventionally* not supposed to use: a program
  // whose `main` returns 127 exits 127, and this is the line that says so.
  result.exitCode = outcome.status;
  return result;
}

} // namespace minc::backend
