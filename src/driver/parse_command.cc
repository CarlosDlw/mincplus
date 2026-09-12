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

    const lex::TokenStream stream = lex::TokenStream::lex(file->id, file->text);

    // Cleared per file so the counts below belong to this input alone and the
    // bag never accumulates across a multi-file command line.
    session.diags().clear();
    // Lexical problems are reported first: a syntax error caused by a malformed
    // token is a consequence, and showing the cause first reads better.
    const std::size_t lexical = lex::reportLexErrors(stream, session.diags());

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
    if (lexical != 0 || syntax != 0) {
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
  request.showTrivia = !options.hideTrivia;
  // Color is decided per stream: a redirected stdout must stay clean even when
  // the terminal the user is watching can render colors on stderr.
  request.dumpColor = support::colorModeFrom(support::stdoutSupportsColor());
  request.diagnosticColor = support::colorModeFrom(support::stderrSupportsColor());
  return parseInputs(request, std::cout, std::cerr);
}

} // namespace minc::driver
