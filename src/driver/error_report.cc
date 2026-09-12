// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "driver/error_report.h"

#include <iostream>

#include "driver/exit_code.h"
#include "driver/version.h"

namespace minc::driver {

void printError(std::string_view message) {
  std::cerr << kProgName << ": error: " << message << '\n';
}

void printUsageHint() {
  std::cerr << "Try '" << kProgName << " --help' for more information.\n";
}

int usageError(std::string_view message) {
  printError(message);
  printUsageHint();
  return exitCode(ExitCode::Usage);
}

} // namespace minc::driver
