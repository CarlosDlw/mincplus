// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "driver/builtins_command.h"

#include <iostream>
#include <string_view>

#include "builtins/dump.h"
#include "driver/error_report.h"
#include "driver/exit_code.h"

namespace minc::driver {

int runBuiltins(const CliOptions& options) {
  // The list is the compiler's, so an input file is a mistake about what this
  // command is rather than something to ignore: `mincc builtins main.mx` would
  // otherwise print the table and say nothing about the file the reader expected
  // to be read. There is no `--target`-dependent answer here either -- a row says
  // its own availability, and none of them is target-gated today.
  if (!options.inputs.empty()) {
    return usageError("`builtins` reads no files");
  }
  (void)options;
  std::cout << builtins::dumpBuiltins();
  return exitCode(ExitCode::Ok);
}

} // namespace minc::driver
