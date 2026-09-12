// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "driver/help_text.h"

#include <algorithm>
#include <cstddef>
#include <iostream>

#include "driver/cli.h"
#include "driver/exit_code.h"
#include "driver/version.h"

namespace minc::driver {
namespace {

constexpr const char* kOptionsBlock =
    "Options:\n"
    "  -h, --help       Print this help and exit\n"
    "  -V, --version    Print version information and exit\n"
    "  --no-trivia      Omit whitespace and comments from dump output\n"
    "  --               Treat every following argument as a file, not an option\n";

constexpr const char* kExitStatusBlock = "Exit status:\n"
                                         "  0  success\n"
                                         "  1  compilation or runtime failure\n"
                                         "  2  invalid command line\n";

// Comma-separated names of the commands matching `implemented`, taken from the
// command table so this text can never advertise a command that does not work.
[[nodiscard]] std::string commandList(bool implemented) {
  std::string out;
  for (const CommandInfo& info : allCommands()) {
    if (info.implemented != implemented) {
      continue;
    }
    if (!out.empty()) {
      out += ", ";
    }
    out += info.name;
  }
  return out;
}

} // namespace

std::string usageLine() {
  return std::string("Usage: ") + kProgName + " <command> [options] [files...]";
}

std::string versionLine() {
  return std::string(kProgName) + " " + kVersion;
}

std::string helpText() {
  std::string out;
  out += "minc+ - minimal C with extras, full C interoperability\n\n";
  // Host support and the interop target are different axes; state both so
  // nobody assumes the compiler is tied to one platform or ABI.
  out += "Hosts:     Linux, macOS, Windows (Clang, GCC, MSVC)\n";
  out += "C interop: System V AMD64 ABI (.o/.a linked via cc/ld)\n\n";
  out += usageLine();
  out += "\n\nCommands:\n";

  // Align the summary column against the longest invocation.
  std::size_t width = 0;
  for (const CommandInfo& info : allCommands()) {
    width = std::max(width, info.name.size() + 1 + info.args.size());
  }
  for (const CommandInfo& info : allCommands()) {
    std::string invocation(info.name);
    invocation += ' ';
    invocation += info.args;
    out += "  ";
    out += invocation;
    out.append(width - invocation.size() + 2, ' ');
    out += info.summary;
    out += '\n';
  }

  out += '\n';
  out += kOptionsBlock;
  out += '\n';
  out += kExitStatusBlock;
  out += '\n';

  const std::string implemented = commandList(true);
  const std::string scaffolded = commandList(false);
  out += "Implemented: ";
  out += implemented.empty() ? std::string("(none)") : implemented;
  out += "\nScaffolded:  ";
  out += scaffolded.empty() ? std::string("(none)") : scaffolded;
  out += '\n';
  if (!scaffolded.empty()) {
    out += "\nScaffolded commands are accepted so build scripts can be written\n";
    out += "against them; they report that they are not implemented yet.\n";
  }
  return out;
}

int runHelp() {
  std::cout << helpText();
  return exitCode(ExitCode::Ok);
}

int runVersion() {
  std::cout << versionLine() << '\n';
  return exitCode(ExitCode::Ok);
}

} // namespace minc::driver
