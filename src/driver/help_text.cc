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
    "  -D name[=body]   Define a macro before the file is read\n"
    "  -U name          Undefine a macro; applied after every -D\n"
    "  -I dir           Add a directory to the include search list\n"
    "  -isystem dir     Like -I, after every -I, but a file found there is\n"
    "                   a system header: warnings in it are suppressed\n"
    "  --no-trivia      Omit whitespace and comments from dump output\n"
    "  --refs           resolve: print every name use and its target\n"
    "  --unresolved     resolve: print only the uses with no target, with reasons\n"
    "  --ast            resolve: the lowered AST and the item tree\n"
    "                   check: the typed tree, with a type on every node\n"
    "  --stats          check: one summary line per file (nothing is printed\n"
    "                   on success without it)\n"
    "  --types          check: print only the table of types\n"
    "  --target TRIPLE  check: the target the C type spellings are read\n"
    "                   against, as an LLVM triple in `arch-vendor-os[-env]`\n"
    "                   form (x86_64-unknown-linux-gnu, x86_64-pc-windows-msvc,\n"
    "                   aarch64-unknown-linux-gnu; default\n"
    "                   x86_64-unknown-linux-gnu). A triple this compiler does\n"
    "                   not state is refused, never guessed\n"
    "  --at POS         pp: [file:]line; resolve: [file:]line:col\n"
    "  -Wunused         warn about declarations nothing refers to\n"
    "  -Wshadow         warn about a declaration that hides another one\n"
    "  -Wconversion     warn about an implicit conversion that may lose\n"
    "                   information\n"
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
