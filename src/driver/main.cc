// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
//
// `main` is the only place that decides which subcommand runs. Everything it
// needs comes from the parsed options: the parser does not print, the commands
// do not exit, and the exit code is chosen here and nowhere else.
#include <iostream>

#include "driver/check_command.h"
#include "driver/cli.h"
#include "driver/error_report.h"
#include "driver/exit_code.h"
#include "driver/help_text.h"
#include "driver/lex_command.h"
#include "driver/parse_command.h"
#include "driver/pp_command.h"
#include "driver/resolve_command.h"
#include "driver/version.h"

int main(int argc, char** argv) {
  const minc::driver::CliOptions opts = minc::driver::parseArgs(argc, argv);

  if (!opts.error.empty()) {
    return minc::driver::usageError(opts.error);
  }
  if (opts.showHelp) {
    return minc::driver::runHelp();
  }
  if (opts.showVersion) {
    return minc::driver::runVersion();
  }
  if (!opts.command.has_value()) {
    return minc::driver::usageError("missing command");
  }

  switch (*opts.command) {
  case minc::driver::Command::Lex:
    return minc::driver::runLex(opts);
  case minc::driver::Command::Parse:
    return minc::driver::runParse(opts);
  case minc::driver::Command::Pp:
    return minc::driver::runPp(opts);
  case minc::driver::Command::Resolve:
    return minc::driver::runResolve(opts);
  case minc::driver::Command::Check:
    return minc::driver::runCheck(opts);
  case minc::driver::Command::Build:
  case minc::driver::Command::Run:
    break;
  }

  std::cerr << minc::driver::kProgName << ": error: command '"
            << minc::driver::toString(*opts.command) << "' is not implemented yet\n";
  return minc::driver::exitCode(minc::driver::ExitCode::Failure);
}
