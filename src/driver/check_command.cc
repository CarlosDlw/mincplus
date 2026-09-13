// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "driver/check_command.h"

#include <cstddef>
#include <iostream>
#include <ostream>
#include <string>
#include <vector>

#include "ast/lower.h"
#include "ast/validate.h"
#include "driver/error_report.h"
#include "driver/exit_code.h"
#include "driver/input_source.h"
#include "lex/token_stream.h"
#include "parse/parse_report.h"
#include "pp/pp_report.h"
#include "pp/preprocessor.h"
#include "resolve/resolve.h"
#include "resolve/resolve_report.h"
#include "resolve/store.h"
#include "sema/dump.h"
#include "sema/sema.h"
#include "sema/sema_report.h"
#include "sema/target.h"
#include "support/diag/diag_renderer.h"
#include "support/expected/fallible.h"
#include "support/session/session.h"
#include "support/source/source_file.h"
#include "support/term/terminal.h"
#include "syntax/store.h"
#include "syntax/tree.h"

namespace minc::driver {
namespace {

// One input's result, kept until the whole invocation has run so the type table
// can be printed once, after every unit has added to it.
struct Unit {
  std::string path;
  const ast::LoweredFile* file = nullptr;
  const resolve::ResolveOutput* resolved = nullptr;
  const sema::SemaOutput* typed = nullptr;
  std::size_t errors = 0;
  std::size_t warnings = 0;
};

} // namespace

int checkInputs(const CheckRequest& request, std::ostream& out, std::ostream& err) {
  // One session, one tree store, one resolve store and one sema context for the
  // invocation. The last one is the point: it owns the compilation's type store,
  // so a type decided while checking the first input is *the same* `TypeId` in
  // the second, which is what a later stage comparing signatures across units
  // depends on.
  support::Session session;
  syntax::TreeStore trees(session.arena());
  resolve::ResolveStore resolves;
  sema::Context sema(sema::targetInfo(request.target));

  const support::DiagRenderer renderer(&session.sources(),
                                       support::RenderOptions{request.diagnosticColor, 4});

  // The preprocessed units, alive for the whole invocation: a token stream is a
  // view into the preprocessed text, and the lowered tree's spans index it too.
  // `reserve` is load-bearing -- a reallocation would dangle every one of those
  // views, which is the kind of bug that only shows up on a large input.
  std::vector<pp::PPResult> preprocessed;
  preprocessed.reserve(request.inputs.size());
  std::vector<ast::LoweredFile> lowered;
  lowered.reserve(request.inputs.size());
  std::vector<Unit> units;
  units.reserve(request.inputs.size());

  bool failed = false;
  for (const std::string& name : request.inputs) {
    const support::Fallible<support::FileId> id = loadInput(session, name);
    if (!id.hasValue()) {
      printError(err, id.error());
      failed = true;
      continue;
    }
    const support::SourceFile* file = session.sources().find(id.value());
    if (file == nullptr) {
      printError(err, "internal error: an input file vanished after loading");
      failed = true;
      continue;
    }

    pp::PPOptions ppOptions;
    ppOptions.defines = request.defines;
    ppOptions.undefines = request.undefines;
    ppOptions.includes.quote = request.includeDirs;
    ppOptions.includes.system = request.systemDirs;
    pp::Preprocessor preprocessor(session, std::move(ppOptions));

    session.diags().clear();
    preprocessed.push_back(preprocessor.run(file->id));
    const pp::PPResult& result = preprocessed.back();
    // The stage reports are called for their side effect -- putting diagnostics
    // in the bag -- and their return values are deliberately ignored: they count
    // *diagnostics added*, which includes the notes attached to an error. What
    // the summary has to say is how many errors there were, and the bag already
    // counts exactly that, without the retention cap changing the number.
    (void)pp::reportLexedFileErrors(result, session.diags());
    (void)pp::reportPPErrors(result.errors, preprocessor.expansions(), session.diags(),
                             &session.symbols());
    (void)pp::reportPPWarnings(result.warnings, preprocessor.expansions(), session.diags(),
                               &session.symbols(), result.systemRegions);

    const lex::TokenStream stream = pp::preprocessedStream(result);
    const syntax::SyntaxTree* tree = trees.parse(stream, file->revision);
    if (tree == nullptr) {
      printError(err, name + ": out of memory while building the syntax tree");
      failed = true;
      continue;
    }
    (void)parse::reportParseErrors(tree->errors(), session.diags());

    // Lower, validate, resolve. Each stage takes the previous one's artifact and
    // returns one of its own; the errors stay values until this function turns
    // them into diagnostics, and nothing above this line knows about `DiagBag`.
    const ast::OriginTable origins{&stream};
    ast::LowerOutput lowerOutput = ast::lowerFile(*tree, session.symbols(), origins);
    const std::vector<ast::AstError> structural = ast::validate(lowerOutput.file);
    (void)resolve::reportAstErrors(lowerOutput.errors, session.diags());
    (void)resolve::reportAstErrors(structural, session.diags());

    resolve::ResolveOptions resolveOptions;
    resolveOptions.warnUnused = request.warnUnused;
    resolveOptions.warnShadow = request.warnShadow;
    const resolve::ResolveOutput* resolved = resolves.outputFor(
        file->id, file->revision, lowerOutput.file, session.symbols(), resolveOptions);
    (void)resolve::reportResolveErrors(resolved->errors, session.diags());
    (void)resolve::reportResolveWarnings(resolved->warnings, session.diags());

    // The typing, through the compilation's central checker. A later stage (or
    // the language server) can call this again for the same revision and get the
    // same answer rather than a second check.
    sema::SemaOptions semaOptions;
    semaOptions.warnConversion = request.warnConversion;
    lowered.push_back(std::move(lowerOutput.file));
    const sema::SemaOutput* typed = sema.check(file->id, file->revision, lowered.back(),
                                               resolved->map, session.symbols(), semaOptions);
    (void)sema::reportSemaErrors(typed->errors, session.diags());
    (void)sema::reportSemaWarnings(typed->warnings, session.diags());

    // The bag was cleared at the top of this iteration, so its counters are this
    // unit's totals -- and unlike the return values above, they count errors and
    // warnings rather than diagnostics-with-notes.
    Unit unit;
    unit.path = file->path;
    unit.file = &lowered.back();
    unit.resolved = resolved;
    unit.typed = typed;
    unit.errors = session.diags().errorCount();
    unit.warnings = session.diags().warningCount();
    units.push_back(unit);

    if (session.diags().hasErrors()) {
      failed = true;
    }

    if (!session.diags().empty()) {
      err << renderer.renderAll(session.diags());
      err.flush();
    }
  }

  // Nothing on stdout unless something was asked for. That is what makes
  // `mincc check` usable the way a compiler is used: a build script reads the
  // exit code, a human reads stderr, and a clean file produces no output at all
  // to scroll past. Each flag below prints exactly one thing.
  if (request.showTypes) {
    // Once for the invocation, not once per file: that is what makes two units'
    // types visibly one `TypeId`.
    out << sema::dumpTypeStore(sema.types());
  }
  if (request.showAst) {
    for (const Unit& unit : units) {
      out << "# " << unit.path << ": typed AST\n";
      out << sema::dumpTypedFile(*unit.file, unit.typed->typed, sema.types());
    }
  } else if (request.stats) {
    // One line per unit, and nothing else: how much the front end built, and
    // whether it was satisfied. The counts come from the stages' own artifacts,
    // not from a running tally.
    for (const Unit& unit : units) {
      out << "# " << unit.path;
      if (unit.resolved != nullptr) {
        out << "  (scopes " << unit.resolved->map.scopes.size() << ", defs "
            << unit.resolved->map.defs.size() << ", refs " << unit.resolved->map.refs.size()
            << ", functions " << unit.typed->typed.functionTable.size() << ")";
      }
      out << "  " << unit.errors << " error(s), " << unit.warnings << " warning(s)\n";
    }
  }
  out.flush();

  return exitCode(failed ? ExitCode::Failure : ExitCode::Ok);
}

int runCheck(const CliOptions& options) {
  if (options.inputs.empty()) {
    return usageError("no input files");
  }
  const std::optional<sema::Target> target = sema::targetFromName(options.target);
  if (!target.has_value()) {
    return usageError("unknown target '" + options.target +
                      "'; known targets are 'systemv-amd64' and 'windows-x64'");
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
  request.warnUnused = options.warnUnused;
  request.warnShadow = options.warnShadow;
  request.diagnosticColor = support::colorModeFrom(support::stderrSupportsColor());
  return checkInputs(request, std::cout, std::cerr);
}

} // namespace minc::driver
