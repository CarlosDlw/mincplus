// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "driver/cli.h"
#include "driver/exit_code.h"
#include "driver/help_text.h"
#include "driver/version.h"

namespace minc::driver {
namespace {

CliOptions parse(std::vector<const char*> argv) {
  argv.insert(argv.begin(), "mincc");
  return parseArgs(static_cast<int>(argv.size()), argv.data());
}

TEST(CliTest, DefaultsEmpty) {
  const CliOptions opts = parse({});
  EXPECT_FALSE(opts.showHelp);
  EXPECT_FALSE(opts.showVersion);
  EXPECT_FALSE(opts.command.has_value());
  EXPECT_TRUE(opts.inputs.empty());
  EXPECT_TRUE(opts.error.empty());
}

TEST(CliTest, HelpAndVersionFlags) {
  EXPECT_TRUE(parse({"-h"}).showHelp);
  EXPECT_TRUE(parse({"--help"}).showHelp);
  EXPECT_TRUE(parse({"-V"}).showVersion);
  EXPECT_TRUE(parse({"--version"}).showVersion);
}

TEST(CliTest, CommandTakesFollowingInputs) {
  const CliOptions opts = parse({"build", "foo.mx", "bar.mx"});
  ASSERT_TRUE(opts.command.has_value());
  EXPECT_EQ(*opts.command, Command::Build);
  ASSERT_EQ(opts.inputs.size(), 2u);
  EXPECT_EQ(opts.inputs[0], "foo.mx");
  EXPECT_EQ(opts.inputs[1], "bar.mx");
  EXPECT_TRUE(opts.error.empty());
}

TEST(CliTest, EveryCommandNameParses) {
  for (const CommandInfo& info : allCommands()) {
    const CliOptions opts = parse({std::string(info.name).c_str()});
    ASSERT_TRUE(opts.command.has_value()) << info.name;
    EXPECT_EQ(*opts.command, info.command);
    EXPECT_STREQ(toString(info.command), std::string(info.name).c_str());
  }
}

TEST(CliTest, UnknownCommandIsAnError) {
  const CliOptions opts = parse({"bulid"});
  EXPECT_FALSE(opts.command.has_value());
  EXPECT_NE(opts.error.find("unknown command 'bulid'"), std::string::npos);
}

TEST(CliTest, UnknownOptionIsAnError) {
  const CliOptions opts = parse({"--nope"});
  EXPECT_NE(opts.error.find("unrecognized option '--nope'"), std::string::npos);
}

// `-isystem` keeps its own list rather than joining `-I`: the two have different
// search order and the files found in the second are system headers, so merging
// them would lose the distinction the flag exists to make.
TEST(CliTest, IsystemTakesItsValueJoinedOrSeparate) {
  const CliOptions joined = parse({"pp", "-isystem/sys", "a.mx"});
  ASSERT_TRUE(joined.error.empty());
  ASSERT_EQ(joined.systemDirs.size(), 1u);
  EXPECT_EQ(joined.systemDirs[0], "/sys");
  EXPECT_TRUE(joined.includeDirs.empty());

  const CliOptions separate = parse({"pp", "-isystem", "/sys", "a.mx"});
  ASSERT_TRUE(separate.error.empty());
  ASSERT_EQ(separate.systemDirs.size(), 1u);
  EXPECT_EQ(separate.systemDirs[0], "/sys");
}

TEST(CliTest, IsystemWithoutAValueIsAnError) {
  const CliOptions opts = parse({"pp", "-isystem"});
  EXPECT_NE(opts.error.find("-isystem"), std::string::npos);
}

TEST(CliTest, IncludeAndSystemListsKeepTheirOwnOrder) {
  const CliOptions opts = parse({"pp", "-I", "one", "-isystem", "two", "-I", "three", "a.mx"});
  ASSERT_TRUE(opts.error.empty());
  ASSERT_EQ(opts.includeDirs.size(), 2u);
  EXPECT_EQ(opts.includeDirs[0], "one");
  EXPECT_EQ(opts.includeDirs[1], "three");
  ASSERT_EQ(opts.systemDirs.size(), 1u);
  EXPECT_EQ(opts.systemDirs[0], "two");
}

TEST(CliTest, LoneDashIsAFileNotAnOption) {
  const CliOptions opts = parse({"build", "-"});
  ASSERT_TRUE(opts.command.has_value());
  ASSERT_EQ(opts.inputs.size(), 1u);
  EXPECT_EQ(opts.inputs[0], "-");
}

TEST(CliTest, DoubleDashEndsOptionParsing) {
  const CliOptions opts = parse({"build", "--", "--still-a-file.mx"});
  ASSERT_TRUE(opts.command.has_value());
  ASSERT_EQ(opts.inputs.size(), 1u);
  EXPECT_EQ(opts.inputs[0], "--still-a-file.mx");
  EXPECT_TRUE(opts.error.empty());
}

TEST(CliTest, HelpWinsOverCommand) {
  const CliOptions opts = parse({"build", "--help", "foo.mx"});
  EXPECT_TRUE(opts.showHelp);
  ASSERT_TRUE(opts.command.has_value());
}

TEST(CliTest, MissingCommandIsNotAParserError) {
  const CliOptions opts = parse({});
  EXPECT_TRUE(opts.error.empty());
  EXPECT_FALSE(opts.command.has_value());
}

TEST(CliTest, NullArgumentIsSkipped) {
  const CliOptions opts = parse({"build", nullptr, "foo.mx"});
  ASSERT_TRUE(opts.command.has_value());
  ASSERT_EQ(opts.inputs.size(), 1u);
  EXPECT_EQ(opts.inputs[0], "foo.mx");
}

TEST(CliExitCodeTest, ValuesAreStable) {
  EXPECT_EQ(exitCode(ExitCode::Ok), 0);
  EXPECT_EQ(exitCode(ExitCode::Failure), 1);
  EXPECT_EQ(exitCode(ExitCode::Usage), 2);
}

TEST(HelpTextTest, ListsEveryCommand) {
  const std::string help = helpText();
  for (const CommandInfo& info : allCommands()) {
    EXPECT_NE(help.find(std::string(info.name)), std::string::npos) << info.name;
    EXPECT_NE(help.find(std::string(info.summary)), std::string::npos) << info.name;
  }
  EXPECT_NE(help.find(usageLine()), std::string::npos);
}

TEST(HelpTextTest, StatesHostAndInteropPlatforms) {
  const std::string help = helpText();
  for (const char* host : {"Linux", "macOS", "Windows"}) {
    EXPECT_NE(help.find(host), std::string::npos) << host;
  }
  for (const char* toolchain : {"Clang", "GCC", "MSVC"}) {
    EXPECT_NE(help.find(toolchain), std::string::npos) << toolchain;
  }
  EXPECT_NE(help.find("System V AMD64"), std::string::npos);
}

TEST(HelpTextTest, IsAsciiOnly) {
  // Windows consoles render non-ASCII poorly; keep this output plain.
  for (const char c : helpText()) {
    const auto byte = static_cast<unsigned char>(c);
    EXPECT_TRUE(c == '\n' || (byte >= 0x20u && byte <= 0x7Eu))
        << "non-printable byte 0x" << std::hex << static_cast<int>(byte);
  }
}

TEST(HelpTextTest, VersionLineNamesProgramAndVersion) {
  const std::string line = versionLine();
  EXPECT_NE(line.find(kProgName), std::string::npos);
  EXPECT_NE(line.find(kVersion), std::string::npos);
}

TEST(CliTest, LexTakesFilesAsInputs) {
  const CliOptions opts = parse({"lex", "a.mx", "b.mx"});
  ASSERT_TRUE(opts.command.has_value());
  EXPECT_EQ(*opts.command, Command::Lex);
  ASSERT_EQ(opts.inputs.size(), 2u);
  EXPECT_EQ(opts.inputs[0], "a.mx");
  EXPECT_EQ(opts.inputs[1], "b.mx");
  EXPECT_TRUE(opts.error.empty());
}

TEST(CliTest, LexAcceptsStandardInputAsADash) {
  const CliOptions opts = parse({"lex", "-"});
  ASSERT_TRUE(opts.command.has_value());
  ASSERT_EQ(opts.inputs.size(), 1u);
  EXPECT_EQ(opts.inputs[0], "-");
}

TEST(CliTest, LexTakesNoOptions) {
  // A flag the driver does not know is a usage error, not silently ignored.
  const CliOptions opts = parse({"lex", "--tokens-only"});
  EXPECT_NE(opts.error.find("unrecognized option '--tokens-only'"), std::string::npos);
}

// Reads the comma-separated command names listed after `label`.
std::vector<std::string> namesAfter(const std::string& help, const std::string& label) {
  std::vector<std::string> names;
  const std::size_t at = help.find(label);
  if (at == std::string::npos) {
    return names;
  }
  std::size_t i = at + label.size();
  std::string current;
  for (; i < help.size() && help[i] != '\n'; ++i) {
    if (help[i] == ',') {
      names.push_back(current);
      current.clear();
      if (i + 1 < help.size() && help[i + 1] == ' ') {
        ++i; // skip the space after the comma
      }
      continue;
    }
    current.push_back(help[i]);
  }
  names.push_back(current);
  return names;
}

// Help derives the two lists from the command table, so a command cannot be
// advertised as working while the dispatch still refuses it.
TEST(HelpTextTest, ImplementedAndScaffoldedListsAreExact) {
  const std::string help = helpText();
  std::vector<std::string> implemented;
  std::vector<std::string> scaffolded;
  for (const CommandInfo& info : allCommands()) {
    (info.implemented ? implemented : scaffolded).emplace_back(info.name);
  }

  EXPECT_EQ(namesAfter(help, "Implemented: "), implemented);
  EXPECT_EQ(namesAfter(help, "Scaffolded:  "), scaffolded);
  // Not one name is written in the text: the list starts with the first
  // implemented command in the table's order, whatever that turns out to be.
  ASSERT_FALSE(implemented.empty());
  EXPECT_NE(help.find("Implemented: " + implemented.front()), std::string::npos);
  EXPECT_FALSE(scaffolded.empty());
}

TEST(CliTest, TheTargetOptionIsCarriedAndDefaultsToSystemV) {
  {
    const char* argv[] = {"mincc", "check", "a.mx"};
    const CliOptions opts = parseArgs(3, argv);
    EXPECT_EQ(opts.error, "");
    EXPECT_EQ(opts.target, "systemv-amd64");
  }
  {
    const char* argv[] = {"mincc", "check", "--target", "windows-x64", "a.mx"};
    const CliOptions opts = parseArgs(5, argv);
    EXPECT_EQ(opts.error, "");
    EXPECT_EQ(opts.target, "windows-x64");
  }
  {
    // `--target=name` is the same thing, because half the world writes it that
    // way and a CLI that accepts only one spelling is a paper cut.
    const char* argv[] = {"mincc", "check", "--target=windows-x64", "a.mx"};
    const CliOptions opts = parseArgs(4, argv);
    EXPECT_EQ(opts.error, "");
    EXPECT_EQ(opts.target, "windows-x64");
  }
  {
    const char* argv[] = {"mincc", "check", "--target", "nonsense", "a.mx"};
    const CliOptions opts = parseArgs(5, argv);
    // The *name* is validated by the command, which knows the table; the parser
    // only guarantees it got a value.
    EXPECT_EQ(opts.error, "");
    EXPECT_EQ(opts.target, "nonsense");
  }
  {
    const char* argv[] = {"mincc", "check", "--target"};
    const CliOptions opts = parseArgs(3, argv);
    EXPECT_FALSE(opts.error.empty());
  }
}

TEST(CliTest, TheConversionWarningIsOptInLikeTheOthers) {
  {
    const char* argv[] = {"mincc", "check", "a.mx"};
    const CliOptions opts = parseArgs(3, argv);
    EXPECT_FALSE(opts.warnConversion);
  }
  {
    const char* argv[] = {"mincc", "check", "-Wconversion", "a.mx"};
    const CliOptions opts = parseArgs(4, argv);
    EXPECT_EQ(opts.error, "");
    EXPECT_TRUE(opts.warnConversion);
  }
  {
    const char* argv[] = {"mincc", "check", "--types", "a.mx"};
    const CliOptions opts = parseArgs(4, argv);
    EXPECT_EQ(opts.error, "");
    EXPECT_TRUE(opts.showTypes);
    EXPECT_FALSE(opts.stats);
  }
  {
    // `--stats` is opt-in: `check` prints nothing on success without it.
    const char* argv[] = {"mincc", "check", "a.mx"};
    const CliOptions opts = parseArgs(3, argv);
    EXPECT_FALSE(opts.stats);
  }
  {
    const char* argv[] = {"mincc", "check", "--stats", "a.mx"};
    const CliOptions opts = parseArgs(4, argv);
    EXPECT_EQ(opts.error, "");
    EXPECT_TRUE(opts.stats);
  }
}

TEST(CliTest, LexIsMarkedImplementedInTheTable) {
  const std::optional<Command> command = commandFromName("lex");
  ASSERT_TRUE(command.has_value());
  for (const CommandInfo& info : allCommands()) {
    if (info.command == *command) {
      EXPECT_TRUE(info.implemented);
    }
  }
}

} // namespace
} // namespace minc::driver
