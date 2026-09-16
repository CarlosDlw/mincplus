// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "driver/check_command.h"

#include <cstddef>
#include <iostream>
#include <ostream>
#include <string>

#include "driver/error_report.h"
#include "driver/exit_code.h"
#include "driver/frontend.h"
#include "sema/dump.h"
#include "sema/target.h"

namespace minc::driver {

int checkInputs(const CheckRequest& request, std::ostream& out, std::ostream& err) {
  // The pipeline is `frontend.cc`'s; this command owns only what to *print*.
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
  const bool ok = frontEnd.run(request.inputs, err);

  // Nothing on stdout unless something was asked for. That is what makes
  // `mincc check` usable the way a compiler is used: a build script reads the
  // exit code, a human reads stderr, and a clean file produces no output at all
  // to scroll past. Each flag below prints exactly one thing.
  if (request.showTypes) {
    // Once for the invocation, not once per file: that is what makes two units'
    // types visibly one `TypeId`.
    out << sema::dumpTypeStore(frontEnd.sema().types());
  }
  if (request.showAst) {
    for (const FrontEndUnit& unit : frontEnd.units()) {
      if (unit.lowered == nullptr || unit.typed == nullptr) {
        continue;
      }
      out << "# " << unit.path << ": typed AST\n";
      out << sema::dumpTypedFile(*unit.lowered, unit.typed->typed, frontEnd.sema().types());
    }
  } else if (request.stats) {
    // One line per unit, and nothing else: how much the front end built, and
    // whether it was satisfied. The counts come from the stages' own artifacts,
    // not from a running tally.
    for (const FrontEndUnit& unit : frontEnd.units()) {
      out << "# " << unit.path;
      if (unit.resolved != nullptr && unit.typed != nullptr) {
        out << "  (scopes " << unit.resolved->map.scopes.size() << ", defs "
            << unit.resolved->map.defs.size() << ", refs " << unit.resolved->map.refs.size()
            << ", functions " << unit.typed->typed.functionTable.size() << ")";
      }
      out << "  " << unit.errors << " error(s), " << unit.warnings << " warning(s)\n";
    }
  }
  out.flush();

  return exitCode(ok ? ExitCode::Ok : ExitCode::Failure);
}

int runCheck(const CliOptions& options) {
  if (options.inputs.empty()) {
    return usageError("no input files");
  }
  const std::optional<sema::TargetInfo> target = sema::targetFromName(options.target);
  if (!target.has_value()) {
    // The *reason* comes from the parser that refused it, so the sentence and the
    // refusal cannot be about different things; the list of stated rows is the
    // one place a target can be read from.
    return usageError("unknown target '" + options.target +
                      "': " + sema::targetRefusal(options.target) + "; the default is '" +
                      std::string(sema::kDefaultTriple) + "'");
  }

  CheckRequest request;
  request.inputs = options.inputs;
  request.defines = splitDefines(options.defines);
  request.undefines = options.undefines;
  request.includeDirs = options.includeDirs;
  request.systemDirs = options.systemDirs;
  request.target = *target;
  request.showAst = options.showAst;
  request.showTypes = options.showTypes;
  request.stats = options.stats;
  request.warnConversion = options.warnConversion;
  request.warnCast = options.warnCast;
  request.warnProvenance = options.warnProvenance;
  request.warnUnused = options.warnUnused;
  request.warnShadow = options.warnShadow;
  request.diagnosticColor =
      support::colorModeFrom(support::stderrSupportsColor(), options.colorChoice);
  request.errorLimit = options.errorLimit;
  return checkInputs(request, std::cout, std::cerr);
}

} // namespace minc::driver
