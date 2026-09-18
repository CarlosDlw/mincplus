// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// `build` and `run`, which are one function with a flag.
//
// Everything that goes wrong here is one of three things, and keeping them apart
// is most of what the file does:
//
// - **The command line was wrong.** Exit 2, and the message names the option and
//   the alternatives. A `-o` with several objects, `--emit` of an unknown kind, a
//   `--target` this LLVM cannot generate code for.
// - **The program was wrong.** The front end and the lowering already reported
//   it, in their own formats, with carets. Nothing here re-formats any of it.
// - **The environment was wrong.** No linker driver on `PATH`, a target whose
//   backend this LLVM was built without, a directory that cannot be written.
//
// The one ordering that is not negotiable: **the invariant scan runs before the
// object is written.** A module that violates a rule of the language is a
// miscompile waiting to happen, and a scan *after* `emitModule` would leave the
// object on disk for a linker to find.
#include "driver/build_command.h"

#include <cstddef>
#include <iosfwd>
#include <iostream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "backend/codegen.h"
#include "driver/diagnostic_options.h"
#include "driver/error_report.h"
#include "driver/exit_code.h"
#include "driver/frontend.h"
#include "driver/stage_report.h"
#include "driver/version.h"
#include "ir/invariants.h"
#include "ir/ir.h"
#include "sema/target.h"
#include "support/fs/fs.h"
#include "support/source/source_manager.h"
#include "support/term/terminal.h"

