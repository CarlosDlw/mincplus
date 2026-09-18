// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The link, which this compiler does not perform.
//
// `build` drives a linker **driver** -- `clang`, `cc`, `gcc` -- and never a
// linker. The reason is not laziness and it is worth restating where the code
// lives: linking an executable by hand means knowing, per host, the startup
// objects, the dynamic linker path, `libc`, `libm`, the compiler runtime, the
// macOS SDK and `-lSystem`, and the MSVC import libraries and subsystem flags on
// Windows. Every one of those is a place to be subtly wrong, and every one of
// them is already right inside a C driver. `codegen.md` § *The link* has the
// full argument; the short version is that a second implementation of platform
// knowledge is a second set of ways to be wrong.
//
// Three properties of the invocation are the point of this file:
//
// 1. **An `argv` array, never a shell string.** `support::runAndWait` hands these
//    exact words to the platform, so there is nothing to quote, no `$` to expand,
//    and no command injection -- and none of the Windows quoting rules for a path
//    with a space in it to get wrong. The user's paths are passed as data,
//    because they are data. (The *one* place those rules exist is inside that
//    module, where the platform requires a command line instead of an array.)
// 2. **The child's streams are inherited and its status is the answer.** Its own
//    message has already been printed in its own format; parsing that text would
//    be a second implementation of someone else's diagnostics, and swallowing it
//    would leave a user with nothing.
// 3. **The driver is discovered with the platform's rules**
//    (`findProgramByName`, including `PATHEXT` on Windows), so a `.exe` suffix
//    and a `PATH` separator are the platform's business and not ours.
//
// The spawn is `support/process`'s for one reason beyond sharing: a linker driver
// that exits 127 is a linker driver that *ran and reported 127*, and
// `llvm::sys::ExecuteAndWait` could not tell that apart from a driver that never
// started. Both are failures, but only one of them is "install a C toolchain".
#include <cstddef>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"

#include "backend/codegen.h"
#include "support/process/process.h"

