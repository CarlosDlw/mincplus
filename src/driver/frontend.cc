// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The front-end sequence, moved here from `check_command.cc` so the commands
// that run it cannot wire it differently.
#include "driver/frontend.h"

#include <cstddef>
#include <ostream>
#include <string>
#include <vector>

#include "ast/lower.h"
#include "ast/validate.h"
#include "driver/diagnostic_options.h"
#include "driver/error_report.h"
#include "driver/input_source.h"
#include "driver/version.h"
#include "lex/token_stream.h"
#include "parse/parse_report.h"
#include "pp/pp_report.h"
#include "resolve/resolve_report.h"
#include "sema/sema_report.h"
#include "support/diag/diag_renderer.h"
#include "support/expected/fallible.h"
#include "support/session/session.h"
#include "support/source/source_file.h"
#include "syntax/tree.h"

namespace minc::driver {

std::string producerString() {
  return std::string(kProgName) + " " + kVersion;
}

FrontEnd::FrontEnd(const FrontEndOptions& options)
    : options_(options), trees_(session_.arena()), sema_(options.target) {}

bool FrontEnd::run(const std::vector<std::string>& inputs, std::ostream& err) {
  // One renderer and one error count for the whole invocation: the bag is cleared
  // per translation unit, so a limit carried in the bag would be a limit per
  // file, and ten files with one error each would print ten errors under a limit
  // of three.
  const support::DiagRenderer renderer(
      &session_.sources(), diagnosticOptions(options_.diagnosticColor, options_.errorLimit));
  std::size_t errorsShown = 0;

  preprocessed_.reserve(inputs.size());
  lowered_.reserve(inputs.size());
  units_.reserve(inputs.size());

  bool failed = false;
  for (const std::string& name : inputs) {
    const support::Fallible<support::FileId> id = loadInput(session_, name);
    if (!id.hasValue()) {
      printError(err, id.error());
      failed = true;
      continue;
    }
    const support::SourceFile* file = session_.sources().find(id.value());
    if (file == nullptr) {
      printError(err, "internal error: an input file vanished after loading");
      failed = true;
      continue;
    }

    pp::PPOptions ppOptions;
    ppOptions.defines = options_.defines;
    ppOptions.undefines = options_.undefines;
    ppOptions.includes.quote = options_.includeDirs;
    ppOptions.includes.system = options_.systemDirs;
    pp::Preprocessor preprocessor(session_, std::move(ppOptions));

    session_.diags().clear();
    preprocessed_.push_back(preprocessor.run(file->id));
    const pp::PPResult& result = preprocessed_.back();
    // The stage reports are called for their side effect -- putting diagnostics
    // in the bag -- and their return values are deliberately ignored: they count
    // *diagnostics added*, which includes the notes attached to an error. The
    // bag already counts errors without the retention cap changing the number.
    (void)pp::reportLexedFileErrors(result, session_.diags());
    (void)pp::reportPPErrors(result.errors, preprocessor.expansions(), session_.diags(),
                             &session_.symbols());
    (void)pp::reportPPWarnings(result.warnings, preprocessor.expansions(), session_.diags(),
                               &session_.symbols(), result.systemRegions);

    const lex::TokenStream stream = pp::preprocessedStream(result);
    const syntax::SyntaxTree* tree = trees_.parse(stream, file->revision);
    if (tree == nullptr) {
      printError(err, name + ": out of memory while building the syntax tree");
      failed = true;
      continue;
    }
    (void)parse::reportParseErrors(tree->errors(), session_.diags());

    // Lower, validate, resolve. Each stage takes the previous one's artifact and
    // returns one of its own; the errors stay values until this function turns
    // them into diagnostics.
    const ast::OriginTable origins{&stream};
    ast::LowerOutput lowerOutput = ast::lowerFile(*tree, session_.symbols(), origins);
    const std::vector<ast::AstError> structural = ast::validate(lowerOutput.file);
    (void)resolve::reportAstErrors(lowerOutput.errors, session_.diags());
    (void)resolve::reportAstErrors(structural, session_.diags());

    resolve::ResolveOptions resolveOptions;
    resolveOptions.warnUnused = options_.warnUnused;
    resolveOptions.warnShadow = options_.warnShadow;
    const resolve::ResolveOutput* resolved = resolves_.outputFor(
        file->id, file->revision, lowerOutput.file, session_.symbols(), resolveOptions);
    (void)resolve::reportResolveErrors(resolved->errors, session_.diags());
    (void)resolve::reportResolveWarnings(resolved->warnings, session_.diags());

    // The typing, through the compilation's central checker. A later stage (or
    // the language server) can call this again for the same revision and get the
    // same answer rather than a second check.
    sema::SemaOptions semaOptions;
    semaOptions.warnConversion = options_.warnConversion;
    lowered_.push_back(std::move(lowerOutput.file));
    const sema::SemaOutput* typed = sema_.check(file->id, file->revision, lowered_.back(),
                                                resolved->map, session_.symbols(), semaOptions);
    (void)sema::reportSemaErrors(typed->errors, session_.diags());
    (void)sema::reportSemaWarnings(typed->warnings, session_.diags());

    FrontEndUnit unit;
    unit.path = file->path;
    unit.file = file->id;
    unit.revision = file->revision;
    unit.lowered = &lowered_.back();
    unit.resolved = resolved;
    unit.typed = typed;
    // The bag was cleared at the top of this iteration, so its counters are this
    // unit's totals -- and unlike the return values above, they count errors and
    // warnings rather than diagnostics-with-notes.
    unit.errors = session_.diags().errorCount();
    unit.warnings = session_.diags().warningCount();
    units_.push_back(unit);

    if (session_.diags().hasErrors()) {
      failed = true;
    }
    if (!session_.diags().empty()) {
      err << renderer.renderAll(session_.diags(), errorsShown);
      err.flush();
    }
  }

  return !failed;
}

} // namespace minc::driver
