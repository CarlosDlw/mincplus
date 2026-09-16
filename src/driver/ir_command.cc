// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "driver/ir_command.h"

#include <cstddef>
#include <iostream>
#include <ostream>
#include <string>
#include <vector>

#include "driver/diagnostic_options.h"
#include "driver/error_report.h"
#include "driver/exit_code.h"
#include "driver/frontend.h"
#include "driver/stage_report.h"
#include "ir/dump.h"
#include "ir/invariants.h"
#include "ir/ir.h"
#include "sema/target.h"
#include "support/session/session.h"
#include "support/term/terminal.h"

namespace minc::driver {

int irInputs(const IrRequest& request, std::ostream& out, std::ostream& err) {
  FrontEndOptions options;
  options.defines = request.defines;
  options.undefines = request.undefines;
  options.includeDirs = request.includeDirs;
  options.systemDirs = request.systemDirs;
  options.target = request.target;
  options.warnConversion = request.warnConversion;
  options.warnCast = request.warnCast;
  options.warnProvenance = request.warnProvenance;
  options.warnUnused = request.warnUnused;
  options.warnShadow = request.warnShadow;
  options.diagnosticColor = request.diagnosticColor;
  options.errorLimit = request.errorLimit;

  FrontEnd frontEnd(options);
  bool ok = frontEnd.run(request.inputs, err);
  const support::SourceManager& sources = frontEnd.session().sources();
  const support::RenderOptions diag =
      diagnosticOptions(request.diagnosticColor, request.errorLimit);

  for (const FrontEndUnit& unit : frontEnd.units()) {
    // A unit whose front end failed has no typed tree, or has one with errors in
    // it. Either way there is nothing whose meaning was decided, so it produces
    // no module -- which is the contract, not an optimisation.
    if (unit.lowered == nullptr || unit.resolved == nullptr || unit.typed == nullptr) {
      continue;
    }
    if (unit.errors != 0) {
      continue;
    }

    // `-g` is an *option* here rather than a flag that changes the walk: the
    // lowering does the same work either way and only attaches locations when one
    // was asked to be kept. `mincc ir -g` is how the metadata is reviewed -- as
    // text, next to the instructions it annotates (`codegen.md`).
    ir::LoweringOptions loweringOptions;
    loweringOptions.debugInfo = request.debugInfo;
    loweringOptions.producer = producerString();
    loweringOptions.checks = request.checks;
    loweringOptions.source = sources.find(unit.file);
    loweringOptions.sources = &sources;

    const ir::IRResult result =
        ir::lowerUnit(*unit.lowered, unit.resolved->map, unit.typed->typed, frontEnd.sema().types(),
                      frontEnd.symbols(), loweringOptions);
    if (result.failed()) {
      renderStageDiagnostics(result.diagnostics, sources, diag, err);
      ok = false;
      continue;
    }

    // The module is printed even when the scan finds something: the reader needs
    // the IR in front of them to understand the violation, and the scan is about
    // *this compiler*, not about the program they wrote.
    out << ir::dumpModule(result.module);
    out.flush();

    const std::vector<ir::IRDiagnostic> violations =
        ir::scanModule(result.module, ir::ScanOptions{request.checks});
    if (!violations.empty()) {
      renderStageDiagnostics(violations, sources, diag, err);
      ok = false;
    }
  }

  return exitCode(ok ? ExitCode::Ok : ExitCode::Failure);
}

int runIr(const CliOptions& options) {
  if (options.inputs.empty()) {
    return usageError("no input files");
  }
  const std::optional<sema::TargetInfo> target = sema::targetFromName(options.target);
  if (!target.has_value()) {
    return usageError("unknown target '" + options.target +
                      "': " + sema::targetRefusal(options.target) + "; the default is '" +
                      std::string(sema::kDefaultTriple) + "'");
  }

  IrRequest request;
  // The checked build at `-O0`'s answer, unless the command line said otherwise
  // (`ir_command.h`).
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
  request.debugInfo = options.debugInfo;
  // `-fcheck`/`-fno-check` when one was written, and the `-O0` answer otherwise:
  // this command has no `-O`, and it prints the module a `-O0` build would have
  // emitted.
  request.checks = options.checkBuild.value_or(true);
  return irInputs(request, std::cout, std::cerr);
}

} // namespace minc::driver