namespace minc::driver {

const char* toString(OutputKind kind) {
  switch (kind) {
  case OutputKind::Executable:
    return "exe";
  case OutputKind::Object:
    return "obj";
  case OutputKind::Assembly:
    return "asm";
  }
  return "exe";
}

std::optional<OutputKind> outputKindFromName(std::string_view name) {
  if (name == "exe") {
    return OutputKind::Executable;
  }
  if (name == "obj") {
    return OutputKind::Object;
  }
  if (name == "asm") {
    return OutputKind::Assembly;
  }
  return std::nullopt;
}

namespace {

// The emitter's answer to the command's. `Object` is what both `exe` and `obj`
// codegen to; the link is what makes the difference, and it stays in this file.
[[nodiscard]] backend::EmitKind emitKindOf(OutputKind kind) {
  return kind == OutputKind::Assembly ? backend::EmitKind::Assembly : backend::EmitKind::Object;
}

// A command line as a reader would type it: arguments with a space or a quote
// are quoted, so the line `-v` prints can be pasted into a shell and does the
// same thing. Purely for display -- what runs is the `argv` array.
[[nodiscard]] std::string describeCommand(const std::vector<std::string>& argv) {
  std::string out;
  for (const std::string& argument : argv) {
    if (!out.empty()) {
      out += ' ';
    }
    const bool needsQuotes =
        argument.empty() || argument.find_first_of(" \t\"'\\$`") != std::string::npos;
    if (needsQuotes) {
      out += '"';
      for (const char c : argument) {
        if (c == '"' || c == '\\') {
          out += '\\';
        }
        out += c;
      }
      out += '"';
    } else {
      out += argument;
    }
  }
  return out;
}

// One object per unit, in the run directory, for the link. Named by index rather
// than after the input: two inputs may share a base name (`a/x.mx`, `b/x.mx`), and
// a collision in a directory the linker reads is a wrong program with no
// diagnostic anywhere.
[[nodiscard]] std::string tempObjectName(std::size_t index) {
  return "unit" + std::to_string(index) + ".o";
}

} // namespace

namespace {

int compile(const BuildRequest& request, bool execute, std::ostream& err) {
  // Before anything else, because everything else asks it a question: `codegen`
  // reads a registry that is empty until this runs, and the refusal for a target
  // whose backend is missing would otherwise be "LLVM will not build a machine"
  // for a target LLVM supports perfectly.
  backend::initialize();

  // Built once from the request: every diagnostic below -- this stage's, the
  // lowering's, the invariant scan's, the linker's -- is rendered through the
  // same options, so `-ferror-limit` and the tab width cannot differ between the
  // first error and the last.
  const support::RenderOptions diag =
      diagnosticOptions(request.diagnosticColor, request.errorLimit);

  if (!backend::targetAvailable(request.target)) {
    const std::vector<backend::CodegenDiagnostic> diagnostics{backend::CodegenDiagnostic{
        support::Span{}, backend::CodegenDiagnosticCode::TargetUnavailable,
        backend::targetRefusal(request.target)}};
    renderStageDiagnostics(diagnostics, diag, err);
    return exitCode(ExitCode::Failure);
  }

  // The output naming, decided once. `-o -` means standard output and only makes
  // sense when nothing has to be linked: an executable is a file the operating
  // system loads, and there is no such thing as linking one to a pipe.
  const bool stdoutOutput = request.output == "-";
  if (stdoutOutput && request.kind == OutputKind::Executable) {
    return usageError(err, "'-o -' cannot write an executable; name a file");
  }
  if (stdoutOutput && request.inputs.size() > 1) {
    return usageError(err, "'-o -' writes one file; give it a single input");
  }
  if (!request.output.empty() && !stdoutOutput && request.kind != OutputKind::Executable &&
      request.inputs.size() > 1) {
    return usageError(err, "'-o' with " + std::to_string(request.inputs.size()) +
                               " inputs would write each one to the same file; name a directory or "
                               "compile them one at a time");
  }

  // The temporary directory, created before the front end runs so a failure to
  // create it is reported before a minute of compilation is spent. Unused (and
  // uncreated) when the objects go where the user said.
  const bool needsTemp = execute || request.kind == OutputKind::Executable;
  backend::TempDir tempDir;
  if (needsTemp && !tempDir.valid()) {
    return usageError(err, "cannot create a temporary directory for the build");
  }

  // The front end. One instance for the whole invocation, so a type decided in
  // the first input is the same `TypeId` in the second -- which matters here more
  // than in `check`, because two units are about to become one program.
  FrontEndOptions frontEndOptions;
  frontEndOptions.defines = request.defines;
  frontEndOptions.undefines = request.undefines;
  frontEndOptions.includeDirs = request.includeDirs;
  frontEndOptions.systemDirs = request.systemDirs;
  frontEndOptions.target = request.target;
  frontEndOptions.warnConversion = request.warnConversion;
  frontEndOptions.warnCast = request.warnCast;
  frontEndOptions.warnProvenance = request.warnProvenance;
  frontEndOptions.warnUnused = request.warnUnused;
  frontEndOptions.warnShadow = request.warnShadow;
  frontEndOptions.diagnosticColor = request.diagnosticColor;

  FrontEnd frontEnd(frontEndOptions);
  bool ok = frontEnd.run(request.inputs, err);
  if (!ok) {
    // The diagnostics are already on `err`, in the format every other stage uses.
    // Nothing is printed here, and **nothing is emitted**: a link that ran with
    // the objects it managed to produce would be a program nobody asked for.
    return exitCode(ExitCode::Failure);
  }

  const support::SourceManager& sources = frontEnd.session().sources();
  std::vector<std::string> objects;
  std::size_t emitted = 0;

  for (const FrontEndUnit& unit : frontEnd.units()) {
    if (unit.lowered == nullptr || unit.resolved == nullptr || unit.typed == nullptr ||
        unit.errors != 0) {
      // A unit the front end refused contributes no object. `ok` is already false
      // in that case and the function returned above, so reaching this is the
      // defensive half: it keeps a future change to the front end's contract from
      // turning into an object built from an unchecked tree.
      ok = false;
      continue;
    }

    ir::LoweringOptions loweringOptions;
    loweringOptions.debugInfo = request.debugInfo;
    loweringOptions.producer = producerString();
    // The checked build. Read from the request, which is where `-fcheck` and the
    // `-O0` default were resolved, so this stage has one answer to materialise and
    // no level to interpret (`checks.md`).
    loweringOptions.checks = request.checks;
    // The source file the spans index into. Null for a unit that never reached
    // the lowering, which cannot happen here because the front end refused it.
    loweringOptions.source = sources.find(unit.file);
    // Every file of the compilation, for the guards' site messages: a guard on an
    // access inside an included header has to name that header, and the unit's own
    // path would be a true sentence about the wrong file.
    loweringOptions.sources = &sources;

    ir::IRResult lowered =
        ir::lowerUnit(*unit.lowered, unit.resolved->map, unit.typed->typed, frontEnd.sema().types(),
                      frontEnd.symbols(), loweringOptions);
    if (lowered.failed()) {
      renderStageDiagnostics(lowered.diagnostics, sources, diag, err);
      ok = false;
      continue;
    }

    // **Before** emission, never after: a violated invariant means this compiler
    // emitted something it promised not to, and the object must not exist. The
    // module is printed beside the violation so the reader can see what happened
    // -- to stderr, because stdout is where the object is going when `-o -` is
    // the answer.
    // The scan is told how the module was built: an unchecked module is *supposed*
    // to carry unguarded accesses, and a checked one is not (`ir/invariants.h`).
    const std::vector<ir::IRDiagnostic> violations =
        ir::scanModule(lowered.module, ir::ScanOptions{request.checks});
    if (!violations.empty()) {
      renderStageDiagnostics(violations, sources, diag, err);
      ok = false;
      continue;
    }

    // Where this unit's object goes. The order matters: an `exe` build with `-o`
    // names the *executable*, so the objects still go to the temporary directory --
    // writing every object to `-o` and then linking that one file would be a link
    // of the last unit against itself.
    std::string path;
    if (request.kind == OutputKind::Executable || execute) {
      path = tempDir.file(tempObjectName(emitted));
    } else if (stdoutOutput) {
      path = "-";
    } else if (!request.output.empty()) {
      path = request.output; // one input, one `-o`; checked above
    } else if (unit.path.empty() || unit.path == "-") {
      // Standard input has no name to derive one from, and inventing one would
      // write a file in the user's directory that they never asked for.
      return usageError(err, "standard input needs '-o FILE' to name the " +
                                 std::string(toString(request.kind)) + " file");
    } else {
      path = backend::defaultOutputPath(unit.path, emitKindOf(request.kind));
    }
    if (path.empty()) {
      return usageError(err, "cannot name an output file for '" + unit.path + "'");
    }

    backend::EmitOptions emitOptions;
    emitOptions.kind = emitKindOf(request.kind);
    emitOptions.outputPath = path;
    emitOptions.level = request.level;
    emitOptions.debugInfo = request.debugInfo;
    emitOptions.verbose = request.verbose;

    backend::EmitResult emittedResult = backend::emitModule(lowered.module, emitOptions);
    if (emittedResult.failed()) {
      renderStageDiagnostics(emittedResult.diagnostics, diag, err);
      ok = false;
      continue;
    }
    if (request.verbose && !emittedResult.trace.empty()) {
      err << emittedResult.trace << '\n';
    }
    ++emitted;
    if (emitOptions.kind == backend::EmitKind::Object) {
      objects.push_back(path);
    }
  }

  if (!ok) {
    return exitCode(ExitCode::Failure);
  }
  if (objects.empty() && request.kind == OutputKind::Executable) {
    // No inputs, or none the front end produced a unit for. A usage problem and
    // not an internal one, and the sentence says which.
    return usageError(err, "no input produced an object to link");
  }

  // Nothing to link: `--emit obj` and `--emit asm` are done, and the files are
  // where the user asked for them.
  if (request.kind != OutputKind::Executable && !execute) {
    return exitCode(ExitCode::Ok);
  }

  // The executable's own path. For `run` it is inside the temporary directory,
  // named with the target's suffix so the platform's loader finds it.
  const std::string executable =
      !request.output.empty() && !stdoutOutput
          ? request.output
          : (request.kind == OutputKind::Executable && !execute
                 ? backend::defaultExecutableName(request.target)
                 : tempDir.file(backend::defaultExecutableName(request.target)));

  backend::LinkRequest linkRequest;
  linkRequest.objects = objects;
  linkRequest.outputPath = executable;
  linkRequest.libraries = request.libraries;
  linkRequest.libraryDirs = request.libraryDirs;
  linkRequest.linker = request.linker;
  linkRequest.sysroot = request.sysroot;
  linkRequest.debugInfo = request.debugInfo;
  linkRequest.target = request.target;
  // The host is the *target* when the two have the same ABI.
  //
  // Not a comparison of the triple text: `--target x86_64-pc-linux-gnu` builds
  // for the machine this process runs on exactly as the default
  // `x86_64-unknown-linux-gnu` does, and comparing the strings would classify one
  // of them as a cross build and refuse to link it. The vendor component is part
  // of the target's *identity* (it is what `codegen` hands to LLVM) but not part
  // of its ABI, so `sameAbi` is what the question "is this the machine I am on?"
  // actually asks.
  linkRequest.targetIsHost = sema::sameAbi(request.target, sema::defaultTarget());

  if (request.verbose) {
    // Printed even when the link fails: the first thing anyone does with a link
    // failure is look at the command, and this is that command.
    err << describeCommand(backend::linkCommandLine(linkRequest)) << '\n';
  }

  const backend::LinkResult linkResult = backend::linkExecutable(linkRequest);
  if (linkResult.failed()) {
    renderStageDiagnostics(linkResult.diagnostics, diag, err);
    return exitCode(ExitCode::Failure);
  }

  if (!execute) {
    return exitCode(ExitCode::Ok);
  }

  backend::RunOptions runOptions;
  runOptions.program = executable;
  runOptions.arguments = request.programArguments;
  const backend::RunResult runResult = backend::runProgram(runOptions);

  if (runResult.spawnFailed) {
    err << kProgName << ": error: cannot run `" << executable << "`: "
        << (runResult.error.empty() ? "the program could not be executed" : runResult.error)
        << '\n';
    return exitCode(ExitCode::Failure);
  }
  if (runResult.crashed) {
    // The child died from a signal or, on Windows, an unhandled exception. Naming
    // which is platform work (`waitpid` on one side, `GetExitCodeProcess` on the
    // other) and this module contains none, so the honest report is that it did
    // not exit normally -- which is the distinction `run` exists to make
    // (`codegen.md`). The platform's own words are appended when it gave any,
    // which is the case when it ran and the ending itself could not be learned.
    err << kProgName << ": error: the program was terminated abnormally"
        << (runResult.error.empty() ? std::string{} : ": " + runResult.error) << '\n';
    return exitCode(ExitCode::Failure);
  }
  // The program's status, verbatim: `run` is a launcher, not an interpreter, and
  // a harness that compares exit codes must see the program's.
  return runResult.exitCode;
}

} // namespace

int buildInputs(const BuildRequest& request, std::ostream& err) {
  return compile(request, /*execute=*/false, err);
}

int runInputs(const BuildRequest& request, std::ostream& err) {
  return compile(request, /*execute=*/true, err);
}

namespace {

// The request both commands share, built from a parsed command line. One function
// so `run` cannot accept an option `build` refuses or default one differently --
// `run` *is* `build`, and the only thing that separates them is the boolean.
[[nodiscard]] std::optional<BuildRequest> requestFrom(const CliOptions& options,
                                                      std::ostream& err) {
  const std::optional<sema::TargetInfo> target = sema::targetFromName(options.target);
  if (!target.has_value()) {
    (void)usageError(err, "unknown target '" + options.target +
                              "': " + sema::targetRefusal(options.target) + "; the default is '" +
                              std::string(sema::kDefaultTriple) + "'");
    return std::nullopt;
  }
  const std::optional<OutputKind> kind = outputKindFromName(options.emit);
  if (!kind.has_value()) {
    (void)usageError(err, "unknown --emit kind '" + options.emit +
                              "'; this compiler states 'exe', 'obj' and 'asm'");
    return std::nullopt;
  }
  const std::optional<backend::OptLevel> level = backend::optLevelFromName(options.optLevel);
  if (!level.has_value()) {
    (void)usageError(err, "unknown optimisation level '-O" + options.optLevel +
                              "'; this compiler states O0, O1, O2, O3, Os and Oz");
    return std::nullopt;
  }
  if (options.inputs.empty()) {
    (void)usageError(err, "no input files");
    return std::nullopt;
  }

  BuildRequest request;
  request.inputs = options.inputs;
  request.defines = splitDefines(options.defines);
  request.undefines = options.undefines;
  request.includeDirs = options.includeDirs;
  request.systemDirs = options.systemDirs;
  request.target = *target;
  request.warnConversion = options.warnConversion;
  request.warnCast = options.warnCast;
  request.warnProvenance = options.warnProvenance;
  request.warnUnused = options.warnUnused;
  request.warnShadow = options.warnShadow;
  request.diagnosticColor =
      support::colorModeFrom(support::stderrSupportsColor(), options.colorChoice);
  request.errorLimit = options.errorLimit;
  request.kind = *kind;
  request.level = *level;
  // The checked build: `-fcheck`/`-fno-check` when one was written, and the level
  // when neither was. It is **one rule and not two** -- `-O0` is the build a
  // program is developed with, and every optimised level is a build that pays
  // nothing (`checks.md`) -- and `mincc ir` resolves it the same way.
  request.checks = options.checkBuild.value_or(*level == backend::OptLevel::O0);
  request.output = options.output;
  request.debugInfo = options.debugInfo;
  request.verbose = options.verbose;
  request.libraryDirs = options.libraryDirs;
  request.libraries = options.libraries;
  request.linker = options.linker;
  request.sysroot = options.sysroot;
  request.run = options.command == Command::Run;
  request.programArguments = options.programArgs;
  return request;
}

} // namespace

int runBuild(const CliOptions& options) {
  const std::optional<BuildRequest> request = requestFrom(options, std::cerr);
  if (!request.has_value()) {
    return exitCode(ExitCode::Usage);
  }
  return buildInputs(*request, std::cerr);
}

int runRun(const CliOptions& options) {
  const std::optional<BuildRequest> request = requestFrom(options, std::cerr);
  if (!request.has_value()) {
    return exitCode(ExitCode::Usage);
  }
  return runInputs(*request, std::cerr);
}

} // namespace minc::driver
