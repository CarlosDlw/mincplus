// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "driver/lex_command.h"

#include <cstddef>
#include <iostream>
#include <string>
#include <utility>

#include "driver/error_report.h"
#include "driver/exit_code.h"
#include "lex/lex_report.h"
#include "lex/token_dump.h"
#include "lex/token_stream.h"
#include "support/diag/diag_renderer.h"
#include "support/expected/fallible.h"
#include "support/session/session.h"
#include "support/source/file_io.h"
#include "support/source/source_file.h"
#include "support/term/terminal.h"

namespace minc::driver {
namespace {

// `-` is the one input name that is not a path: it means standard input, which
// is what makes `mincc lex -` work in a pipe.
[[nodiscard]] support::Fallible<support::FileId> loadInput(support::Session& session,
                                                           const std::string& name) {
  if (name != "-") {
    return session.loadFromDisk(name);
  }
  support::Fallible<std::string> bytes = support::readStdinBytes();
  if (!bytes.hasValue()) {
    return support::makeUnexpected<std::string>(bytes.error());
  }
  return session.addFile("<stdin>", std::move(bytes).value());
}

} // namespace

int runLex(const CliOptions& options) {
  if (options.inputs.empty()) {
    return usageError("no input files");
  }

  // One session for the whole invocation, which is what the sources, the
  // diagnostics, and the interner are meant to be owned by. Nothing here
  // reaches for a global.
  support::Session session;

  // Color is decided per stream: a redirected stdout must stay clean even when
  // the terminal the user is watching can render colors on stderr.
  const support::DiagRenderer renderer(
      &session.sources(),
      support::RenderOptions{support::colorModeFrom(support::stderrSupportsColor()), 4});
  const lex::DumpOptions dumpOptions{support::colorModeFrom(support::stdoutSupportsColor())};

  bool failed = false;
  for (const std::string& name : options.inputs) {
    const support::Fallible<support::FileId> id = loadInput(session, name);
    if (!id.hasValue()) {
      printError(id.error());
      failed = true;
      continue;
    }

    const support::SourceFile* file = session.sources().find(id.value());
    if (file == nullptr) {
      printError("internal error: an input file vanished after loading");
      failed = true;
      continue;
    }

    const lex::TokenStream stream = lex::TokenStream::lex(file->id, file->text);

    // Cleared per file so the count below belongs to this input alone and the
    // bag never accumulates across a multi-file command line.
    session.diags().clear();
    const std::size_t problems = lex::reportLexErrors(stream, session.diags());

    std::cout << lex::dumpTokens(stream, file->path, dumpOptions);
    // Flush before the diagnostics: when stdout and stderr are the same
    // terminal, this keeps each file's table next to its errors.
    std::cout.flush();

    if (!session.diags().empty()) {
      std::cerr << renderer.renderAll(session.diags());
      std::cerr.flush();
    }
    if (problems != 0) {
      failed = true;
    }
  }

  return exitCode(failed ? ExitCode::Failure : ExitCode::Ok);
}

} // namespace minc::driver
