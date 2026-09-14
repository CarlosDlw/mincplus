// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "driver/lex_command.h"

#include <cstddef>
#include <iostream>
#include <ostream>
#include <string>

#include "driver/diagnostic_options.h"
#include "driver/error_report.h"
#include "driver/exit_code.h"
#include "driver/input_source.h"
#include "lex/lex_report.h"
#include "lex/token_dump.h"
#include "lex/token_stream.h"
#include "support/diag/diag_renderer.h"
#include "support/expected/fallible.h"
#include "support/session/session.h"
#include "support/source/source_file.h"
#include "support/term/terminal.h"

namespace minc::driver {

int lexInputs(const LexRequest& request, std::ostream& out, std::ostream& err) {
  // One session for the whole invocation, which is what the sources, the
  // diagnostics, and the interner are meant to be owned by. Nothing here
  // reaches for a global.
  support::Session session;

  const support::DiagRenderer renderer(
      &session.sources(), diagnosticOptions(request.diagnosticColor, request.errorLimit));
  // The error budget spans the invocation: the bag is cleared per input.
  std::size_t errorsShown = 0;
  const lex::DumpOptions dumpOptions{request.dumpColor};

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

    // Cleared per file so the count below belongs to this input alone and the
    // bag never accumulates across a multi-file command line.
    session.diags().clear();
    const std::size_t problems = lex::reportLexErrors(stream, session.diags());

    out << lex::dumpTokens(stream, file->path, dumpOptions);
    // Flush before the diagnostics: when the two streams are the same terminal,
    // this keeps each file's table next to its errors.
    out.flush();

    if (!session.diags().empty()) {
      err << renderer.renderAll(session.diags(), errorsShown);
      err.flush();
    }
    if (problems != 0) {
      failed = true;
    }
  }

  return exitCode(failed ? ExitCode::Failure : ExitCode::Ok);
}

int runLex(const CliOptions& options) {
  if (options.inputs.empty()) {
    return usageError("no input files");
  }

  LexRequest request;
  request.inputs = options.inputs;
  // Color is decided per stream: a redirected stdout must stay clean even when
  // the terminal the user is watching can render colors on stderr.
  request.dumpColor = support::colorModeFrom(support::stdoutSupportsColor(), options.colorChoice);
  request.diagnosticColor =
      support::colorModeFrom(support::stderrSupportsColor(), options.colorChoice);
  request.errorLimit = options.errorLimit;
  return lexInputs(request, std::cout, std::cerr);
}

} // namespace minc::driver
