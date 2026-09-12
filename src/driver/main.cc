// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include <iostream>

#include "driver/cli.h"
#include "driver/exit_code.h"
#include "driver/help_text.h"
#include "driver/version.h"

namespace {

// Every usage error ends with the same actionable hint.
int usageError(const std::string& message) {
  std::cerr << minc::driver::kProgName << ": error: " << message << '\n'
            << "Try '" << minc::driver::kProgName << " --help' for more information.\n";
  return minc::driver::exitCode(minc::driver::ExitCode::Usage);
}

} // namespace

int main(int argc, char** argv) {
  const minc::driver::CliOptions opts = minc::driver::parseArgs(argc, argv);

  if (!opts.error.empty()) {
    return usageError(opts.error);
  }
  if (opts.showHelp) {
    return minc::driver::runHelp();
  }
  if (opts.showVersion) {
    return minc::driver::runVersion();
  }
  if (!opts.command.has_value()) {
    return usageError("missing command");
  }

  std::cerr << minc::driver::kProgName << ": error: command '"
            << minc::driver::toString(*opts.command) << "' is not implemented yet\n";
  return minc::driver::exitCode(minc::driver::ExitCode::Failure);
}
