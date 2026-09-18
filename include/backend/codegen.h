// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Codegen: a module in, an object (or an assembly listing) out.
//
// The stage the pipeline puts between `ir` and the linker, and the last one that
// produces something other than a diagnostic. Its whole job is to select a
// `TargetMachine` from the triple the module already carries, run LLVM's pass
// pipeline, and hand the result to LLVM's emitter.
//
// Three properties shape this header.
//
// **It contains no LLVM.** Like `ir.h`, and for the same reason: the driver
// includes this file, and a driver that had to link `llvm/*` would put the
// boundary in a header instead of in the build graph. The module is a handle
// (`ir::Module`), the target is `sema::TargetInfo`, and the output is a path.
//
// **There is no semantic refusal.** The promise this project is built on is that
// a program the checker accepts must run, and a module that reached here has
// been verified and scanned; so a construct this stage cannot lower would mean
// `ir` emitted something it should not have -- an internal error, never an
// "unsupported feature". Every code below is therefore about the *environment*
// or about this compiler, and none of them is about the user's program.
//
// **The optimisation level is decided here, not in `ir`.** `ir` produces
// canonical IR and runs no passes of its own (`ir.md`); `-O` is a property of
// the build, and the pipeline that implements it is LLVM's.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ir/ir.h"
#include "sema/target.h"
#include "support/span/span.h"

namespace minc::backend {

// --- the build environment ------------------------------------------------------
//
// One fact `--version -v` prints and nothing else decides with: the LLVM this
// compiler was built against. It lives here because it is LLVM's own answer and
// this is the module allowed to ask.
//
// The host triple is *not* asked here. It used to be
// (`llvm::sys::getDefaultTargetTriple()`), and that was one host answer too many:
// the machine the compiler runs on is a property of the build, CMake states it in
// `sema/host.h`, and the spelling `--target` accepts is the one `sema` prints. A
// second answer from LLVM could only ever disagree with it about formatting --
// `x86_64-pc-linux-gnu` against `x86_64-unknown-linux-gnu`, `arm64` against
// `aarch64` -- which is exactly the difference a reader of `-vV` would have to
// spend time deciding was not a bug.

// The LLVM this compiler was built against, as its own version string.
[[nodiscard]] std::string llvmVersion();

// What to write. LLVM's own enumeration is `llvm::CodeGenFileType` --
// `AssemblyFile`, `ObjectFile`, `Null` -- and `None` is the third one with the
// name the command line uses.
enum class EmitKind : std::uint8_t {
  Object,
  Assembly,
  None,
};

// `-O`. A class in LLVM (`OptimizationLevel`) and an enum here, because the
// command line has six spellings and a caller should not be able to pass a level
// this compiler does not offer.
enum class OptLevel : std::uint8_t {
  O0,
  O1,
  O2,
  O3,
  Os,
  Oz,
};

[[nodiscard]] const char* toString(EmitKind kind);
[[nodiscard]] const char* toString(OptLevel level);
// `-O2` -> `OptLevel::O2`; `std::nullopt` for a spelling this compiler does not
// state, so the caller refuses rather than defaulting.
[[nodiscard]] std::optional<OptLevel> optLevelFromName(std::string_view name);
[[nodiscard]] std::optional<EmitKind> emitKindFromName(std::string_view name);

// Every way this stage can refuse, as a stable code.
//
// The classes are named apart because they have *opposite fixes*, which is the
// rule every earlier stage follows and the one this stage extends: an
// `ir-unsupported-*` wanted a feature and an `ir-internal` wanted a bug report,
// and here a `TargetUnavailable` wants an LLVM built with that backend while an
// `Internal` still wants a bug report. Reading the wrong one wastes a day.
enum class CodegenDiagnosticCode : std::uint8_t {
  // This LLVM has no code generator for the module's triple. `llvm-config
  // --targets-built` decides it, and a distribution LLVM routinely omits
  // backends. The fix is an LLVM, not a program change.
  TargetUnavailable,
  // The target has no printer for this file type (an object for a target that
  // only emits assembly). `addPassesToEmitFile` returning true is this.
  EmitUnsupported,
  // No C linker driver on the `PATH` and none named. `--linker PATH` fixes it.
  LinkerNotFound,
  // An executable was asked for a triple that is not the host's, without the
  // linker and sysroot that would make it meaningful.
  LinkerUnavailable,
  // The linker ran and failed. Its own diagnostics have already been printed;
  // this is the code that says so, and its message is the status.
  LinkFailed,
  // The output file could not be written: permissions, disk, a directory.
  ObjectWriteFailed,
  // A precondition this stage relies on was violated: no module, no output path,
  // an LLVM call that failed for no reason above. A bug in this compiler.
  Internal,
};

struct CodegenDiagnosticCodeInfo {
  CodegenDiagnosticCode code;
  const char* name;
};

[[nodiscard]] std::string_view toString(CodegenDiagnosticCode code);
[[nodiscard]] const char* nameOf(CodegenDiagnosticCode code);
// Every code, derived from the one table, so adding one without a row is caught
// by the enumeration test rather than by a user.
[[nodiscard]] const std::vector<CodegenDiagnosticCode>& allDiagnosticCodes();

struct CodegenDiagnostic {
  support::Span span;
  CodegenDiagnosticCode code = CodegenDiagnosticCode::Internal;
  std::string message;
};

// --- the stage ------------------------------------------------------------------

struct EmitOptions {
  EmitKind kind = EmitKind::Object;
  // Where the object or listing goes. Required for `Object` and `Assembly`;
  // ignored for `None`. The caller owns the temporary-directory lifetime, so
  // this stage never invents a path.
  std::string outputPath;
  OptLevel level = OptLevel::O0;
  // `-g`. Read, not decided: `ir` built the metadata, this stage only refuses to
  // lose it (`codegen.md`, § *Debug information*).
  bool debugInfo = false;
  // Print the commands LLVM runs (`-mllvm -debug-pass-manager`-style output is
  // not what this is): the stage's own trace, for `-v`.
  bool verbose = false;
  // Ask the backend's *machine* verifier to run. LLVM defaults this to off
  // (`DisableVerify = true` in `addPassesToEmitFile`), which is a silent
  // difference between a debug build and a release one; this project asks for it
  // and says so.
  bool verifyMachineCode = true;
};

struct EmitResult {
  std::vector<CodegenDiagnostic> diagnostics;
  // The exact `argv`-level description of what LLVM was asked to do, for `-v`
  // and for a test that wants to assert on the pipeline rather than the bytes.
  std::string trace;

