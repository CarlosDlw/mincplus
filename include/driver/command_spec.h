// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The command line as data: one statement of every option, read by two.
//
// `cli.cc` reads this to decide what it is looking at, and `help_render.cc`
// reads it to print a page. Neither keeps a list of its own, which is the point:
// the failure this file exists to prevent is a hand-written help text beside a
// hand-written parser, drifting apart one option at a time. `README.md` and
// [`docs/architectures/cli.md`](../../docs/architectures/cli.md) describe the
// surface this table is the source of.
//
// What is data here is the *surface*: names, short letters, whether a value is
// taken, the one-line description, the default, the permitted values as
// documentation, the group an option is printed under, examples, environment
// variables, and what to read next. What is code, in `cli.cc`, is the
// *behaviour*: `-D` becomes a `(name, body)` pair, `-o` is last-wins, a `--`
// stops option parsing. Splitting it any other way would mean inventing a small
// argument-parsing language to express four special cases.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace minc::driver {

// Subcommands the driver understands. Anything else is a usage error.
enum class Command : std::uint8_t { Build, Run, Check, Lex, Parse, Pp, Resolve, Ir };

// One option's identity, as the parser's `switch` sees it. Adding an option
// means adding an enumerator and a row; a test proves every enumerator has a row
// and every row has a handler, so neither half can be forgotten.
enum class OptionId : std::uint8_t {
  // Global: applies to every command.
  Help,
  Version,
  Color,
  // The front end: the preprocessor's inputs.
  Define,
  Undefine,
  Include,
  Isystem,
  Target,
  // What a command prints.
  NoTrivia,
  Ast,
  Types,
  Stats,
  Refs,
  Unresolved,
  ListDefines,
  ListIncludes,
  ListDeps,
  At,
  // `build` and `run`.
  Output,
  OptLevel,
  DebugInfo,
  Verbose,
  Emit,
  LibraryDir,
  Library,
  Linker,
  Sysroot,
  // Warnings.
  WarnUnused,
  WarnShadow,
  WarnConversion,
};

// Whether an option is followed by a value, and how the value is written.
//
// `Optional` is the `-O` case and only that: the value may be joined (`-O2`) and
// is never taken from the next argument, so `-O` alone means one thing and does
// not swallow the file that follows it. `Required` accepts both spellings,
// joined and separate, because half the world writes `-DFOO=1` and the other
// half writes `-D FOO=1`.
enum class ValueKind : std::uint8_t { None, Required, Optional };

struct OptionSpec {
  OptionId id;
  // The spelling this option is matched by, and the one the help prints as
  // primary: `--emit`, `-o`, `-Wunused`, `-isystem`. A name that begins with a
  // single dash and takes a value also accepts the joined spelling
  // (`-isystemdir`, `-DFOO=1`, `-o/path`).
  std::string_view name;
  // The single letter this option also answers to, or `'\0'`. `-o` has a name
  // and no second letter; `-O` and `-o` are different options that share a
  // prefix, which is why the name is matched exactly before any short-letter
  // rule runs.
  char shortName = '\0';
  ValueKind value = ValueKind::None;
  // What the value is called in the help: `PATH`, `KIND`. Empty for a flag.
  //
  // `valueName`, `help`, `defaultValue` and `values` carry default member
  // initializers rather than being restated on every row: a flag has no value
  // name, no default and no permitted values, and a table of forty rows that
  // spells out the empty ones hides the four that do not have them. It is also
  // what lets the rows be written as designated initializers without `gcc`
  // warning about the members they leave out (`-Wmissing-field-initializers`
  // fires on an omitted member that has no default, and not otherwise).
  std::string_view valueName{};
  // One line, wrapped by the renderer. It is a sentence about what the option
  // does, not about how to spell it -- the spelling is already on the line.
  std::string_view help{};
  // Shown as `[default]` when it is not the empty string.
  std::string_view defaultValue{};
  // The permitted values, **for the help only**. Validation lives with the
  // command that owns the option, because that is where the sentence naming the
  // alternatives can be written in full (`'exe', 'obj' and 'asm'`).
  std::span<const std::string_view> values{};

  [[nodiscard]] bool takesValue() const {
    return value != ValueKind::None;
  }
};

// A group of options as they are printed, and -- because it is the same list --
// the set a command accepts. It holds *ids* rather than rows, so an option is
// described once in `kOptions` and no group can carry a second, stale copy of a
// name or a description.
struct OptionGroup {
  // Empty for the group printed first, without a heading.
  std::string_view title;
  std::span<const OptionId> ids;
};

