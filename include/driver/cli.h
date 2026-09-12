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
#include <vector>

namespace minc::driver {

// Subcommands the driver understands. Anything else is a usage error.
enum class Command : std::uint8_t { Build, Run, Check, Lex };

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
  std::optional<Command> command;
  std::vector<std::string> inputs; // files and pass-through arguments
  std::string error;               // non-empty => usage error; ignore the rest
};

// Parses argv[1..argc). Accepts a possibly-null argv[i] (some CRTs allow it).
[[nodiscard]] CliOptions parseArgs(int argc, const char* const* argv);

} // namespace minc::driver