  [[nodiscard]] bool failed() const {
    return !diagnostics.empty();
  }
};

// Emits one module.
//
// Precondition, enforced rather than trusted: `module.built()` and a non-empty
// `outputPath` for an emit kind that writes one. A module that was never built
// is a caller bug and is refused with `Internal`.
//
// The module's triple decides the target machine -- not the host, and not a
// parameter -- because `sema` already decided it and a second answer here would
// be a second spelling of one target (`ir.md`, decision 21).
//
// Non-const, deliberately: the optimiser rewrites the module in place, so a
// `const` parameter would be a lie about what this call does.
[[nodiscard]] EmitResult emitModule(ir::Module& module, const EmitOptions& options);

// The default file name for an executable on `target`: `a.exe` where the target
// is Windows and `a.out` everywhere else.
//
// Here and not in the driver because it is platform knowledge, and `architecture.md`
// keeps platform knowledge out of the driver -- the driver may not include an
// LLVM header to ask `Triple::isOSWindows`, so the stage that already links LLVM
// answers the question and the driver only prints the answer.
[[nodiscard]] std::string defaultExecutableName(const sema::TargetInfo& target);

// The object file name for a source path: the path with its extension replaced
// by `.o` (or `.s` for an assembly listing). Also here, and for the same reason:
// the suffix a platform's tools expect is the platform's.
[[nodiscard]] std::string defaultOutputPath(std::string_view input, EmitKind kind);

// True when this LLVM build can generate code for `target`. What `build` asks
// before it promises anything, so `--target` on a machine whose LLVM omits a
// backend is a sentence rather than a crash half-way through.
[[nodiscard]] bool targetAvailable(const sema::TargetInfo& target);
// Why not, as a sentence for a diagnostic; empty when it is available.
[[nodiscard]] std::string targetRefusal(const sema::TargetInfo& target);

// Registers LLVM's targets for code generation. Idempotent, and separate from
// `ir`'s registration on purpose: `ir` registers the target *infos* it needs for
// a data layout, and this registers the printers as well. Neither stage may
// depend on the other (`ir.md`, decision 13), so each has its own guard.
void initialize();

// --- linking --------------------------------------------------------------------

struct LinkRequest {
  std::vector<std::string> objects;     // relocatable objects to link
  std::string outputPath;               // the executable
  std::vector<std::string> libraries;   // `-l NAME`, in order
  std::vector<std::string> libraryDirs; // `-L DIR`, in order
  // Empty means "search": `clang`, then `cc`, then `gcc`.
  std::string linker;
  std::string sysroot;
  bool debugInfo = false;
  // The ABI the objects were built for. Linking is only driven for the host
  // triple (`codegen.md`, § *Cross-compiling*), so a foreign one is refused
  // unless a `--linker` and `--sysroot` were named.
  sema::TargetInfo target = sema::defaultTarget();
  // True when this run's host is the target. The driver answers this, because
  // only it knows whether the user asked to cross-compile.
  bool targetIsHost = true;
};

struct LinkResult {
  std::vector<CodegenDiagnostic> diagnostics;
  // The program that was (or would be) run, and its arguments, in order. Filled
  // even when the link fails, so `-v` can print what was attempted.
  std::vector<std::string> command;
  int exitCode = 0;

