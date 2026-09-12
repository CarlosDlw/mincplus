// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "driver/cli.h"

#include <array>

namespace minc::driver {
namespace {

constexpr std::array<CommandInfo, 3> kCommands{{
    {Command::Build, "build", "<files...>", "Compile sources and link an executable"},
    {Command::Run, "run", "<files...>", "Build and run the resulting program"},
    {Command::Check, "check", "<files...>", "Parse and type-check only; no code is emitted"},
}};

// A lone "-" and any argument not starting with '-' are positional. Doing this
// by hand (instead of relying on getopt) keeps behaviour identical on every
// platform, including Windows where getopt is absent.
[[nodiscard]] bool isPositional(std::string_view arg) {
  return arg.empty() || arg == "-" || arg.front() != '-';
}

} // namespace

std::span<const CommandInfo> allCommands() {
  return kCommands;
}

const char* toString(Command command) {
  switch (command) {
  case Command::Build:
    return "build";
  case Command::Run:
    return "run";
  case Command::Check:
    return "check";
  }
  return "unknown";
}

std::optional<Command> commandFromName(std::string_view name) {
  for (const CommandInfo& info : kCommands) {
    if (info.name == name) {
      return info.command;
    }
  }
  return std::nullopt;
}

CliOptions parseArgs(int argc, const char* const* argv) {
  CliOptions opts;
  bool optionsEnded = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv != nullptr && argv[i] != nullptr ? argv[i] : "";
    if (arg.empty()) {
      continue;
    }

    if (!optionsEnded) {
      if (arg == "--") {
        optionsEnded = true;
        continue;
      }
      if (arg == "-h" || arg == "--help") {
        opts.showHelp = true;
        continue;
      }
      if (arg == "-V" || arg == "--version") {
        opts.showVersion = true;
        continue;
      }
      if (!isPositional(arg)) {
        opts.error = "unrecognized option '" + std::string(arg) + "'";
        return opts;
      }
    }

    // The first positional argument names the subcommand; the rest are inputs.
    if (!opts.command.has_value()) {
      const std::optional<Command> command = commandFromName(arg);
      if (!command.has_value()) {
        opts.error = "unknown command '" + std::string(arg) + "'";
        return opts;
      }
      opts.command = command;
      continue;
    }
    opts.inputs.emplace_back(arg);
  }

  return opts;
}

} // namespace minc::driver
