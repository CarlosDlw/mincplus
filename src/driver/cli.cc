// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "driver/cli.h"

#include <array>

namespace minc::driver {
namespace {

constexpr std::array<CommandInfo, 8> kCommands{{
    // The first two commands that produce something other than a diagnostic, and
    // the first that hand work to a linker: `build` emits objects and drives a C
    // linker driver, `run` is `build` plus an `exec` (`codegen.md`).
    {Command::Build, "build", "[options] <files...>",
     "Compile and link an executable; -o, -O, -g, --emit, --target, -L, -l", true},
    {Command::Run, "run", "[options] <files...> [-- args...]",
     "Build a program and run it; everything after `--` goes to the program", true},
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
    // `ir` is the first command past the front end: it lowers each checked unit
    // to LLVM IR and prints the module, which is what makes the lowering
    // reviewable and `make examples` a test of it.
    {Command::Ir, "ir", "[options] <files...>",
     "Lower each file to LLVM IR; print the module, --target selects the ABI", true},
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
  case Command::Ir:
    return "ir";
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
        opts.sawDoubleDash = true;
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
      // `-g` and `-v`. Matched before the value-taking options below so a
      // single-letter flag can never be mistaken for one whose value is joined.
      if (arg == "-g") {
        opts.debugInfo = true;
        continue;
      }
      if (arg == "-v") {
        opts.verbose = true;
        continue;
      }
      // `-O LEVEL`. The level is the letter(s) after `-O`, and a bare `-O` means
      // `-O1` -- both are what GCC and Clang do, and a compiler that accepts only
      // one spelling is a paper cut in a build script.
      if (arg.size() >= 2 && arg[0] == '-' && arg[1] == 'O') {
        std::string level(arg.substr(2));
        if (level.empty()) {
          level = "1";
        }
        opts.optLevel = std::move(level);
        continue;
      }
      // `-o PATH`, joined or separate. Last one wins, like every C compiler: a
      // build script that appends `-o` means the appended one.
      if (arg == "-o" || (arg.size() > 2 && arg.rfind("-o", 0) == 0)) {
        std::string value = arg.size() > 2 ? std::string(arg.substr(2)) : std::string{};
        if (value.empty()) {
          if (i + 1 < argc) {
            value = argv[i + 1] != nullptr ? argv[++i] : "";
          }
        }
        if (value.empty()) {
          opts.error = "option '-o' needs a file name";
          return opts;
        }
        opts.output = std::move(value);
        continue;
      }
      // `-L DIR` / `-l NAME`, joined or separate, and matched before the
      // `-D`/`-U`/`-I` branch only because neither letter collides with those.
      if (arg.size() >= 2 && arg[0] == '-' && (arg[1] == 'L' || arg[1] == 'l')) {
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
        if (arg[1] == 'L') {
          opts.libraryDirs.push_back(std::move(value));
        } else {
          opts.libraries.push_back(std::move(value));
        }
        continue;
      }
      // `--emit KIND`, `--linker PATH`, `--sysroot DIR`. All three accept the
      // joined and the separate spelling; the value is understood by the command,
      // which is where the message can name the alternatives.
      const auto valueOption = [&](std::string_view name, std::string& out) {
        const std::string_view argView(arg);
        if (argView == name) {
          if (i + 1 >= argc || argv[i + 1] == nullptr) {
            opts.error = "option '" + std::string(name) + "' needs a value";
            return true;
          }
          out = argv[++i];
          return true;
        }
        if (argView.size() > name.size() && argView.rfind(name, 0) == 0 &&
            argView[name.size()] == '=') {
          out = std::string(argView.substr(name.size() + 1));
          if (out.empty()) {
            opts.error = "option '" + std::string(name) + "' needs a value";
          }
          return true;
        }
        return false;
      };
      if (valueOption("--emit", opts.emit) || valueOption("--linker", opts.linker) ||
          valueOption("--sysroot", opts.sysroot)) {
        if (!opts.error.empty()) {
          return opts;
        }
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
    // After a `--` in a `run`, a positional is the *program's*, not the
    // compiler's. Nothing here looks at it -- it is copied and handed to `exec`
    // -- which is what makes `-- -o --emit` two ordinary arguments.
    if (opts.sawDoubleDash && opts.command == Command::Run) {
      opts.programArgs.emplace_back(arg);
      continue;
    }
    opts.inputs.emplace_back(arg);
  }

  return opts;
}

} // namespace minc::driver
