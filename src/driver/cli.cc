// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "driver/cli.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "driver/suggest.h"
#include "sema/target.h"

namespace minc::driver {
namespace {

// One option the user typed, in the order it was typed. Collected first and
// applied afterwards, because whether an option is even *allowed* depends on the
// command, and the command may be named after it on the line.
struct Occurrence {
  OptionId id;
  std::string value;
};

// The two flags that outrank everything else on the line, found before the parse
// proper can turn any other argument into an error.
//
// This is `clig.dev`'s rule -- "you should be able to add -h to the end of
// anything and it should show help" -- and it is a separate pass for one reason:
// `mincc --nosuch -h` must print help, and a single pass that reports the first
// problem it meets cannot.
struct PreScan {
  bool help = false;
  bool version = false;
  // `help` as the first positional: `mincc help build`, the git-like spelling the
  // guide asks for, and the reason `help` is not a command of the table (it takes
  // a command *name*, not a file).
  bool helpCommand = false;
  int helpWordIndex = -1;
  // Whether help or version was asked for as a *flag*. This is the one thing that
  // outranks a problem elsewhere on the line, and it is deliberately not the same
  // as `showHelp`: `mincc --nosuch -h` prints the page because the flag outranks
  // the unknown option, but `mincc help buidl` names a topic that does not exist
  // and that is the whole answer. The `help` *command* is an ordinary command; its
  // line still has to be valid.
  bool flagForm = false;
};

// `-vV` and `-Vv`: a cluster of one-letter flags, accepted as a unit because
// `rustc` made that spelling muscle memory for "the version block, verbose". A
// general cluster rule lives in the parse proper; this one exists so the pre-scan
// can see the version request before anything else can fail.
[[nodiscard]] bool isVersionCluster(std::string_view arg) {
  if (arg.size() < 3 || arg[0] != '-' || arg[1] == '-') {
    return false;
  }
  bool sawVersion = false;
  for (std::size_t index = 1; index < arg.size(); ++index) {
    if (arg[index] == 'V') {
      sawVersion = true;
    } else if (arg[index] != 'v') {
      return false;
    }
  }
  return sawVersion;
}

// The words a positional could have been meant as, for the suggestion: the
// options the command being typed actually accepts, or every option when no
// command has been named yet.
[[nodiscard]] std::span<const std::string_view> suggestionCandidates(const CliOptions& opts) {
  return opts.command.has_value() ? optionNamesOf(*opts.command) : allOptionNames();
}

[[nodiscard]] std::string quoted(std::string_view text) {
  return "'" + std::string(text) + "'";
}

// The owners of an option, as a phrase: "build and run", "check", "ir and build".
[[nodiscard]] std::string ownersPhrase(OptionId id) {
  std::vector<std::string_view> names;
  for (const Command owner : ownersOf(id)) {
    names.push_back(toString(owner));
  }
  std::string phrase;
  for (std::size_t index = 0; index < names.size(); ++index) {
    if (index != 0) {
      phrase += index + 1 == names.size() ? " and " : ", ";
    }
    phrase += names[index];
  }
  return phrase;
}

// The first owner, for a suggestion that names one page to read.
[[nodiscard]] std::string firstOwner(OptionId id) {
  const std::span<const Command> owners = ownersOf(id);
  return owners.empty() ? std::string{} : std::string(toString(owners.front()));
}

void failUnknownOption(CliOptions& opts, std::string_view spelling) {
  opts.error = "unrecognized option " + quoted(spelling);
  if (const std::optional<std::string_view> near =
          nearestName(suggestionCandidates(opts), spelling)) {
    opts.suggestion = std::string(*near);
  }
}

void failUnknownCommand(CliOptions& opts, std::string_view spelling) {
  opts.error = "unknown command " + quoted(spelling);
  if (const std::optional<std::string_view> near = nearestName(commandNames(), spelling)) {
    opts.suggestion = std::string(*near);
    return;
  }
  // A file where a command belongs is the mistake this catches: `mincc main.mx`.
  // The guide's advice is to suggest the corrected command line rather than to run
  // it, and there is only one sensible command to name here.
  if (spelling.find('.') != std::string_view::npos) {
    opts.suggestion = std::string(programSpec().name) + " run " + std::string(spelling);
  }
}

[[nodiscard]] PreScan preScanArgs(int argc, const char* const* argv) {
  PreScan prescan;
  for (int i = 1; i < argc; ++i) {
    const char* raw = argv != nullptr ? argv[i] : nullptr;
    if (raw == nullptr || *raw == '\0') {
      continue;
    }
    const std::string_view arg(raw);
    if (arg == "--") {
      // A program argument is not this compiler's: `mincc run p.mx -- -h` runs a
      // program that is passed `-h`.
      break;
    }
    if (arg == "-h" || arg == "--help") {
      prescan.help = true;
      prescan.flagForm = true;
      continue;
    }
    if (arg == "-V" || arg == "--version" || isVersionCluster(arg)) {
      prescan.version = true;
      prescan.flagForm = true;
      continue;
    }
    // `help` is only the help command as the *first* positional; anywhere else it
    // is a file name, and the scan keeps looking for `-h` either way.
    if (arg.front() != '-' && prescan.helpWordIndex < 0) {
      if (arg == "help") {
        prescan.help = true;
        prescan.helpCommand = true;
        prescan.helpWordIndex = i;
      }
      continue;
    }
  }
  return prescan;
}

// The parse proper: everything except the two flags that outrank it.
void parseInto(CliOptions& opts, const PreScan& prescan, int argc, const char* const* argv) {
  const auto argumentAt = [argc, argv](int index) -> std::string_view {
    if (argv == nullptr || index >= argc || argv[index] == nullptr) {
      return std::string_view{};
    }
    return argv[index];
  };
  const auto hasValueAt = [&argumentAt](int index) { return !argumentAt(index).empty(); };

  std::vector<Occurrence> occurrences;
  bool optionsEnded = false;
  bool topicSeen = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argumentAt(i);
    if (arg.empty()) {
      continue;
    }

    if (!optionsEnded) {
      if (arg == "--") {
        optionsEnded = true;
        opts.sawDoubleDash = true;
        continue;
      }
      // A lone `-` is standard input, which is a file and not an option.
      if (arg.front() == '-' && arg != "-") {
        // --- `--name`, and `--name=value` ----------------------------------
        if (arg.rfind("--", 0) == 0) {
          const std::size_t equals = arg.find('=');
          const std::string_view name =
              equals == std::string_view::npos ? arg : arg.substr(0, equals);
          const std::string_view joined =
              equals == std::string_view::npos ? std::string_view{} : arg.substr(equals + 1);
          const OptionSpec* spec = findOptionByName(name);
          if (spec == nullptr) {
            failUnknownOption(opts, name);
            return;
          }
          if (!spec->takesValue()) {
            if (equals != std::string_view::npos) {
              opts.error = "option " + quoted(name) + " does not take a value";
              return;
            }
            occurrences.push_back({spec->id, {}});
            continue;
          }
          if (equals != std::string_view::npos) {
            if (joined.empty()) {
              opts.error = "option " + quoted(name) + " needs a value";
              return;
            }
            occurrences.push_back({spec->id, std::string(joined)});
            continue;
          }
          if (!hasValueAt(i + 1)) {
            opts.error = "option " + quoted(name) + " needs a value";
            return;
          }
          occurrences.push_back({spec->id, std::string(argumentAt(++i))});
          continue;
        }

        // --- the exact single-dash spelling: -o, -O, -g, -v, -Wshadow --------
        if (const OptionSpec* spec = findOptionByName(arg)) {
          if (!spec->takesValue() || spec->value == ValueKind::Optional) {
            // A bare `-O` is a level of its own and never takes the next
            // argument: `mincc build -O main.mx` compiles `main.mx` at -O1.
            occurrences.push_back({spec->id, {}});
            continue;
          }
          if (!hasValueAt(i + 1)) {
            opts.error = "option " + quoted(arg) + " needs a value";
            return;
          }
          occurrences.push_back({spec->id, std::string(argumentAt(++i))});
          continue;
        }

        // --- a joined value on a single-dash name: -DFOO, -Ipath, -o/path ----
        if (const OptionSpec* spec = findJoinedValueOption(arg)) {
          occurrences.push_back({spec->id, std::string(arg.substr(spec->name.size()))});
          continue;
        }

        // --- a single letter: -h, -V ----------------------------------------
        if (arg.size() == 2) {
          if (const OptionSpec* spec = findOptionByShortName(arg[1])) {
            occurrences.push_back({spec->id, {}});
            continue;
          }
        }

        // --- a cluster of one-letter flags: -vV, -vh -------------------------
        if (arg.size() > 2) {
          std::vector<OptionId> clustered;
          bool cluster = true;
          for (std::size_t index = 1; index < arg.size(); ++index) {
            const OptionSpec* spec = findOptionByShortName(arg[index]);
            // Only options that take no value may be clustered: `-vo out` is not
            // a spelling this compiler accepts, and guessing which letter owns the
            // value is how a CLI starts inventing grammar.
            if (spec == nullptr || spec->takesValue()) {
              cluster = false;
              break;
            }
            clustered.push_back(spec->id);
          }
          if (cluster) {
            for (const OptionId id : clustered) {
              occurrences.push_back({id, {}});
            }
            continue;
          }
        }

        // --- the warning family, which has a sentence of its own -------------
        if (arg.rfind("-W", 0) == 0) {
          opts.error = "unknown warning option " + quoted(arg);
          std::vector<std::string_view> warnings;
          for (const std::string_view name : allOptionNames()) {
            if (name.rfind("-W", 0) == 0) {
              warnings.push_back(name);
            }
          }
          if (const std::optional<std::string_view> near = nearestName(warnings, arg)) {
            opts.suggestion = std::string(*near);
          }
          return;
        }

        failUnknownOption(opts, arg);
        return;
      }
    }

    // --- positionals ---------------------------------------------------------
    if (i == prescan.helpWordIndex) {
      continue; // the word `help` itself, not a command
    }
    if (!opts.command.has_value()) {
      // `mincc help build`: the topic is the next positional.
      if (prescan.helpCommand && !topicSeen) {
        topicSeen = true;
        const std::optional<Command> topic = commandFromName(arg);
        if (!topic.has_value()) {
          failUnknownCommand(opts, arg);
          return;
        }
        opts.helpTopic = topic;
        continue;
      }
      if (prescan.helpCommand) {
        opts.error =
            "the help command takes one command name, and " + quoted(arg) + " is a second one";
        return;
      }
      const std::optional<Command> command = commandFromName(arg);
      if (!command.has_value()) {
        failUnknownCommand(opts, arg);
        return;
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

  // --- who may say what -----------------------------------------------------
  //
  // Help and version outrank the rest of the line, so an option from the wrong
  // command is not an error on a line that asked for help: `mincc lex --emit obj
  // -h` prints the page, which is what `parseArgs` below relies on.
  if (!opts.showHelp && !opts.showVersion) {
    for (const Occurrence& occurrence : occurrences) {
      const OptionSpec& spec = optionById(occurrence.id);
      if (!opts.command.has_value()) {
        if (!isGlobalOption(spec.id)) {
          opts.error = quoted(spec.name) + " is an option of " + ownersPhrase(spec.id) +
                       "; name the command first";
          return;
        }
        continue;
      }
      if (!optionAppliesTo(spec.id, *opts.command)) {
        const std::string owners = ownersPhrase(spec.id);
        opts.error = quoted(spec.name) + " is not an option of " + quoted(toString(*opts.command)) +
                     (owners.empty() ? std::string{} : "; it belongs to " + owners);
        // The page that documents it is a better answer than a spelling
        // neighbour, and one line away.
        const std::string owner = firstOwner(spec.id);
        if (!owner.empty()) {
          opts.suggestion = "mincc help " + owner;
        }
        return;
      }
    }
  }

  // --- applying -------------------------------------------------------------
  for (const Occurrence& occurrence : occurrences) {
    const OptionSpec& spec = optionById(occurrence.id);
    switch (occurrence.id) {
    case OptionId::Help:
      opts.showHelp = true;
      break;
    case OptionId::Version:
      opts.showVersion = true;
      break;
    case OptionId::Color:
      if (const std::optional<support::ColorChoice> choice =
              support::colorChoiceFromName(occurrence.value)) {
        opts.colorChoice = *choice;
      } else {
        // The alternatives are read from the spec, so the sentence cannot list a
        // value the option does not take.
        std::string values;
        for (std::size_t index = 0; index < spec.values.size(); ++index) {
          values += index == 0 ? "" : ", ";
          values += spec.values[index];
        }
        opts.error = "invalid value " + quoted(occurrence.value) + " for " + quoted(spec.name) +
                     "; the values are " + values;
        return;
      }
      break;
    case OptionId::Define:
      opts.defines.push_back(occurrence.value);
      break;
    case OptionId::Undefine:
      opts.undefines.push_back(occurrence.value);
      break;
    case OptionId::Include:
      opts.includeDirs.push_back(occurrence.value);
      break;
    case OptionId::Isystem:
      opts.systemDirs.push_back(occurrence.value);
      break;
    case OptionId::Target:
      opts.target = occurrence.value;
      break;
    case OptionId::NoTrivia:
      opts.hideTrivia = true;
      break;
    case OptionId::Ast:
      opts.showAst = true;
      break;
    case OptionId::Types:
      opts.showTypes = true;
      break;
    case OptionId::Stats:
      opts.stats = true;
      break;
    case OptionId::Refs:
      opts.showRefs = true;
      break;
    case OptionId::Unresolved:
      opts.showUnresolved = true;
      break;
    case OptionId::ListDefines:
      opts.showDefines = true;
      break;
    case OptionId::ListIncludes:
      opts.showIncludes = true;
      break;
    case OptionId::ListDeps:
      opts.showDeps = true;
      break;
    case OptionId::At:
      opts.at = occurrence.value;
      break;
    case OptionId::Output:
      opts.output = occurrence.value; // last one wins, as every C compiler does
      break;
    case OptionId::OptLevel:
      opts.optLevel = occurrence.value.empty() ? "1" : occurrence.value;
      break;
    case OptionId::DebugInfo:
      opts.debugInfo = true;
      break;
    case OptionId::Verbose:
      opts.verbose = true;
      break;
    case OptionId::Emit:
      opts.emit = occurrence.value;
      break;
    case OptionId::LibraryDir:
      opts.libraryDirs.push_back(occurrence.value);
      break;
    case OptionId::Library:
      opts.libraries.push_back(occurrence.value);
      break;
    case OptionId::Linker:
      opts.linker = occurrence.value;
      break;
    case OptionId::Sysroot:
      opts.sysroot = occurrence.value;
      break;
    case OptionId::WarnUnused:
      opts.warnUnused = true;
      break;
    case OptionId::WarnShadow:
      opts.warnShadow = true;
      break;
    case OptionId::WarnConversion:
      opts.warnConversion = true;
      break;
    }
  }
}

} // namespace

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

CliOptions parseArgs(int argc, const char* const* argv) {
  CliOptions opts;
  opts.emptyCommandLine = argc <= 1;
  opts.target = std::string(sema::kDefaultTriple);

  const PreScan prescan = preScanArgs(argc, argv);
  opts.showHelp = prescan.help;
  opts.showVersion = prescan.version;

  parseInto(opts, prescan, argc, argv);

  // **The flags outrank a typo on the same line.** The parse stops at the first
  // problem it meets, which is right for a line that meant one thing and wrong
  // for a line that asked for help; clearing the error here is what makes
  // `mincc --nosuch -h` print the page instead of complaining about `--nosuch`,
  // and it is one place rather than a check at every early return.
  //
  // The `help` *command* is not covered on purpose: `mincc help buidl` is a line
  // that asks about a command that does not exist, and swallowing that error
  // would print the general index instead of saying so.
  if (prescan.flagForm && !opts.error.empty()) {
    opts.error.clear();
    opts.suggestion.clear();
  }
  return opts;
}

} // namespace minc::driver
