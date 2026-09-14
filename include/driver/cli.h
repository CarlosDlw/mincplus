// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Command-line parsing for `mincc`, which is a reader of the spec table.
//
// `parseArgs` is pure: it never prints, never exits, and never throws. It also
// never *decides* what a command accepts -- `command_spec.h` does, and this file
// reads it. I/O and exit codes live in `help_text.h` and `main.cc`, so the
// parser stays trivially testable and reusable.
//
// Three of its rules are worth stating because they are deliberate and a test
// pins each one:
//
//  - **Anything `-h`/`--help` can be found in, it is found in**, before any
//    other argument can become an error, and a `--` stops that search: the
//    guide every CLI follows asks that `-h` work at the end of a broken command
//    line. `-h` is never overloaded to mean something else.
//  - **An option that belongs to another command is an error, not a no-op.**
//    `mincc lex --emit obj` was silently ignored before; an option accepted and
//    then ignored is worse than one refused, because the user believes it did
//    something.
//  - **A value is taken from the next argument only when the spec says the value
//    is required.** `-O` is the case that makes this matter: `-O` alone is one
//    level and must not eat the file name after it.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "driver/command_spec.h"
#include "support/term/terminal.h"

namespace minc::driver {

struct CliOptions {
  // `--help`/`-h`, or the `help` command. `helpTopic` is the command the topic
  // was about, when one was named and it exists.
  bool showHelp = false;
  std::optional<Command> helpTopic;
  bool showVersion = false;
  // Nothing but the program name was given. Not the same as "help was asked
  // for": the text is the same and the stream and exit code are not.
  bool emptyCommandLine = false;
  // `--color=auto|always|never`.
  support::ColorChoice colorChoice = support::ColorChoice::Auto;

  // `--no-trivia`: leave whitespace and comments out of dump output. It is a
  // display filter, not a lexer mode -- the token stream keeps every byte
  // either way -- so it is an option of the commands that print tokens.
  bool hideTrivia = false;
  std::optional<Command> command;
  std::vector<std::string> inputs; // files, and standard input as `-`
  // Preprocessor inputs, in the order they were written: order is meaning, both
  // for `-D`/`-U` (a later one wins) and for `-I` (the search order).
  std::vector<std::string> defines;
  std::vector<std::string> undefines;
  std::vector<std::string> includeDirs;
  // `-isystem dir`: searched after every `-I`, and the files found there are
  // *system headers*, which means warnings in them are suppressed. The order of
  // the two lists is meaning, so they are kept apart rather than concatenated.
  std::vector<std::string> systemDirs;
  // `--defines`, `--includes`, `--deps`: what `pp` should print. Not a single
  // enum, because asking for two of them at once is meaningful.
  bool showDefines = false;
  bool showIncludes = false;
  bool showDeps = false;
  // `--refs`/`--unresolved`: what `resolve` should print. Both at once is
  // meaningful (every use, with the unresolved ones marked), so not one enum.
  bool showRefs = false;
  bool showUnresolved = false;
  // `--ast`: print the tree instead of the tables. For `check` it is the *typed*
  // tree, which has a type on every node.
  bool showAst = false;
  // `--types`: print only the type table.
  bool showTypes = false;
  // `--stats`: one summary line per input, and no tables. `check` prints nothing
  // on success without it.
  bool stats = false;
  // `--target`: the ABI the C type spellings and the layout are read against. A
  // triple, resolved through `sema/target.h`, so a target can only mean the row
  // that table states for it.
  std::string target;
  // `-Wunused`, `-Wshadow`. Off by default, like every other warning here: a
  // compiler that warns about ordinary code teaches people to ignore it.
  bool warnUnused = false;
  bool warnShadow = false;
  // `-Wconversion`. Off by default for the same reason as the others.
  bool warnConversion = false;
  // `--at [file:]line` for `pp`, `[file:]line:col` for `resolve`.
  std::string at;

  // --- `build` and `run` -------------------------------------------------------
  //
  // Kept apart from the display flags above because these change what is
  // *produced*, not what is printed: a typo in one of them is not a cosmetic
  // difference, so each has a real value and none of them defaults to "whatever".

  // `-g`. Debug information; see `codegen.md` § *Debug information*.
  bool debugInfo = false;
  // `-v`. Print the exact commands the build runs, which is the first thing a
  // reader wants after a link failure. With `--version`, the version becomes the
  // block a bug report needs.
  bool verbose = false;
  // `-o PATH`. Empty means the command's default, which differs per emit kind, so
  // the choice is made where the emit kind is known and not here.
  std::string output;
  // `-O`. Stored as the letters after the `-O` (`"2"`, `"s"`, `"z"`), because the
  // spelling and its meaning are one thing and `backend` validates it.
  std::string optLevel = "0";
  // `--emit KIND`: `exe`, `obj`, `asm`. Validated by the command, which is where
  // the sentence for an unknown kind can name the ones that exist.
  std::string emit = "exe";
  // `-L DIR` / `-l NAME`, in the order written: link order is meaning.
  std::vector<std::string> libraryDirs;
  std::vector<std::string> libraries;
  // `--linker PATH`: the linker *driver* (clang/cc/gcc), never a raw linker.
  std::string linker;
  // `--sysroot DIR`: forwarded to the driver; required with `-L`/`-l` when the
  // target is not the host.
  std::string sysroot;
  // Everything after `--` when the command is `run`. Interpreted by nobody: the
  // whole point of the separator is that `mincc run p.mx -- -o --emit` passes two
  // ordinary arguments to the program.
  std::vector<std::string> programArgs;
  // Whether a `--` was seen. Kept, because "no program arguments" and "program
  // arguments after the separator" are different states of a `run` invocation.
  bool sawDoubleDash = false;

  std::string error; // non-empty => usage error; ignore the rest
  // A spelling to offer with the error, empty when nothing was close enough.
  std::string suggestion;
};

// Parses argv[1..argc). Accepts a possibly-null argv[i] (some CRTs allow it).
//
// `target` is filled with the compiler's default triple when no `--target` was
// given, which is the same constant `sema` validates against -- one spelling,
// read from that table rather than restated here.
[[nodiscard]] CliOptions parseArgs(int argc, const char* const* argv);

// `-D name[=body]`, written as one string, split into the pairs the preprocessor
// takes. One implementation so every command that preprocesses cannot disagree
// about what `-DNAME=` means (an *empty* body, which is a define and not a
// no-op).
[[nodiscard]] std::vector<std::pair<std::string, std::string>>
splitDefines(const std::vector<std::string>& defines);

} // namespace minc::driver
