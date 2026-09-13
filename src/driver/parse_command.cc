// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "driver/parse_command.h"

#include <cstddef>
#include <iostream>
#include <ostream>
#include <string>

#include "driver/error_report.h"
#include "driver/exit_code.h"
#include "driver/input_source.h"
#include "lex/lex_report.h"
#include "lex/token_stream.h"
#include "parse/parse_report.h"
#include "pp/pp_report.h"
#include "pp/preprocessor.h"
#include "support/diag/diag_renderer.h"
#include "support/expected/fallible.h"
#include "support/session/session.h"
#include "support/source/source_file.h"
#include "support/term/terminal.h"
#include "syntax/dump.h"
#include "syntax/store.h"

namespace minc::driver {

int parseInputs(const ParseRequest& request, std::ostream& out, std::ostream& err) {
  // One session and one tree store for the whole invocation: together they own
  // the sources, the diagnostics, the arena the trees are built into, and the
  // node cache, so nothing here reaches for a global. Because the store is per
  // invocation, identical subtrees in two different inputs are the same green
  // node.
  support::Session session;
  syntax::TreeStore store(session.arena());

  const support::DiagRenderer renderer(&session.sources(),
                                       support::RenderOptions{request.diagnosticColor, 4});
  syntax::DumpOptions dumpOptions;
  dumpOptions.color = request.dumpColor;
  dumpOptions.showTrivia = request.showTrivia;

  // The preprocessed unit of every input, alive for the whole invocation.
  //
  // A green leaf's text is a view, and the node cache is shared across inputs --
  // that sharing is the reason the store exists. So the text a leaf points at
  // has to outlive the cache, and it cannot be freed at the end of the loop
  // iteration that produced it. `reserve` is load-bearing: it is what keeps the
  // elements from moving out from under the views.
  std::vector<pp::PPResult> preprocessed;
  preprocessed.reserve(request.inputs.size());

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

    // The front end is `source -> lex -> preprocess -> parse`, in that order and
    // with no way to skip a stage: the parser is handed the preprocessed stream,
    // because a parser that reads a file directly cannot see a macro and would
    // report every directive as a syntax error. The tree is built from the same
    // stream, so the two halves cannot disagree about what the tokens are.
    pp::PPOptions ppOptions;
    ppOptions.defines = request.defines;
    ppOptions.undefines = request.undefines;
    ppOptions.includes.quote = request.includeDirs;
    ppOptions.includes.system = request.systemDirs;
    pp::Preprocessor preprocessor(session, std::move(ppOptions));

    // Cleared per file so the counts below belong to this input alone and the
    // bag never accumulates across a multi-file command line.
    session.diags().clear();
    preprocessed.push_back(preprocessor.run(file->id));
    const pp::PPResult& result = preprocessed.back();
    const std::size_t lexical = pp::reportLexedFileErrors(result, session.diags());
    (void)pp::reportPPErrors(result.errors, preprocessor.expansions(), session.diags(),
                             &session.symbols());
    (void)pp::reportPPWarnings(result.warnings, preprocessor.expansions(), session.diags(),
                               &session.symbols(), result.systemRegions);

    const lex::TokenStream stream = pp::preprocessedStream(result);
    const syntax::SyntaxTree* tree = store.parse(stream, file->revision);
    if (tree == nullptr) {
      printError(err, name + ": out of memory while building the syntax tree");
      failed = true;
      continue;
    }
    const std::size_t syntax = parse::reportParseErrors(tree->errors(), session.diags());

    out << syntax::dumpTree(*tree, file->path, dumpOptions);
    // Flush before the diagnostics: when the two streams are the same terminal,
    // this keeps each file's tree next to its errors.
    out.flush();

    if (!session.diags().empty()) {
      err << renderer.renderAll(session.diags());
      err.flush();
    }
    if (lexical != 0 || syntax != 0 || !result.errors.empty()) {
      failed = true;
    }
  }

  return exitCode(failed ? ExitCode::Failure : ExitCode::Ok);
}

int runParse(const CliOptions& options) {
  if (options.inputs.empty()) {
    return usageError("no input files");
  }

  ParseRequest request;
  request.inputs = options.inputs;
  request.defines = splitDefines(options.defines);
  request.undefines = options.undefines;
  request.includeDirs = options.includeDirs;
  request.systemDirs = options.systemDirs;
  request.showTrivia = !options.hideTrivia;
  // Color is decided per stream: a redirected stdout must stay clean even when
  // the terminal the user is watching can render colors on stderr.
  request.dumpColor = support::colorModeFrom(support::stdoutSupportsColor());
  request.diagnosticColor = support::colorModeFrom(support::stderrSupportsColor());
  return parseInputs(request, std::cout, std::cerr);
}

} // namespace minc::driver
