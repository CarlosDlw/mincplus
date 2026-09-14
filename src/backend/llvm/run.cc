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
// The platform split this needs -- `fork`/`exec`/`waitpid` on one side,
// `CreateProcess`/`GetExitCodeProcess` on the other -- is LLVM's to make, not
// this module's: `architecture.md` names `support/term` as the one module that
// may contain `#if defined(_WIN32)`, and a second one would need an argument
// rather than an accident. `llvm::sys::ExecuteAndWait` is that layer.
#include <string>
#include <utility>
#include <vector>

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Program.h"

#include "backend/codegen.h"

namespace minc::backend {

RunResult runProgram(const RunOptions& options) {
  RunResult result;
  if (options.program.empty()) {
    result.spawnFailed = true;
    result.error = "no program to run";
    return result;
  }

  llvm::SmallVector<llvm::StringRef, 16> arguments;
  arguments.reserve(options.arguments.size() + 1);
  // `argv[0]` is the program's own path, exactly as if it had been invoked
  // directly. A program that prints `argv[0]` prints what the shell would have
  // given it.
  arguments.emplace_back(options.program);
  for (const std::string& argument : options.arguments) {
    // Interpreted by nobody: everything after `--` is an ordinary argument, and
    // the one thing `run` must never do is look at it. `mincc run p.mx -- -o
    // --emit` passes two arguments to a program.
    arguments.emplace_back(argument);
  }

  std::string errorMessage;
  bool executionFailed = false;
  const int status = llvm::sys::ExecuteAndWait(options.program, arguments, /*Env=*/std::nullopt,
                                               /*Redirects=*/{}, /*SecondsToWait=*/0,
                                               /*MemoryLimit=*/0, &errorMessage, &executionFailed);

  if (executionFailed || status == -1) {
    result.spawnFailed = true;
    result.error = errorMessage.empty() ? "the program could not be executed" : errorMessage;
    return result;
  }
  // `-2` is "a crash during execution or timeout", which is how the portable
  // layer reports a signal death. Naming the signal would take `waitpid` on one
  // platform and `GetExitCodeProcess` on the other, which is the platform code
  // this module may not contain.
  if (status == -2) {
    result.crashed = true;
    return result;
  }
  result.exitCode = status;
  return result;
}

} // namespace minc::backend
