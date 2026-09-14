// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The table and the parser, proven to be the same list.
//
// This is the test that makes "the help is generated from what the parser
// accepts" a fact rather than an intention. Every option a command's groups name
// is typed at the parser and must be accepted; every option a command's groups do
// *not* name must be refused. Add an option to the parser and forget the table
// and these fail; add it to the table and forget the parser and they fail too.
#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "driver/cli.h"
#include "driver/command_spec.h"

namespace minc::driver {
namespace {

// The command line for `command` with one option on it, in the spelling the table
// gives as primary. A value is supplied when the option takes one -- except for
// the optional case (`-O`), where the point is that it is not needed.
//
// The words are returned, and not the `const char*` array, on purpose: the array
// would point into strings this function owns, and handing out those pointers
// makes every caller read freed memory. The caller below keeps the words alive
// for the whole expression, which is exactly as long as `parseArgs` reads them.
[[nodiscard]] std::vector<std::string> wordsFor(Command command, const OptionSpec& option) {
  std::vector<std::string> words{"mincc", std::string(toString(command)), std::string(option.name)};
  if (option.value == ValueKind::Required) {
    // A value the option actually takes: `--color` validates its own, and the
    // point of the walk is that every documented option is *accepted*, not that a
    // placeholder happens to be well-formed.
    words.emplace_back(option.values.empty() ? std::string("value")
                                             : std::string(option.values.front()));
  }
  words.emplace_back("a.mx");
  return words;
}

// `argv` built by the caller of `parseArgs`, never outliving the words it points
// into: `words` is a reference to a caller's object or to a temporary that lives
// until the end of the full expression, which includes this call.
[[nodiscard]] CliOptions parseWords(const std::vector<std::string>& words) {
  std::vector<const char*> argv;
  argv.reserve(words.size());
  for (const std::string& word : words) {
    argv.push_back(word.c_str());
  }
  return parseArgs(static_cast<int>(argv.size()), argv.data());
}

TEST(CommandSpecTest, EveryCommandHasOneSpec) {
  std::vector<Command> seen;
  for (const CommandSpec& spec : allCommands()) {
    for (const Command other : seen) {
      EXPECT_NE(other, spec.command) << "duplicated row for " << spec.name;
    }
    seen.push_back(spec.command);
    EXPECT_STREQ(toString(spec.command), std::string(spec.name).c_str());
    ASSERT_TRUE(commandFromName(spec.name).has_value()) << spec.name;
    EXPECT_EQ(*commandFromName(spec.name), spec.command);
  }
  EXPECT_EQ(seen.size(), allCommands().size());
}

TEST(CommandSpecTest, TheParserAcceptsEveryOptionTheTableLists) {
  for (const CommandSpec& spec : allCommands()) {
    for (const OptionGroup& group : spec.groups) {
      for (const OptionId id : group.ids) {
        const OptionSpec& option = optionById(id);
        const CliOptions opts = parseWords(wordsFor(spec.command, option));
        EXPECT_TRUE(opts.error.empty()) << spec.name << " " << option.name << ": " << opts.error;
      }
    }
  }
}

TEST(CommandSpecTest, TheParserRefusesEveryOptionTheTableDoesNotList) {
  // The other half of the parity: not one option slips through for a command
  // whose page does not document it. `mincc lex --emit obj` is the case that
  // used to be accepted and silently dropped.
  for (const CommandSpec& spec : allCommands()) {
    for (const OptionSpec& option : allOptions()) {
      if (optionAppliesTo(option.id, spec.command)) {
        continue;
      }
      const CliOptions opts = parseWords(wordsFor(spec.command, option));
      EXPECT_FALSE(opts.error.empty())
          << spec.name << " accepted " << option.name << " without documenting it";
    }
  }
}

TEST(CommandSpecTest, TheGlobalGroupIsTheSameListForEveryCommand) {
  const std::array<OptionId, 3> globals{OptionId::Help, OptionId::Version, OptionId::Color};
  for (const CommandSpec& spec : allCommands()) {
    for (const OptionId id : globals) {
      EXPECT_TRUE(isGlobalOption(id)) << optionById(id).name;
      EXPECT_TRUE(optionAppliesTo(id, spec.command))
          << spec.name << " does not accept " << optionById(id).name;
    }
  }
  // `isGlobalOption` and the help's own group are the same list, read twice: an
  // option in one and not the other is a command that refuses what its page
  // prints, which is the class of bug this whole file exists for.
  for (const OptionSpec& option : allOptions()) {
    if (optionAppliesTo(option.id, Command::Lex) && optionAppliesTo(option.id, Command::Ir)) {
      continue; // in both, so it proves nothing about globals
    }
    EXPECT_FALSE(isGlobalOption(option.id)) << option.name;
  }
}

TEST(CommandSpecTest, TheTableIsWellFormed) {
  for (const std::string_view name : allOptionNames()) {
    EXPECT_FALSE(name.empty());
    EXPECT_EQ(name.front(), '-');
    const OptionSpec* option = findOptionByName(name);
    ASSERT_NE(option, nullptr) << name;
    EXPECT_FALSE(option->help.empty()) << name;
    if (option->takesValue()) {
      EXPECT_FALSE(option->valueName.empty()) << name;
    } else {
      EXPECT_TRUE(option->valueName.empty()) << name;
    }
  }

  // Two options that share a spelling would make the lookup a coin toss.
  for (std::size_t first = 0; first < allOptionNames().size(); ++first) {
    for (std::size_t second = first + 1; second < allOptionNames().size(); ++second) {
      EXPECT_NE(allOptionNames()[first], allOptionNames()[second]);
    }
  }
}

TEST(CommandSpecTest, NoGroupListsAnOptionTwice) {
  for (const CommandSpec& spec : allCommands()) {
    std::vector<OptionId> seen;
    for (const OptionGroup& group : spec.groups) {
      for (const OptionId id : group.ids) {
        for (const OptionId other : seen) {
          EXPECT_NE(other, id) << spec.name << " / " << optionById(id).name;
        }
        seen.push_back(id);
      }
    }
  }
}

TEST(CommandSpecTest, EveryCommandIsReachableFromItsOwnPage) {
  for (const CommandSpec& spec : allCommands()) {
    ASSERT_FALSE(spec.usage.empty()) << spec.name;
    EXPECT_EQ(spec.usage.front().rfind("mincc ", 0), 0u) << spec.name;
    EXPECT_EQ(spec.usage.front().find(spec.name), 6u) << spec.name;
    EXPECT_EQ(spec.brief.rfind(spec.name, 0), 0u) << spec.name;
    EXPECT_FALSE(spec.summary.empty()) << spec.name;
    EXPECT_FALSE(spec.description.empty()) << spec.name;
    EXPECT_FALSE(spec.examples.empty()) << spec.name;
  }
}

TEST(CommandSpecTest, EveryNamedCommandInSeeAlsoExists) {
  for (const CommandSpec& spec : allCommands()) {
    for (const std::string_view other : spec.seeAlso) {
      EXPECT_TRUE(commandFromName(other).has_value()) << spec.name << " -> " << other;
    }
  }
}

TEST(CommandSpecTest, TheProgramPageIsWellFormed) {
  const ProgramSpec& program = programSpec();
  EXPECT_FALSE(program.name.empty());
  EXPECT_FALSE(program.tagline.empty());
  EXPECT_FALSE(program.facts.empty());
  EXPECT_FALSE(program.usage.empty());
  EXPECT_FALSE(program.groups.empty());
  EXPECT_FALSE(program.environment.empty());
  EXPECT_FALSE(program.exitStatus.empty());
  for (const Fact& fact : program.facts) {
    EXPECT_FALSE(fact.label.empty());
    EXPECT_FALSE(fact.text.empty());
  }
}

} // namespace
} // namespace minc::driver