namespace minc::backend {
namespace {

void add(std::vector<CodegenDiagnostic>& out, CodegenDiagnosticCode code, std::string message) {
  CodegenDiagnostic diagnostic;
  diagnostic.code = code;
  diagnostic.message = std::move(message);
  out.push_back(std::move(diagnostic));
}

// The search order, and the reason for it: `clang` is the same LLVM generation
// as this compiler, understands every target this stage emits, and on Windows
// finds and drives `link.exe` correctly while translating the Unix `-l`/`-L`
// spellings. `cc` is the system's C compiler and is present wherever a C
// toolchain is; `gcc` covers a system whose `cc` is missing.
constexpr const char* kDrivers[] = {"clang", "cc", "gcc"};

// The resolved path of the driver, or nothing. A named driver is looked up on
// the `PATH` first and then accepted as a path, because `--linker ./ld-wrapper`
// names a file rather than a program name and a user who wrote a path meant it.
[[nodiscard]] std::optional<std::string> resolveDriver(const std::string& named) {
  if (!named.empty()) {
    const llvm::ErrorOr<std::string> found = llvm::sys::findProgramByName(named);
    if (found) {
      return *found;
    }
    if (llvm::sys::fs::exists(named)) {
      return named;
    }
    return std::nullopt;
  }
  for (const char* candidate : kDrivers) {
    const llvm::ErrorOr<std::string> found = llvm::sys::findProgramByName(candidate);
    if (found) {
      return *found;
    }
  }
  return std::nullopt;
}

} // namespace

std::vector<std::string> linkCommandLine(const LinkRequest& request) {
  std::vector<std::string> command;
  // `argv[0]` is the resolved driver when one was found, and the *name* the user
  // gave (or the first candidate) otherwise -- so `-v` prints the command that
  // will be attempted rather than an empty string, and a failure is diagnosable
  // from the line above it.
  const std::optional<std::string> driver = resolveDriver(request.linker);
  command.push_back(
      driver.value_or(request.linker.empty() ? std::string(kDrivers[0]) : request.linker));

  command.emplace_back("-o");
  command.push_back(request.outputPath);
  if (request.debugInfo) {
    // `clang`/`gcc` translate this for the platform's linker (`/DEBUG` on
    // Windows), which is one more thing the driver absorbs for us.
    command.emplace_back("-g");
  }
  if (!request.sysroot.empty()) {
    command.push_back("--sysroot=" + request.sysroot);
  }
  // Directories before libraries and objects before libraries: a link resolves
  // left to right, and an archive that appears before the object that needs it
  // contributes nothing.
  for (const std::string& dir : request.libraryDirs) {
    command.push_back("-L" + dir);
  }
  for (const std::string& object : request.objects) {
    command.push_back(object);
  }
  for (const std::string& library : request.libraries) {
    command.push_back("-l" + library);
  }
  return command;
}

LinkResult linkExecutable(const LinkRequest& request) {
  LinkResult result;

  if (request.objects.empty()) {
    add(result.diagnostics, CodegenDiagnosticCode::Internal,
        "a link was requested with no objects; there is nothing to link");
    return result;
  }
  if (request.outputPath.empty()) {
    add(result.diagnostics, CodegenDiagnosticCode::Internal,
        "a link was requested with no output path");
    return result;
  }

  // Cross-linking is refused before anything is spawned. Without a sysroot we
  // would hand the linker objects for one platform and libraries for another,
  // and the failure -- if there were one -- would be a link error about `libc`
  // that says nothing about the actual mistake (`codegen.md`, decision 12).
  if (!request.targetIsHost && (request.linker.empty() || request.sysroot.empty())) {
    add(result.diagnostics, CodegenDiagnosticCode::LinkerUnavailable,
        "linking an executable for `" + request.target.name() +
            "` on this host needs `--linker PATH` and `--sysroot DIR`, because the target's C "
            "runtime is not the host's; `--emit=obj` cross-compiles without either");
    return result;
  }

  const std::optional<std::string> driver = resolveDriver(request.linker);
  result.command = linkCommandLine(request);
  if (!driver.has_value()) {
    const std::string wanted =
        request.linker.empty() ? std::string("clang, cc or gcc") : "`" + request.linker + "`";
    add(result.diagnostics, CodegenDiagnosticCode::LinkerNotFound,
        "no C linker driver found (" + wanted +
            "); install a C toolchain on PATH, or name one with `--linker PATH`");
    return result;
  }

  std::vector<std::string> argv = std::move(result.command);
  argv[0] = *driver;
  result.command = argv;

  // `argv[0]` is the resolved path, and the rest are the words unchanged: what
  // runs is what `-v` printed, modulo the resolution itself.
  const support::ProcessOutcome outcome =
      support::runAndWait(*driver, std::vector<std::string>(argv.begin() + 1, argv.end()));

  if (!outcome.started) {
    add(result.diagnostics, CodegenDiagnosticCode::LinkerNotFound,
        "cannot run the linker driver `" + *driver + "`" +
            (outcome.error.empty() ? std::string{} : ": " + outcome.error));
    return result;
  }
  // A driver that ran and did not exit on its own: a signal, or an unhandled
  // exception on Windows. Without this branch a linker that died of an access
  // violation would be reported as "exited with status <negative>", which reads
  // as a status the driver chose.
  if (!outcome.exited) {
    add(result.diagnostics, CodegenDiagnosticCode::LinkFailed,
        "the linker driver `" + *driver + "` crashed" +
            (outcome.error.empty() ? std::string{} : ": " + outcome.error));
    result.exitCode = 1;
    return result;
  }
  const int status = outcome.status;
  if (status != 0) {
    // **The program's failure, and the linker's message is already on stderr.**
    // We do not paraphrase it: the linker's text is more precise than anything
    // this stage could write, and re-formatting it is a permanent maintenance
    // cost that buys nothing.
    add(result.diagnostics, CodegenDiagnosticCode::LinkFailed,
        "the linker driver `" + *driver + "` exited with status " + std::to_string(status));
    result.exitCode = status;
    return result;
  }

  result.exitCode = 0;
  return result;
}

} // namespace minc::backend
