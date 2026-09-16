// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "driver/cli.h"
#include "driver/command_spec.h"
#include "driver/exit_code.h"
#include "driver/help_text.h"
#include "driver/version.h"
// For `kDefaultTriple`: the parser fills `--target`'s default from that table,
// and this test is what keeps the two from drifting.
#include "sema/target.h"

namespace minc::driver {
namespace {

CliOptions parse(std::vector<const char*> argv) {
  argv.insert(argv.begin(), "mincc");
  return parseArgs(static_cast<int>(argv.size()), argv.data());
}

// --- the shape of a command line --------------------------------------------

TEST(CliTest, DefaultsEmpty) {
  const CliOptions opts = parse({});
  EXPECT_FALSE(opts.showHelp);
  EXPECT_FALSE(opts.showVersion);
  EXPECT_FALSE(opts.command.has_value());
  EXPECT_TRUE(opts.inputs.empty());
  EXPECT_TRUE(opts.error.empty());
}

TEST(CliTest, AnEmptyCommandLineIsRecordedAsSuch) {
  // The text printed for `mincc` and for `mincc --help` is the same; the stream
  // and the exit code are not, and this is the flag that tells them apart.
  EXPECT_TRUE(parse({}).emptyCommandLine);
  EXPECT_FALSE(parse({"--help"}).emptyCommandLine);
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
  for (const CommandSpec& spec : allCommands()) {
    const CliOptions opts = parse({std::string(spec.name).c_str()});
    ASSERT_TRUE(opts.command.has_value()) << spec.name;
    EXPECT_EQ(*opts.command, spec.command);
    EXPECT_STREQ(toString(spec.command), std::string(spec.name).c_str());
  }
}

TEST(CliTest, UnknownCommandIsAnError) {
  const CliOptions opts = parse({"bulid"});
  EXPECT_FALSE(opts.command.has_value());
  EXPECT_NE(opts.error.find("unknown command 'bulid'"), std::string::npos);
  // The spelling is one edit from a command that exists, so it is offered.
  EXPECT_EQ(opts.suggestion, "build");
}

TEST(CliTest, AFileWhereACommandBelongsSuggestsTheCommand) {
  // `mincc main.mx` is the mistake this catches, and the useful answer is a whole
  // command line rather than a command name.
  const CliOptions opts = parse({"main.mx"});
  EXPECT_NE(opts.error.find("unknown command 'main.mx'"), std::string::npos);
  EXPECT_NE(opts.suggestion.find("run"), std::string::npos);
  EXPECT_NE(opts.suggestion.find("main.mx"), std::string::npos);
}

TEST(CliTest, UnknownOptionIsAnErrorAndSuggestsTheNearest) {
  const CliOptions opts = parse({"build", "--targt", "x", "a.mx"});
  EXPECT_NE(opts.error.find("unrecognized option '--targt'"), std::string::npos);
  EXPECT_EQ(opts.suggestion, "--target");
}

TEST(CliTest, UnknownOptionWithoutANearNeighbourSuggestsNothing) {
  const CliOptions opts = parse({"build", "--completely-unrelated", "a.mx"});
  EXPECT_TRUE(opts.suggestion.empty());
}

TEST(CliTest, HelpWinsOverEverythingAfterIt) {
  // The rule from the CLI guidelines: `-h` works at the end of a broken line.
  for (const std::vector<const char*>& argv : std::vector<std::vector<const char*>>{
           {"build", "--nosuch", "-h"}, {"--nosuch", "--help"}, {"lex", "--emit", "obj", "-h"}}) {
    const CliOptions opts = parse(argv);
    EXPECT_TRUE(opts.error.empty()) << argv[0];
    EXPECT_TRUE(opts.showHelp);
  }
}

TEST(CliTest, HelpWinsOverCommand) {
  const CliOptions opts = parse({"build", "--help", "foo.mx"});
  EXPECT_TRUE(opts.showHelp);
  ASSERT_TRUE(opts.command.has_value());
  EXPECT_EQ(*opts.command, Command::Build);
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

// --- the `help` command ------------------------------------------------------

TEST(CliTest, HelpCommandTakesAnOptionalTopic) {
  const CliOptions bare = parse({"help"});
  EXPECT_TRUE(bare.showHelp);
  EXPECT_TRUE(bare.error.empty());
  EXPECT_FALSE(bare.helpTopic.has_value());
  EXPECT_FALSE(bare.command.has_value());

  const CliOptions topic = parse({"help", "build"});
  EXPECT_TRUE(topic.showHelp);
  EXPECT_TRUE(topic.error.empty());
  ASSERT_TRUE(topic.helpTopic.has_value());
  EXPECT_EQ(*topic.helpTopic, Command::Build);
}

TEST(CliTest, HelpCommandWithAnUnknownTopicIsAnError) {
  const CliOptions opts = parse({"help", "buidl"});
  EXPECT_NE(opts.error.find("unknown command 'buidl'"), std::string::npos);
  EXPECT_EQ(opts.suggestion, "build");
}

TEST(CliTest, HelpCommandRefusesASecondName) {
  const CliOptions opts = parse({"help", "build", "check"});
  EXPECT_NE(opts.error.find("one command name"), std::string::npos);
}

// --- options and the command that owns them ---------------------------------

TEST(CliTest, AnOptionOfAnotherCommandIsRefusedRatherThanIgnored) {
  // `mincc lex --emit obj` used to be accepted and silently dropped.
  const CliOptions opts = parse({"lex", "--emit", "obj", "a.mx"});
  EXPECT_NE(opts.error.find("'--emit' is not an option of 'lex'"), std::string::npos);
  EXPECT_NE(opts.error.find("build"), std::string::npos);
}

TEST(CliTest, ACommandOptionBeforeTheCommandIsAccepted) {
  // The option set is checked once the command is known, so the order on the
  // line is free -- which is what makes `mincc -g build a.mx` work.
  const CliOptions opts = parse({"-g", "build", "a.mx"});
  EXPECT_TRUE(opts.error.empty());
  EXPECT_TRUE(opts.debugInfo);
  ASSERT_TRUE(opts.command.has_value());
  EXPECT_EQ(*opts.command, Command::Build);
}

TEST(CliTest, ACommandOptionWithNoCommandNamesTheCommandsThatHaveIt) {
  const CliOptions opts = parse({"--emit", "obj"});
  EXPECT_NE(opts.error.find("name the command first"), std::string::npos);
  EXPECT_NE(opts.error.find("build"), std::string::npos);
}

TEST(CliTest, LexTakesNoOptions) {
  const CliOptions opts = parse({"lex", "--tokens-only"});
  EXPECT_NE(opts.error.find("unrecognized option '--tokens-only'"), std::string::npos);
}

TEST(CliTest, TheWarningAFrontEndCommandDoesNotTakeIsRefused) {
  // `resolve` lints names and does not type-check, so `-Wconversion` is not one
  // of its options; `check` is where it means something.
  const CliOptions resolve = parse({"resolve", "-Wconversion", "a.mx"});
  EXPECT_NE(resolve.error.find("-Wconversion"), std::string::npos);
  const CliOptions check = parse({"check", "-Wconversion", "a.mx"});
  EXPECT_TRUE(check.error.empty());
  EXPECT_TRUE(check.warnConversion);
}

TEST(CliTest, UnknownWarningOptionIsItsOwnMessage) {
  const CliOptions opts = parse({"check", "-Wshadoww", "a.mx"});
  EXPECT_NE(opts.error.find("unknown warning option '-Wshadoww'"), std::string::npos);
  EXPECT_EQ(opts.suggestion, "-Wshadow");
}

// --- values -----------------------------------------------------------------

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

TEST(CliTest, TheOptimisationLevelIsOptionalAndNeverEatsTheNextArgument) {
  // `-O` is the one option whose value is optional, and the distinction matters:
  // a `-O` that took the next argument would compile `a.mx` at level "a.mx".
  const CliOptions bare = parse({"build", "-O", "a.mx"});
  ASSERT_TRUE(bare.error.empty());
  EXPECT_EQ(bare.optLevel, "1");
  ASSERT_EQ(bare.inputs.size(), 1u);
  EXPECT_EQ(bare.inputs[0], "a.mx");

  EXPECT_EQ(parse({"build", "-O2", "a.mx"}).optLevel, "2");
  EXPECT_EQ(parse({"build", "-Oz", "a.mx"}).optLevel, "z");
}

TEST(CliTest, TheCheckedBuildIsThreeStatesAndNotTwo) {
  // "Not said" is a different answer from "said no": the default is a *level* --
  // the checked build is what `-O0` means -- so the parsed options have to keep
  // the difference, and `requestFrom` is what resolves it (`checks.md`).
  const CliOptions silent = parse({"build", "a.mx"});
  ASSERT_TRUE(silent.error.empty());
  EXPECT_FALSE(silent.checkBuild.has_value());

  const CliOptions on = parse({"build", "-fcheck", "a.mx"});
  ASSERT_TRUE(on.error.empty());
  ASSERT_TRUE(on.checkBuild.has_value());
  EXPECT_TRUE(*on.checkBuild);

  const CliOptions off = parse({"build", "-fno-check", "a.mx"});
  ASSERT_TRUE(off.error.empty());
  ASSERT_TRUE(off.checkBuild.has_value());
  EXPECT_FALSE(*off.checkBuild);
}

TEST(CliTest, JoinedAndSeparateValuesAgree) {
  const CliOptions joined = parse({"build", "-DFOO=1", "-Iinc", "-oout", "a.mx"});
  ASSERT_TRUE(joined.error.empty());
  ASSERT_EQ(joined.defines.size(), 1u);
  EXPECT_EQ(joined.defines[0], "FOO=1");
  EXPECT_EQ(joined.includeDirs[0], "inc");
  EXPECT_EQ(joined.output, "out");

  const CliOptions separate = parse({"build", "-D", "FOO=1", "-I", "inc", "-o", "out", "a.mx"});
  ASSERT_TRUE(separate.error.empty());
  EXPECT_EQ(separate.defines[0], "FOO=1");
  EXPECT_EQ(separate.includeDirs[0], "inc");
  EXPECT_EQ(separate.output, "out");
}

TEST(CliTest, ALongOptionRefusesAValueItDoesNotTake) {
  const CliOptions opts = parse({"build", "--help=yes", "a.mx"});
  EXPECT_NE(opts.error.find("does not take a value"), std::string::npos);
}

// --- clusters ---------------------------------------------------------------

TEST(CliTest, TheVersionClusterSetsBothFlags) {
  for (const char* spelling : {"-vV", "-Vv", "-vvV"}) {
    const CliOptions opts = parse({spelling});
    EXPECT_TRUE(opts.showVersion) << spelling;
    EXPECT_TRUE(opts.verbose) << spelling;
    EXPECT_TRUE(opts.error.empty()) << spelling;
  }
}

TEST(CliTest, TheVersionFlagBesideVerboseAlsoGivesTheBlock) {
  // `-V -v` is not a cluster, but `-V` is a flag that outranks the command
  // rules, so the `-v` beside it is carried instead of being refused for
  // belonging to `build` and `run`.
  const CliOptions opts = parse({"-V", "-v"});
  EXPECT_TRUE(opts.showVersion);
  EXPECT_TRUE(opts.verbose);
  EXPECT_TRUE(opts.error.empty());
}

TEST(CliTest, AClusterOfValueLessFlagsIsAccepted) {
  const CliOptions opts = parse({"build", "-vg", "a.mx"});
  ASSERT_TRUE(opts.error.empty());
  EXPECT_TRUE(opts.verbose);
  EXPECT_TRUE(opts.debugInfo);
}

TEST(CliTest, AClusterThatWouldNeedAValueIsRefused) {
  // `-vo out`: which letter takes the value is a guess, and a CLI that guesses
  // is inventing grammar.
  const CliOptions opts = parse({"build", "-vo", "out", "a.mx"});
  EXPECT_FALSE(opts.error.empty());
}

// --- colour -----------------------------------------------------------------

TEST(CliTest, ColourDefaultsToAutoAndIsCarried) {
  EXPECT_EQ(parse({"check", "a.mx"}).colorChoice, support::ColorChoice::Auto);

  const CliOptions never = parse({"--color=never", "check", "a.mx"});
  ASSERT_TRUE(never.error.empty());
  EXPECT_EQ(never.colorChoice, support::ColorChoice::Never);

  const CliOptions always = parse({"check", "--color", "always", "a.mx"});
  ASSERT_TRUE(always.error.empty());
  EXPECT_EQ(always.colorChoice, support::ColorChoice::Always);
}

TEST(CliTest, AnInvalidColourNamesTheValuesItTakes) {
  const CliOptions opts = parse({"--color=maybe", "check", "a.mx"});
  EXPECT_NE(opts.error.find("invalid value 'maybe'"), std::string::npos);
  EXPECT_NE(opts.error.find("auto, always, never"), std::string::npos);
}

// --- the error limit --------------------------------------------------------

TEST(CliTest, TheErrorLimitIsAcceptedJoinedSeparateAndAbbreviated) {
  const CliOptions joined = parse({"check", "-ferror-limit=20", "a.mx"});
  ASSERT_TRUE(joined.error.empty()) << joined.error;
  EXPECT_EQ(joined.errorLimit, 20u);

  const CliOptions separate = parse({"check", "-ferror-limit", "20", "a.mx"});
  ASSERT_TRUE(separate.error.empty()) << separate.error;
  EXPECT_EQ(separate.errorLimit, 20u);
  // The file is still a file: the value was taken and nothing else was eaten.
  ASSERT_EQ(separate.inputs.size(), 1u);
  EXPECT_EQ(separate.inputs[0], "a.mx");
}

TEST(CliTest, ZeroMeansEverythingAndAHugeValueIsClampedToWhatCanBeKept) {
  // `0` is clang's spelling of "no limit", and it is stored as the retention cap
  // because that is the most the bag can hold -- one number for the renderer to
  // compare against. A value above the cap is the same thing without a
  // diagnostic: "show at most 100000" is satisfied by showing all that exist.
  EXPECT_EQ(parse({"check", "-ferror-limit=0", "a.mx"}).errorLimit, support::kMaxDiagnostics);
  EXPECT_EQ(parse({"check", "-ferror-limit=100000", "a.mx"}).errorLimit, support::kMaxDiagnostics);
  EXPECT_EQ(parse({"check", "-ferror-limit=1024", "a.mx"}).errorLimit, 1024u);
  // The default is the cap, spelled from the table so the help cannot name a
  // different number from the one the compiler enforces.
  EXPECT_EQ(parse({"check", "a.mx"}).errorLimit, support::kMaxDiagnostics);
}

TEST(CliTest, ABadErrorLimitIsRefusedRatherThanGuessed) {
  // `1x` is not `1`: reading the digits until something else appears is how a
  // command line starts meaning more than it says.
  for (const char* spelling : {"-ferror-limit=x", "-ferror-limit=1x", "-ferror-limit=-1"}) {
    const CliOptions opts = parse({"check", spelling, "a.mx"});
    EXPECT_NE(opts.error.find("non-negative integer"), std::string::npos) << spelling;
    EXPECT_NE(opts.error.find("-ferror-limit"), std::string::npos) << spelling;
  }
  // An empty value is a *missing* value and not an invalid one, which is the
  // distinction the long-option branch already makes (`--color=`).
  const CliOptions empty = parse({"check", "-ferror-limit=", "a.mx"});
  EXPECT_NE(empty.error.find("needs a value"), std::string::npos) << empty.error;
}

TEST(CliTest, AParseErrorKeepsTheLastErrorLimitOnTheLine) {
  const CliOptions opts = parse({"build", "-ferror-limit=5", "-ferror-limit=9", "a.mx"});
  ASSERT_TRUE(opts.error.empty()) << opts.error;
  EXPECT_EQ(opts.errorLimit, 9u); // last one wins, like every other option here
}

// --- positionals, `--`, and the program's own arguments ---------------------

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

TEST(CliTest, ADoubleDashInARunSendsEverythingAfterItToTheProgram) {
  const CliOptions opts = parse({"run", "p.mx", "--", "-o", "--emit", "x"});
  ASSERT_TRUE(opts.error.empty());
  ASSERT_EQ(opts.inputs.size(), 1u);
  EXPECT_EQ(opts.inputs[0], "p.mx");
  ASSERT_EQ(opts.programArgs.size(), 3u);
  EXPECT_EQ(opts.programArgs[0], "-o");
  EXPECT_EQ(opts.programArgs[1], "--emit");
  EXPECT_EQ(opts.programArgs[2], "x");
}

TEST(CliTest, AProgramArgumentThatLooksLikeHelpIsNotHelp) {
  // `mincc run p.mx -- -h` runs a program that is passed `-h`; the pre-scan stops
  // at the separator for exactly this reason.
  const CliOptions opts = parse({"run", "p.mx", "--", "-h"});
  EXPECT_FALSE(opts.showHelp);
  ASSERT_EQ(opts.programArgs.size(), 1u);
  EXPECT_EQ(opts.programArgs[0], "-h");
}

// --- the target --------------------------------------------------------------

TEST(CliTest, TheTargetOptionIsCarriedAndDefaultsToTheDefaultTriple) {
  const CliOptions defaults = parse({"check", "a.mx"});
  EXPECT_EQ(defaults.target, std::string(sema::kDefaultTriple));

  const CliOptions separate = parse({"check", "--target", "x86_64-pc-windows-msvc", "a.mx"});
  ASSERT_TRUE(separate.error.empty());
  EXPECT_EQ(separate.target, "x86_64-pc-windows-msvc");

  const CliOptions joined = parse({"check", "--target=x86_64-pc-windows-msvc", "a.mx"});
  ASSERT_TRUE(joined.error.empty());
  EXPECT_EQ(joined.target, "x86_64-pc-windows-msvc");

  // The *name* is validated by the command, which knows the table; the parser
  // only guarantees it got a value.
  const CliOptions nonsense = parse({"check", "--target", "nonsense", "a.mx"});
  ASSERT_TRUE(nonsense.error.empty());
  EXPECT_EQ(nonsense.target, "nonsense");

  const CliOptions missing = parse({"check", "--target"});
  EXPECT_FALSE(missing.error.empty());
}

// --- help text ---------------------------------------------------------------

TEST(HelpTextTest, TheOverviewListsEveryCommand) {
  const std::string help = helpText();
  for (const CommandSpec& spec : allCommands()) {
    EXPECT_NE(help.find(spec.brief), std::string::npos) << spec.name;
    EXPECT_NE(help.find(spec.summary), std::string::npos) << spec.name;
  }
}

TEST(HelpTextTest, EveryCommandPageMentionsItsOwnOptions) {
  for (const CommandSpec& spec : allCommands()) {
    const std::string page = helpText(spec.command);
    EXPECT_NE(page.find(spec.usage.front()), std::string::npos) << spec.name;
    for (const OptionGroup& group : spec.groups) {
      for (const OptionId id : group.ids) {
        EXPECT_NE(page.find(optionById(id).name), std::string::npos)
            << spec.name << " / " << optionById(id).name;
      }
    }
  }
}

TEST(HelpTextTest, TheOverviewNamesTheTwoUsefulNextSteps) {
  const std::string help = helpText();
  EXPECT_NE(help.find("mincc help <command>"), std::string::npos);
  EXPECT_NE(help.find("Usage:"), std::string::npos);
}

TEST(HelpTextTest, IsAsciiOnly) {
  // A Windows console with a legacy code page renders UTF-8 unpredictably, and a
  // help page is the wrong place to find out.
  for (const std::string& text :
       {helpText(), helpText(Command::Build), versionLine(), usageLine(), versionBlock()}) {
    for (const char c : text) {
      const auto byte = static_cast<unsigned char>(c);
      EXPECT_TRUE(c == '\n' || c == '\t' || (byte >= 0x20U && byte <= 0x7EU))
          << "non-ASCII byte " << static_cast<int>(byte);
    }
  }
}

TEST(HelpTextTest, VersionLineNamesProgramAndVersion) {
  const std::string line = versionLine();
  EXPECT_NE(line.find(kProgName), std::string::npos);
  EXPECT_NE(line.find(kVersion), std::string::npos);
}

TEST(HelpTextTest, TheVersionBlockIsTheOneABugReportNeeds) {
  const std::string block = versionBlock();
  for (const char* key : {"binary:", "host:", "default target:", "LLVM:"}) {
    EXPECT_NE(block.find(key), std::string::npos) << key;
  }
  // The host is LLVM's answer and it is never empty; a build that could not ask
  // would print an empty value, which is the failure this pins.
  EXPECT_NE(block.find(std::string(sema::kDefaultTriple)), std::string::npos);
}

TEST(CliExitCodeTest, ValuesAreStable) {
  EXPECT_EQ(exitCode(ExitCode::Ok), 0);
  EXPECT_EQ(exitCode(ExitCode::Failure), 1);
  EXPECT_EQ(exitCode(ExitCode::Usage), 2);
}

} // namespace
} // namespace minc::driver