// A labelled fact, printed as a two-column row that wraps like every other
// wrapped text: a definition (`Hosts:`), an environment variable (`NO_COLOR`).
// A table rather than a pre-aligned string, because a string aligned by hand is
// a line that stops fitting the moment the terminal is narrower than the machine
// it was written on -- measured, not feared.
struct Fact {
  std::string_view label;
  std::string_view text;
};

// One subcommand, as the parser and the help both see it.
struct CommandSpec {
  Command command;
  std::string_view name;
  // One line for the overview. No trailing period: it is a table cell.
  std::string_view summary;
  // The shape, for the overview's one-line-per-command list: `build <files...>`.
  // Shorter than the usage on purpose -- a list is scanned, and the full
  // invocation with its optional parts is on the command's own page, one line
  // away. Keeping them apart is what lets the overview stay readable at 80
  // columns instead of pushing every summary into a second line.
  std::string_view brief;
  // Usage lines, one per way to invoke it, without the program name.
  std::span<const std::string_view> usage;
  // The paragraphs of its page.
  std::string_view description;
  // **The option set of this command, and the only one.** The parser refuses an
  // option that is not here (and not global), so nothing can be accepted while
  // being invisible in the help.
  std::span<const OptionGroup> groups;
  // Lines, printed verbatim under `Examples:`.
  std::span<const std::string_view> examples;
  // Command names printed under `See also:`.
  std::span<const std::string_view> seeAlso;
  // False for a command that is parsed and refused. The overview derives the
  // marker from this rather than naming commands in prose.
  bool implemented;
};

// The program itself: the overview page.
struct ProgramSpec {
  std::string_view name;
  std::string_view tagline;
  // Hosts, targets, and what C interop means here -- the two axes a reader has
  // to be able to tell apart.
  std::span<const Fact> facts;
  std::span<const std::string_view> usage;
  // The options every command accepts, printed on every command's page too.
  std::span<const OptionGroup> groups;
  // The environment variables this program reads, and what each one does.
  std::span<const Fact> environment;
  // The exit-status contract, as one row per code: the code is the label and the
  // meaning the text, so the two columns stay aligned and wrap.
  std::span<const Fact> exitStatus;
};

[[nodiscard]] const ProgramSpec& programSpec();
[[nodiscard]] std::span<const CommandSpec> allCommands();
[[nodiscard]] std::span<const OptionGroup> globalOptionGroups();

[[nodiscard]] const char* toString(Command command);
[[nodiscard]] std::optional<Command> commandFromName(std::string_view name);
[[nodiscard]] const CommandSpec& commandSpec(Command command);

// Whether `command` accepts `id`. True for a global option and for one of the
// command's own; the parser's rejection of `mincc lex --emit obj` is this
// function, and the renderer's table is the same groups read the other way.
[[nodiscard]] bool optionAppliesTo(OptionId id, Command command);

// The row for an id. Every id has exactly one, and no id has two.
[[nodiscard]] const OptionSpec& optionById(OptionId id);

// Every row, in table order. The parser's surface is a *subset* of this per
// command, so a test can walk the whole table and prove which commands refuse
// which rows rather than trusting the groups by eye.
[[nodiscard]] std::span<const OptionSpec> allOptions();

// Lookup by the spelling on the command line. Matches the exact name first
// (`--emit`, `-Wshadow`, `-isystem`) and then a short letter; a joined value is
// the caller's business, because only the caller knows where the value ended.
[[nodiscard]] const OptionSpec* findOptionByName(std::string_view name);
[[nodiscard]] const OptionSpec* findOptionByShortName(char letter);
// The longest `-isystem`-style name that is a prefix of `arg`, or nullptr. Only
// single-dash, value-taking options are candidates: that spelling exists for
// them and for nothing else.
[[nodiscard]] const OptionSpec* findJoinedValueOption(std::string_view arg);

// The commands whose option set contains `id`, in table order. Used to say
// *which* command an option belongs to when one is passed to the wrong one.
[[nodiscard]] std::span<const Command> ownersOf(OptionId id);

// True for an option in the global group: `--help`, `--version`, `--color`. The
// one list, read rather than restated, so "global" cannot come to mean two
// things.
[[nodiscard]] bool isGlobalOption(OptionId id);

// Every spelling that can be typed for `command`, for the spelling suggestion:
// option names, and the command names themselves.
[[nodiscard]] std::span<const std::string_view> optionNamesOf(Command command);
[[nodiscard]] std::span<const std::string_view> allOptionNames();
[[nodiscard]] std::span<const std::string_view> commandNames();

} // namespace minc::driver
