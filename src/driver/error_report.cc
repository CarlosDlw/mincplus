// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "driver/error_report.h"

#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "driver/exit_code.h"
#include "driver/version.h"

namespace minc::driver {

void printError(std::ostream& err, std::string_view message) {
  err << kProgName << ": error: " << message << '\n';
}

void printSuggestion(std::ostream& err, std::string_view suggestion) {
  if (suggestion.empty()) {
    return;
  }
  err << "note: did you mean '" << suggestion << "'?\n";
}

void printUsageHint(std::ostream& err, std::optional<Command> command) {
  err << "Try '" << kProgName;
  if (command.has_value()) {
    err << ' ' << toString(*command);
  }
  err << " --help' for more information.\n";
}

int usageError(std::ostream& err, std::string_view message, std::string_view suggestion,
               std::optional<Command> command) {
  printError(err, message);
  printSuggestion(err, suggestion);
  printUsageHint(err, command);
  return exitCode(ExitCode::Usage);
}

void printError(std::string_view message) {
  printError(std::cerr, message);
}

void printSuggestion(std::string_view suggestion) {
  printSuggestion(std::cerr, suggestion);
}

void printUsageHint(std::optional<Command> command) {
  printUsageHint(std::cerr, command);
}

int usageError(std::string_view message, std::string_view suggestion,
               std::optional<Command> command) {
  return usageError(std::cerr, message, suggestion, command);
}

} // namespace minc::driver