  [[nodiscard]] bool failed() const {
    return !diagnostics.empty();
  }
};

// The `argv` a link would run: the discovered (or named) driver, then the flags,
// then the objects. Separate from `linkExecutable` so a test can assert the
// command line without a toolchain, and so `-v` prints *the* command instead of a
// description of one.
[[nodiscard]] std::vector<std::string> linkCommandLine(const LinkRequest& request);

// Runs the linker. An empty `objects` is a caller bug and is refused.
[[nodiscard]] LinkResult linkExecutable(const LinkRequest& request);

// --- running --------------------------------------------------------------------

struct RunOptions {
  std::string program;
  std::vector<std::string> arguments; // everything after `--`, untouched
};

struct RunResult {
  // The child's exit status, or 0 when it did not exit normally.
  int exitCode = 0;
  // The child did not exit on its own -- a signal on POSIX, an unhandled
  // exception on Windows. Named as a boolean rather than as a signal number
  // because naming the signal is platform code, and no stage may contain any.
  // "Crashed" is the portable truth, and the driver reports it as such.
  //
  // The fact comes *back* from `support/process` rather than being inferred from
  // the status: this project used to read `llvm::sys`'s `-1`/`-2`/negative-number
  // convention, which could not tell a program that exited 127 from one that never
  // ran (`support/process/process.h` has the whole argument).
  bool crashed = false;
  // The program could not be executed at all (missing file, not executable, no
  // permission). Distinct from a crash: nothing ran.
  bool spawnFailed = false;
  // Why it could not be executed, from the platform layer -- or, when it ran and
  // did not exit, why its ending could not be learned. Empty on success.
  std::string error;

  [[nodiscard]] bool normalExit() const {
    return !crashed && !spawnFailed;
  }
};

// Executes a built program as a child process, with the three standard streams
// inherited.
//
// This is what `run` is: the same binary `build` produces, executed the way the
// operating system executes one. Argument forwarding, stdio, the exit status and
// a signal death are all the child's, which is the whole reason `run` is not an
// in-process JIT (`codegen.md`, § *The one question that decides the shape*).
[[nodiscard]] RunResult runProgram(const RunOptions& options);

// --- files ----------------------------------------------------------------------

// A directory that removes itself, for the objects and the executable of one
// `build`/`run` invocation.
//
// `llvm::sys::fs::createUniqueFile` is what creates the directory's files
// (`O_CREAT|O_EXCL` and a random suffix), so the create is atomic and a
// predictable path in a world-writable directory is never one somebody else
// chose. The destructor removes the tree, on the error paths too -- which is the
// half that is usually forgotten.
class TempDir {
public:
  TempDir();
  ~TempDir();
  TempDir(TempDir&&) noexcept;
  TempDir& operator=(TempDir&&) noexcept;
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] bool valid() const {
    return !path_.empty();
  }
  // Empty when the directory could not be created.
  [[nodiscard]] const std::string& path() const {
    return path_;
  }
  // `path()/name`, using the host's separator rules. A `Twine` in LLVM; a
  // `std::string` here so the caller never sees LLVM.
  [[nodiscard]] std::string file(std::string_view name) const;

private:
  std::string path_;
};

} // namespace minc::backend
