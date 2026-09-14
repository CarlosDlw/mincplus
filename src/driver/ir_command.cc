// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "driver/ir_command.h"

#include <cstddef>
#include <iostream>
#include <ostream>
#include <string>
#include <vector>

#include "driver/error_report.h"
#include "driver/exit_code.h"
#include "driver/frontend.h"
#include "ir/dump.h"
#include "ir/invariants.h"
#include "ir/ir.h"
#include "ir/ir_report.h"
#include "sema/target.h"
#include "support/diag/diag_bag.h"
#include "support/diag/diag_renderer.h"
#include "support/session/session.h"
#include "support/term/terminal.h"

namespace minc::driver {
namespace {

// Renders IR diagnostics through the same machinery every other stage's
// diagnostics go through, so a caret and a code read the same here as anywhere.
void render(std::span<const ir::IRDiagnostic> diagnostics, FrontEnd& frontEnd,
            support::ColorMode color, std::ostream& err) {
  if (diagnostics.empty()) {
    return;
  }
  support::DiagBag bag;
  (void)ir::reportIRDiagnostics(diagnostics, bag);
  const support::DiagRenderer renderer(&frontEnd.session().sources(),
                                       support::RenderOptions{color, 4});
  err << renderer.renderAll(bag);
  err.flush();
}

} // namespace

int irInputs(const IrRequest& request, std::ostream& out, std::ostream& err) {
  FrontEndOptions options;
  options.defines = request.defines;
  options.undefines = request.undefines;
  options.includeDirs = request.includeDirs;
  options.systemDirs = request.systemDirs;
  options.target = request.target;
  options.warnConversion = request.warnConversion;
  options.warnUnused = request.warnUnused;
  options.warnShadow = request.warnShadow;
  options.diagnosticColor = request.diagnosticColor;

  FrontEnd frontEnd(options);
  bool ok = frontEnd.run(request.inputs, err);

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

    const ir::IRResult result = ir::lowerUnit(*unit.lowered, unit.resolved->map, unit.typed->typed,
                                              frontEnd.sema().types(), frontEnd.symbols());
    if (result.failed()) {
      render(result.diagnostics, frontEnd, request.diagnosticColor, err);
      ok = false;
      continue;
    }

    // The module is printed even when the scan finds something: the reader needs
    // the IR in front of them to understand the violation, and the scan is about
    // *this compiler*, not about the program they wrote.
    out << ir::dumpModule(result.module);
    out.flush();

    const std::vector<ir::IRDiagnostic> violations = ir::scanModule(result.module);
    if (!violations.empty()) {
      render(violations, frontEnd, request.diagnosticColor, err);
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
  request.inputs = options.inputs;
  request.defines = splitDefines(options.defines);
  request.undefines = options.undefines;
  request.includeDirs = options.includeDirs;
  request.systemDirs = options.systemDirs;
  request.target = *target;
  request.warnConversion = options.warnConversion;
  request.warnUnused = options.warnUnused;
  request.warnShadow = options.warnShadow;
  request.diagnosticColor = support::colorModeFrom(support::stderrSupportsColor());
  return irInputs(request, std::cout, std::cerr);
}

} // namespace minc::driver
