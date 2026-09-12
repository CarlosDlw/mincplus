// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "driver/error_report.h"

#include <iostream>
#include <string_view>

#include "driver/exit_code.h"
#include "driver/version.h"

namespace minc::driver {

void printError(std::ostream& err, std::string_view message) {
  err << kProgName << ": error: " << message << '\n';
}

void printUsageHint(std::ostream& err) {
  err << "Try '" << kProgName << " --help' for more information.\n";
}

int usageError(std::ostream& err, std::string_view message) {
  printError(err, message);
  printUsageHint(err);
  return exitCode(ExitCode::Usage);
}

void printError(std::string_view message) {
  printError(std::cerr, message);
}

void printUsageHint() {
  printUsageHint(std::cerr);
}

int usageError(std::string_view message) {
  return usageError(std::cerr, message);
}

} // namespace minc::driver
