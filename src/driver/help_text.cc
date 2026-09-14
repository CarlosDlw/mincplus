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
    "  --               Treat every following argument as a file, not an option\n"
    "\n"
    "build and run options:\n"
    "  -o PATH          where the output goes. Default a.out (a.exe on a\n"
    "                   Windows target); for --emit obj/asm it defaults to the\n"
    "                   input with its extension replaced. `-o -` writes the\n"
    "                   object or listing to standard output\n"
    "  -O LEVEL         O0, O1, O2, O3, Os, Oz; default O0. A bare `-O` is -O1\n"
    "  -g               emit debug information (DWARF on ELF and Mach-O,\n"
    "                   CodeView on PE), read by gdb, lldb and llvm-dwarfdump\n"
    "  --emit KIND      exe, obj or asm; default exe\n"
    "  -L DIR           add a directory to the linker driver's search list\n"
    "  -l NAME          link with a library; order is meaning\n"
    "  --linker PATH    the linker *driver* to use: clang, cc, gcc, or a path.\n"
    "                   By default the first of clang, cc, gcc found on PATH\n"
    "  --sysroot DIR    forwarded to the linker driver; required with -L/-l\n"
    "                   when the target is not the host\n"
    "  -v               print the commands the build runs\n"
    "\n"
    "run also accepts everything above, and:\n"
    "  -- args...       everything after `--` is passed to the program and\n"
    "                   interpreted by nobody: `mincc run p.mx -- -o` passes\n"
    "                   the single argument -o. `run` exits with the program's\n"
    "                   own status\n";

// `run` exits with the program's own status, which is why the table says so and
// not "0 on success": a script that runs `mincc run` sees what the program
// returned, the way `sh -c` reports it. A program killed by a signal did not
// return a status, and that is reported as a failure with a message: naming the
// signal would take platform code, and `backend` contains none.
constexpr const char* kExitStatusBlock = "Exit status:\n"
                                         "  0  success\n"
                                         "  1  a diagnostic was printed (compiler, toolchain,\n"
                                         "     or a program `run` could not execute or that\n"
                                         "     did not exit normally)\n"
                                         "  2  invalid command line\n"
                                         "  n  `run` only: the program's own status\n";

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
  out += "C interop: plain C ABI objects and libraries; the link is driven by a C\n";
  out += "           compiler driver (clang/cc/gcc), so a foreign linker is never\n";
  out += "           second-guessed. `-L` and `-l` reach it unchanged\n";
  out += "Targets:   x86_64, aarch64, riscv64, i386 -- Linux, macOS, Windows and\n";
  out += "           FreeBSD; --target selects the ABI for `check`, `ir` and\n";
  out += "           `build --emit obj/asm`. An executable is only linked for\n";
  out += "           the host unless --linker and --sysroot are given\n\n";
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
