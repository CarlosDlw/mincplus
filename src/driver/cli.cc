// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "driver/cli.h"

#include <array>

namespace minc::driver {
namespace {

constexpr std::array<CommandInfo, 7> kCommands{{
    {Command::Build, "build", "<files...>", "Compile sources and link an executable", false},
    {Command::Run, "run", "<files...>", "Build and run the resulting program", false},
    {Command::Check, "check", "[options] <files...>",
     "Check only: type-check every unit; silent on success, no code is emitted", true},
    // The three stages, each with the view it is responsible for, and the
    // pipeline written out where a reader looks for it: `lex` is the raw bytes
    // of one file (which is why a `#` is an error there), `pp` is the token
    // stream of the translation unit, `parse` is the tree over that stream.
    {Command::Lex, "lex", "<files...>", "Lex one file; raw tokens, no preprocessing", true},
    {Command::Parse, "parse", "[options] <files...>",
     "Preprocess and parse each file; print its syntax tree", true},
    {Command::Pp, "pp", "[options] <files...>",
     "Preprocess each file; -D/-U/-I, --defines, --includes, --deps, --at", true},
    // `resolve` is the first command that looks at *meaning*: it lowers the tree,
    // resolves every name, and prints the scopes and definitions. It is also the
    // first consumer of the two stages after the parser, and the command that
    // proves them.
    {Command::Resolve, "resolve", "[options] <files...>",
     "Resolve names; scopes/defs/refs, --ast, --refs, --unresolved, --at", true},
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
  case Command::Lex:
    return "lex";
  case Command::Parse:
    return "parse";
  case Command::Pp:
    return "pp";
  case Command::Resolve:
    return "resolve";
  }
  return "unknown";
}

std::vector<std::pair<std::string, std::string>>
splitDefines(const std::vector<std::string>& defines) {
  std::vector<std::pair<std::string, std::string>> out;
  out.reserve(defines.size());
  for (const std::string& define : defines) {
    const std::size_t equals = define.find('=');
    if (equals == std::string::npos) {
      out.emplace_back(define, std::string{});
    } else {
      out.emplace_back(define.substr(0, equals), define.substr(equals + 1));
    }
  }
  return out;
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
      if (arg == "--no-trivia") {
        opts.hideTrivia = true;
        continue;
      }
      // `-isystem dir` (or `-isystemdir`, which GCC also accepts). It has to be
      // matched before the single-letter options below: `-i` is not one of them,
      // so without this it would be an unrecognized option.
      if (arg == "-isystem" || arg.rfind("-isystem", 0) == 0) {
        std::string value(arg.substr(8));
        if (value.empty()) {
          if (i + 1 < argc) {
            value = argv[i + 1] != nullptr ? argv[++i] : "";
          }
        }
        if (value.empty()) {
          opts.error = "option '-isystem' needs a value";
          return opts;
        }
        opts.systemDirs.push_back(std::move(value));
        continue;
      }
      // `-D`/`-U`/`-I` take a value, joined or separate. Both spellings are
      // accepted because half the world writes `-DFOO=1` and the other half
      // `-D FOO=1`, and a compiler that accepts only one is a paper cut.
      if (arg.size() >= 2 && arg[0] == '-' && (arg[1] == 'D' || arg[1] == 'U' || arg[1] == 'I')) {
        std::string value(arg.substr(2));
        if (value.empty()) {
          if (i + 1 < argc) {
            value = argv[i + 1] != nullptr ? argv[++i] : "";
          }
        }
        if (value.empty()) {
          opts.error = "option '" + std::string(arg) + "' needs a value";
          return opts;
        }
        switch (arg[1]) {
        case 'D':
          opts.defines.push_back(std::move(value));
          break;
        case 'U':
          opts.undefines.push_back(std::move(value));
          break;
        default:
          opts.includeDirs.push_back(std::move(value));
          break;
        }
        continue;
      }
      if (arg == "--defines" || arg == "--includes" || arg == "--deps") {
        opts.showDefines = opts.showDefines || arg == "--defines";
        opts.showIncludes = opts.showIncludes || arg == "--includes";
        opts.showDeps = opts.showDeps || arg == "--deps";
        continue;
      }
      if (arg == "--refs" || arg == "--unresolved" || arg == "--ast" || arg == "--types" ||
          arg == "--stats") {
        opts.showRefs = opts.showRefs || arg == "--refs";
        opts.showUnresolved = opts.showUnresolved || arg == "--unresolved";
        opts.showAst = opts.showAst || arg == "--ast";
        opts.showTypes = opts.showTypes || arg == "--types";
        opts.stats = opts.stats || arg == "--stats";
        continue;
      }
      // `--target NAME`. The name is validated by the command, not here, so the
      // parser never has to know the table of targets -- and so the error names
      // the ones that exist instead of just rejecting a string.
      if (arg == "--target" || arg.rfind("--target=", 0) == 0) {
        std::string value = arg == "--target" ? std::string{} : std::string(arg.substr(9));
        if (value.empty()) {
          if (i + 1 < argc) {
            value = argv[i + 1] != nullptr ? argv[++i] : "";
          }
        }
        if (value.empty()) {
          opts.error = "option '--target' needs a name";
          return opts;
        }
        opts.target = std::move(value);
        continue;
      }
      // `-Wunused` / `-Wshadow`. Written the way every C compiler writes them,
      // and an unknown `-W` is a usage error rather than a silent no-op: a
      // warning somebody asked for and did not get is worse than a typo caught
      // now.
      if (arg.size() > 2 && arg[0] == '-' && arg[1] == 'W') {
        const std::string_view name = arg.substr(2);
        if (name == "unused") {
          opts.warnUnused = true;
        } else if (name == "shadow") {
          opts.warnShadow = true;
        } else if (name == "conversion") {
          opts.warnConversion = true;
        } else {
          opts.error = "unknown warning option '" + std::string(arg) + "'";
          return opts;
        }
        continue;
      }
      if (arg == "--at") {
        if (i + 1 >= argc || argv[i + 1] == nullptr) {
          opts.error = "option '--at' needs '[file:]line'";
          return opts;
        }
        opts.at = argv[++i];
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
