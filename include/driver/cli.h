// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Command-line parsing for `mincc`.
//
// parseArgs is pure: it never prints, never exits, and never throws. I/O and
// exit codes live in help_text.h/main.cc so the parser stays trivially
// testable and reusable.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace minc::driver {

// Subcommands the driver understands. Anything else is a usage error.
enum class Command : std::uint8_t { Build, Run, Check, Lex, Parse, Pp, Resolve };

// Name, argument shape, and one-line description of a subcommand. Kept in one
// table so the parser, error messages, and help text cannot drift apart.
struct CommandInfo {
  Command command;
  std::string_view name;
  std::string_view args;
  std::string_view summary;
  // Whether the command does anything yet. Help derives its "implemented" list
  // from this instead of naming commands in prose, so a command cannot be
  // advertised as working while the dispatch still refuses it.
  bool implemented;
};

[[nodiscard]] std::span<const CommandInfo> allCommands();
[[nodiscard]] const char* toString(Command command);
[[nodiscard]] std::optional<Command> commandFromName(std::string_view name);

struct CliOptions {
  bool showHelp = false;
  bool showVersion = false;
  // `--no-trivia`: leave whitespace and comments out of dump output. It is a
  // display filter, not a lexer mode -- the token stream keeps every byte
  // either way -- so it is a global option rather than a per-command one.
  bool hideTrivia = false;
  std::optional<Command> command;
  std::vector<std::string> inputs; // files and pass-through arguments
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
  // `--ast`: print the lowered AST instead of the scope/def tables. For `check`
  // it is the *typed* tree, which has a type on every node.
  bool showAst = false;
  // `--types`: print only the type table.
  bool showTypes = false;
  // `--target`: the ABI the C type spellings and the layout are read against.
  // A name, resolved through `sema/target.h`, so a target can only mean the row
  // that table prints for it.
  std::string target = "systemv-amd64";
  // `-Wunused`, `-Wshadow`. Off by default, like every other warning here: a
  // compiler that warns about ordinary code teaches people to ignore it.
  bool warnUnused = false;
  bool warnShadow = false;
  // `-Wconversion`. Off by default for the same reason as the others.
  bool warnConversion = false;
  // `--at [file:]line` for `pp`, `[file:]line:col` for `resolve`.
  std::string at;
  std::string error; // non-empty => usage error; ignore the rest
};

// Parses argv[1..argc). Accepts a possibly-null argv[i] (some CRTs allow it).
[[nodiscard]] CliOptions parseArgs(int argc, const char* const* argv);

// `-D name[=body]`, written as one string, split into the pairs the preprocessor
// takes. One implementation so every command that preprocesses cannot disagree
// about what `-DNAME=` means (an *empty* body, which is a define and not a
// no-op).
[[nodiscard]] std::vector<std::pair<std::string, std::string>>
splitDefines(const std::vector<std::string>& defines);

} // namespace minc::driver
