// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
//
// `main` is the only place that decides which subcommand runs. Everything it
// needs comes from the parsed options: the parser does not print, the commands
// do not exit, and the exit code is chosen here and nowhere else.
//
// The order of the first four steps is the contract `docs/architectures/cli.md`
// states, and it is the order because each one outranks the ones below it:
//
//   1. a usage error, unless help or version was asked for -- in which case the
//      parser already decided, and `opts.error` is empty;
//   2. `--help`/`-h`/`help`, which print a page and exit 0;
//   3. `--version`/`-V`, the same;
//   4. an empty command line, which prints the overview on **stderr** and exits
//      2: printing it is what happened, but nothing was done.
#include <iostream>

#include "driver/build_command.h"
#include "driver/check_command.h"
#include "driver/cli.h"
#include "driver/error_report.h"
#include "driver/exit_code.h"
#include "driver/help_text.h"
#include "driver/ir_command.h"
#include "driver/lex_command.h"
#include "driver/parse_command.h"
#include "driver/pp_command.h"
#include "driver/resolve_command.h"
#include "driver/version.h"

int main(int argc, char** argv) {
  const minc::driver::CliOptions opts = minc::driver::parseArgs(argc, argv);

  if (!opts.error.empty()) {
    return minc::driver::usageError(opts.error, opts.suggestion, opts.command);
  }
  if (opts.showHelp) {
    // `mincc help build` and `mincc build --help` are the same page, and a
    // command named anywhere on a help line is the topic of it.
    if (opts.helpTopic.has_value()) {
      return minc::driver::runHelp(opts.colorChoice, opts.helpTopic);
    }
    return minc::driver::runHelp(opts.colorChoice, opts.command);
  }
  if (opts.showVersion) {
    return minc::driver::runVersion(opts.colorChoice, opts.verbose);
  }
  if (opts.emptyCommandLine) {
    return minc::driver::runBareInvocation(opts.colorChoice);
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
  case minc::driver::Command::Ir:
    return minc::driver::runIr(opts);
  case minc::driver::Command::Build:
    return minc::driver::runBuild(opts);
  case minc::driver::Command::Run:
    return minc::driver::runRun(opts);
  }

  // Unreachable: the switch above is total over `Command`, and `-Wswitch` keeps it
  // that way as the enum grows. The fall-through exists so a value that somehow
  // reached here is a message rather than a crash.
  std::cerr << minc::driver::kProgName << ": error: command '"
            << minc::driver::toString(*opts.command) << "' has no dispatch\n";
  return minc::driver::exitCode(minc::driver::ExitCode::Failure);
}
