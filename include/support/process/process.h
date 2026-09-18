// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Starting a program and waiting for it: the one platform question the compiler
// has that no LLVM API answers faithfully.
//
// The compiler runs exactly two children -- the program `run` built, and the C
// linker driver `build` drives -- and both need the same three facts from the
// platform: *did it start*, *how did it end*, and, when it did not start, *why*.
//
// It used to ask `llvm::sys::ExecuteAndWait`, and that API cannot answer the
// first two. `llvm/lib/Support/Unix/Program.inc` hands the child a convention for
// a failed `exec` -- `_exit(errno == ENOENT ? 127 : 126)` -- and then its `Wait`
// maps those two statuses back to `-1` ("could not execute") with an error string
// attached. The two cases are indistinguishable afterwards: a program that
// *returned* 127 and a program that never ran produce the same pair. So `mincc
// run` reported a program whose `main` returned 127 as one that could not be
// started at all -- a real program, a real status, silently replaced by a lie. On
// Windows the same layer is lossy in a different place (`status & 0xFF == 0` is
// answered with `1`, so a program exiting 256 is reported as exiting 1).
//
// An API cannot be repaired from outside: the information is gone by the time the
// call returns. So the compiler owns the spawn -- `posix_spawn` + `waitpid` on one
// side, `CreateProcessW` + `GetExitCodeProcess` on the other -- and asks the
// platform directly. Reading LLVM's *messages* to guess which of the two cases
// happened is the tempting non-fix and is refused on purpose: those strings are
// not an interface, they are prose, and a correction that depends on prose is a
// bug waiting for a release note.
//
// This is the fourth file in the tree that contains a platform branch
// (`architecture.md`), beside `support/term`, `support/fs` and
// `support/source/file_io`, and it exists for the same reason as the other three:
// a question is asked, an answer comes back, and no caller learns which OS
// answered.
#pragma once

#include <string>
#include <vector>

namespace minc::support {

// How a child process ended, as far as the platform is willing to say.
struct ProcessOutcome {
  // The program was started. False means **nothing ran**: the file is missing, is
  // not executable, is not an image this platform can load, or the platform
  // refused to create the process. `error` says which.
  bool started = false;
  // Why it did not start, or -- when it started but this layer could not learn
  // how it ended -- why its ending is unknown. Empty otherwise.
  std::string error;
  // It ended by exiting, so `status` is the status the program itself produced.
  // False when it was started and did not exit on its own: a signal on POSIX, an
  // unhandled exception on Windows. "Did not exit" and "did not start" are two
  // different facts, and this flag is what keeps them apart -- the distinction
  // `run` exists to report (`codegen.md`).
  bool exited = false;
  // The status, meaningful only when `exited`. Eight significant bits on POSIX
  // and 32 on Windows, exactly as each platform's own waiting call reports it --
  // this layer does not normalize, because normalizing is how a status becomes a
  // different one (`llvm::sys::Wait`'s Windows mapping is the example: a program
  // exiting `0x100` is reported there as exiting `1`).
  int status = 0;
};

// Runs `program` with `arguments`, with `program` as the child's own `argv[0]`,
// the three standard streams inherited, and no timeout, and waits for it to
// finish.
//
// There is no shell in between: `arguments` are passed as words the platform
// hands to the child verbatim, so a path with a space, a `$`, or a quote in it is
// a path and not syntax. Inheriting the streams is what makes `run` a launcher
// rather than an interpreter -- the child writes to the user's terminal in the
// child's own format.
[[nodiscard]] ProcessOutcome runAndWait(const std::string& program,
                                        const std::vector<std::string>& arguments);

} // namespace minc::support
