// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The command line, written once. `command_spec.h` says why it is data.
//
// Reading order: the option rows, then the groups (lists of ids, so a name is
// written once), then the commands, then the program. Nothing below repeats a
// name or a description that appears above it.
#include "driver/command_spec.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

#include "sema/target.h"
#include "support/limits.h"

namespace minc::driver {
namespace {

// --- the option rows --------------------------------------------------------
//
// One row per option that can be typed. `values` is documentation, for the help;
// the command that owns an option writes the sentence that rejects a bad value,
// because that is where the alternatives can be named in full.

constexpr std::array<std::string_view, 3> kColorValues{"auto", "always", "never"};
constexpr std::array<std::string_view, 6> kOptLevels{"O0", "O1", "O2", "O3", "Os", "Oz"};
constexpr std::array<std::string_view, 3> kEmitKinds{"exe", "obj", "asm"};

constexpr std::array<OptionSpec, 31> kOptions{{
    // --- global: accepted by every command ---------------------------------
    {.id = OptionId::Help,
     .name = "--help",
     .shortName = 'h',
     .help = "print the overview, or one command's page with `mincc help <command>`"},
    {.id = OptionId::Version,
     .name = "--version",
     .shortName = 'V',
     .help = "print the version; with -v, the block a bug report needs"},
    {.id = OptionId::Color,
     .name = "--color",
     .value = ValueKind::Required,
     .valueName = "WHEN",
     .help = "when to use color. `always` beats NO_COLOR and `never` beats "
             "everything",
     .defaultValue = "auto",
     .values = kColorValues},
    {.id = OptionId::ErrorLimit,
     .name = "-ferror-limit",
     .value = ValueKind::Required,
     .valueName = "N",
     .help = "show at most N errors and stop; the diagnostics after the limit are "
             "counted and not shown, 0 means all of them",
     .defaultValue = support::kMaxDiagnosticsText},

    // --- the front end -----------------------------------------------------
    {.id = OptionId::Define,
     .name = "-D",
     .shortName = 'D',
     .value = ValueKind::Required,
     .valueName = "NAME[=BODY]",
     .help = "define a macro before the file is read; may be given more than once, "
             "and the order is kept"},
    {.id = OptionId::Undefine,
     .name = "-U",
     .shortName = 'U',
     .value = ValueKind::Required,
     .valueName = "NAME",
     .help = "undefine a macro; every -U is applied after every -D"},
    {.id = OptionId::Include,
     .name = "-I",
     .shortName = 'I',
     .value = ValueKind::Required,
     .valueName = "DIR",
     .help = "add a directory to the include search list, in the order written"},
    {.id = OptionId::Isystem,
     .name = "-isystem",
     .value = ValueKind::Required,
     .valueName = "DIR",
     .help = "like -I but searched after every -I, and a file found there is a "
             "system header, so warnings in it are suppressed and errors are not"},
    {.id = OptionId::Target,
     .name = "--target",
     .value = ValueKind::Required,
     .valueName = "TRIPLE",
     .help = "the target the C type spellings and the layout are read against, as an "
             "LLVM triple in arch-vendor-os[-env] form. A triple this compiler does "
             "not state is refused with a reason, never guessed at",
     .defaultValue = sema::kDefaultTriple},

    // --- what a command prints --------------------------------------------
    {.id = OptionId::NoTrivia,
     .name = "--no-trivia",
     .help = "leave whitespace and comments out of dump output; it filters the "
             "display, and the token stream keeps every byte either way"},
    {.id = OptionId::Ast,
     .name = "--ast",
     .help = "print the tree instead of the tables: the lowered AST for resolve, the "
             "typed tree for check, where every node carries its type"},
    {.id = OptionId::Types, .name = "--types", .help = "print the table of types and nothing else"},
    {.id = OptionId::Stats,
     .name = "--stats",
     .help = "one summary line per input; without it check prints nothing on success"},
    {.id = OptionId::Refs,
     .name = "--refs",
     .help = "print every name use with the declaration it resolved to"},
    {.id = OptionId::Unresolved,
     .name = "--unresolved",
     .help = "print only the uses with no target, and the reason each one has"},
    {.id = OptionId::ListDefines,
     .name = "--defines",
     .help = "print the macros defined at the end of preprocessing"},
    {.id = OptionId::ListIncludes,
     .name = "--includes",
     .help = "print the include graph, one edge per line"},
    {.id = OptionId::ListDeps,
     .name = "--deps",
     .help = "print the files the unit read, as make dependencies"},
    {.id = OptionId::At,
     .name = "--at",
     .value = ValueKind::Required,
     .valueName = "POS",
     .help = "answer about one position: line for pp, line:col for resolve, each "
             "optionally prefixed by the file it belongs to"},

    // --- what build and run produce ---------------------------------------
    {.id = OptionId::Output,
     .name = "-o",
     .shortName = 'o',
     .value = ValueKind::Required,
     .valueName = "PATH",
     .help = "where the output goes: a.out by default (a.exe for a Windows target), "
             "or the input with its extension replaced for --emit obj or asm. `-o -` "
             "writes the object or listing to standard output"},
    {.id = OptionId::OptLevel,
     .name = "-O",
     .shortName = 'O',
     .value = ValueKind::Optional,
     .valueName = "LEVEL",
     .help = "optimisation level. A bare `-O` is -O1, and the level is never "
             "taken from the next argument",
     .defaultValue = "O0",
     .values = kOptLevels},
    {.id = OptionId::DebugInfo,
     .name = "-g",
     .shortName = 'g',
     .help = "emit debug information (DWARF on ELF and Mach-O, CodeView on PE), read "
             "by gdb, lldb and llvm-dwarfdump"},
    {.id = OptionId::Emit,
     .name = "--emit",
     .value = ValueKind::Required,
     .valueName = "KIND",
     .help = "what kind of file to write",
     .defaultValue = "exe",
     .values = kEmitKinds},
    {.id = OptionId::Verbose,
     .name = "-v",
     .shortName = 'v',
     .help = "print the commands the build runs, which is the first thing to look at "
             "after a link failure"},

    // --- the link ---------------------------------------------------------
    {.id = OptionId::LibraryDir,
     .name = "-L",
     .shortName = 'L',
     .value = ValueKind::Required,
     .valueName = "DIR",
     .help = "add a directory to the linker driver's search list"},
    {.id = OptionId::Library,
     .name = "-l",
     .shortName = 'l',
     .value = ValueKind::Required,
     .valueName = "NAME",
     .help = "link with a library; order is meaning, so libraries reach the driver "
             "in the order written, after the objects"},
    {.id = OptionId::Linker,
     .name = "--linker",
     .value = ValueKind::Required,
     .valueName = "PATH",
     .help = "the linker *driver* to use: clang, cc, gcc, or a path. By default the "
             "first of clang, cc and gcc found on PATH"},
    {.id = OptionId::Sysroot,
     .name = "--sysroot",
     .value = ValueKind::Required,
     .valueName = "DIR",
     .help = "the target's system root, handed to the linker driver; required with "
             "-L/-l when the target is not the host"},

    // --- warnings ---------------------------------------------------------
    {.id = OptionId::WarnUnused,
     .name = "-Wunused",
     .help = "warn about a declaration nothing refers to"},
    {.id = OptionId::WarnShadow,
     .name = "-Wshadow",
     .help = "warn about a declaration that hides another one"},
    {.id = OptionId::WarnConversion,
     .name = "-Wconversion",
     .help = "warn about an implicit conversion that may lose information"},
}};

// --- the groups -------------------------------------------------------------
//
// A group is an ordered list of ids: the parser's membership test and the help's
// table are the same list read from two sides.

constexpr std::array<OptionId, 4> kGlobalIds{OptionId::Help, OptionId::Version, OptionId::Color,
                                             OptionId::ErrorLimit};
constexpr std::array<OptionId, 5> kInputIds{OptionId::Define, OptionId::Undefine, OptionId::Include,
                                            OptionId::Isystem, OptionId::Target};
constexpr std::array<OptionId, 3> kWarningIds{OptionId::WarnUnused, OptionId::WarnShadow,
                                              OptionId::WarnConversion};
constexpr std::array<OptionId, 2> kLintIds{OptionId::WarnUnused, OptionId::WarnShadow};

constexpr std::array<OptionId, 4> kPpIds{OptionId::ListDefines, OptionId::ListIncludes,
                                         OptionId::ListDeps, OptionId::At};
constexpr std::array<OptionId, 4> kResolveIds{OptionId::Ast, OptionId::Refs, OptionId::Unresolved,
                                              OptionId::At};
constexpr std::array<OptionId, 3> kCheckIds{OptionId::Ast, OptionId::Types, OptionId::Stats};
constexpr std::array<OptionId, 1> kParseIds{OptionId::NoTrivia};
constexpr std::array<OptionId, 1> kModuleDebugIds{OptionId::DebugInfo};
constexpr std::array<OptionId, 5> kEmitIds{OptionId::Output, OptionId::OptLevel, OptionId::Emit,
                                           OptionId::DebugInfo, OptionId::Verbose};
constexpr std::array<OptionId, 4> kLinkIds{OptionId::LibraryDir, OptionId::Library,
                                           OptionId::Linker, OptionId::Sysroot};

constexpr OptionGroup kGlobalGroup{.title = "Global options", .ids = kGlobalIds};
constexpr OptionGroup kInputGroup{.title = "Input", .ids = kInputIds};
constexpr OptionGroup kWarningGroup{.title = "Diagnostics", .ids = kWarningIds};
constexpr OptionGroup kLintGroup{.title = "Diagnostics", .ids = kLintIds};
constexpr OptionGroup kPpGroup{.title = "What to print", .ids = kPpIds};
constexpr OptionGroup kResolveGroup{.title = "What to print", .ids = kResolveIds};
constexpr OptionGroup kCheckGroup{.title = "What to print", .ids = kCheckIds};
constexpr OptionGroup kParseGroup{.title = "Output", .ids = kParseIds};
constexpr OptionGroup kModuleDebugGroup{.title = "Code generation", .ids = kModuleDebugIds};
constexpr OptionGroup kEmitGroup{.title = "Output", .ids = kEmitIds};
constexpr OptionGroup kLinkGroup{.title = "Linking", .ids = kLinkIds};

// --- the commands -----------------------------------------------------------

constexpr std::array<std::string_view, 1> kBuildUsage{"mincc build [options] <files...>"};
constexpr std::array<std::string_view, 1> kRunUsage{"mincc run [options] <files...> [-- args...]"};
constexpr std::array<std::string_view, 1> kCheckUsage{"mincc check [options] <files...>"};
constexpr std::array<std::string_view, 1> kLexUsage{"mincc lex <files...>"};
constexpr std::array<std::string_view, 1> kParseUsage{"mincc parse [options] <files...>"};
constexpr std::array<std::string_view, 1> kPpUsage{"mincc pp [options] <files...>"};
constexpr std::array<std::string_view, 1> kResolveUsage{"mincc resolve [options] <files...>"};
constexpr std::array<std::string_view, 1> kIrUsage{"mincc ir [options] <files...>"};
constexpr std::array<std::string_view, 1> kBuiltinsUsage{"mincc builtins"};

constexpr std::array<std::string_view, 3> kBuildExamples{
    "$ mincc build -o prog main.mx", "$ mincc build -O2 --emit obj main.mx",
    "$ mincc build -o prog main.mx util.mx -L build -l math"};
constexpr std::array<std::string_view, 3> kRunExamples{
    "$ mincc run main.mx", "$ mincc run main.mx -- one two", "$ mincc run main.mx -- -o --emit"};
constexpr std::array<std::string_view, 3> kCheckExamples{
    "$ mincc check main.mx", "$ mincc check --stats main.mx", "$ mincc check -Wconversion main.mx"};
constexpr std::array<std::string_view, 2> kLexExamples{"$ mincc lex main.mx",
                                                       "$ mincc lex - < main.mx"};
constexpr std::array<std::string_view, 2> kParseExamples{"$ mincc parse main.mx",
                                                         "$ mincc parse --no-trivia main.mx"};
constexpr std::array<std::string_view, 3> kPpExamples{
    "$ mincc pp main.mx", "$ mincc pp -I include --defines main.mx", "$ mincc pp --at 12 main.mx"};
constexpr std::array<std::string_view, 2> kResolveExamples{"$ mincc resolve main.mx",
                                                           "$ mincc resolve --unresolved main.mx"};
constexpr std::array<std::string_view, 2> kIrExamples{
    "$ mincc ir main.mx", "$ mincc ir -g --target aarch64-unknown-linux-gnu main.mx"};
constexpr std::array<std::string_view, 1> kBuiltinsExamples{"$ mincc builtins"};
// Nothing but the global page's options: this command reads no file and has no
// mode of its own, which is the whole of its interface.
constexpr std::array<OptionGroup, 1> kBuiltinsGroups{kGlobalGroup};

constexpr std::array<std::string_view, 2> kSeeCheckAndBuild{"check", "build"};
constexpr std::array<std::string_view, 2> kSeeRunAndCheck{"run", "check"};
constexpr std::array<std::string_view, 2> kSeeLexAndPp{"lex", "pp"};
constexpr std::array<std::string_view, 2> kSeeParseAndResolve{"parse", "resolve"};
constexpr std::array<std::string_view, 2> kSeePpAndLex{"pp", "lex"};
constexpr std::array<std::string_view, 2> kSeeCheckAndParse{"check", "parse"};
constexpr std::array<std::string_view, 2> kSeeIrAndBuild{"ir", "build"};
constexpr std::array<std::string_view, 2> kSeeBuiltinsAndCheck{"builtins", "check"};
constexpr std::array<std::string_view, 1> kSeeBuild{"build"};

constexpr std::array<OptionGroup, 5> kBuildGroups{kEmitGroup, kLinkGroup, kInputGroup,
                                                  kWarningGroup, kGlobalGroup};
constexpr std::array<OptionGroup, 5> kRunGroups{kEmitGroup, kLinkGroup, kInputGroup, kWarningGroup,
                                                kGlobalGroup};
constexpr std::array<OptionGroup, 4> kCheckGroups{kCheckGroup, kInputGroup, kWarningGroup,
                                                  kGlobalGroup};
constexpr std::array<OptionGroup, 1> kLexGroups{kGlobalGroup};
constexpr std::array<OptionGroup, 3> kParseGroups{kParseGroup, kInputGroup, kGlobalGroup};
constexpr std::array<OptionGroup, 3> kPpGroups{kPpGroup, kInputGroup, kGlobalGroup};
constexpr std::array<OptionGroup, 4> kResolveGroups{kResolveGroup, kInputGroup, kLintGroup,
                                                    kGlobalGroup};
constexpr std::array<OptionGroup, 4> kIrGroups{kModuleDebugGroup, kInputGroup, kWarningGroup,
                                               kGlobalGroup};

constexpr std::array<CommandSpec, 9> kCommands{{
    {.command = Command::Build,
     .name = "build",
     .summary = "compile and link an executable",
     .brief = "build <files...>",
     .usage = kBuildUsage,
     .description = "The whole pipeline, to a file: the front end, the LLVM lowering, the "
                    "object, and a link. Nothing is linked by this compiler; a C driver "
                    "(clang, cc or gcc) is invoked with an argv array and does the link, "
                    "because the startup objects, the C runtime and the platform's linker "
                    "flags are knowledge a C toolchain already has and a second copy of them "
                    "would be a second set of ways to be wrong.\n\n"
                    "The invariant scan runs before the object is written: a module that "
                    "violates a rule of the language produces a diagnostic and no file at all.",
     .groups = kBuildGroups,
     .examples = kBuildExamples,
     .seeAlso = kSeeRunAndCheck,
     .implemented = true},
    {.command = Command::Run,
     .name = "run",
     .summary = "build a program and run it",
     .brief = "run <files...> [-- args...]",
     .usage = kRunUsage,
     .description = "`build`, and then the executable is run. The program is a child process "
                    "and not a JIT in this one, so its crash is its own, its exit status is "
                    "the one this command returns, and its standard streams are the "
                    "terminal's.\n\n"
                    "Everything after `--` is passed to the program and read by nobody here: "
                    "`mincc run p.mx -- -o --emit` hands the program two ordinary arguments.",
     .groups = kRunGroups,
     .examples = kRunExamples,
     .seeAlso = kSeeBuild,
     .implemented = true},
    {.command = Command::Check,
     .name = "check",
     .summary = "type-check every unit; nothing is emitted",
     .brief = "check <files...>",
     .usage = kCheckUsage,
     .description = "The front end through type checking, and nothing after it. On success "
                    "there is no output and the status is 0, which is what makes it usable in "
                    "a script unchanged; the flags below ask for a view of what was decided.",
     .groups = kCheckGroups,
     .examples = kCheckExamples,
     .seeAlso = kSeeIrAndBuild,
     .implemented = true},
    {.command = Command::Lex,
     .name = "lex",
     .summary = "one file's raw tokens, without preprocessing",
     .brief = "lex <files...>",
     .usage = kLexUsage,
     .description = "The raw lexer over one file. No directive is interpreted, so a `#` is an "
                    "ordinary token and `#include` is not expanded -- that is `pp`'s work. "
                    "Every byte of the file belongs to exactly one token, whitespace and "
                    "comments included, and a lexical error is reported with a caret rather "
                    "than stopping the scan.",
     .groups = kLexGroups,
     .examples = kLexExamples,
     .seeAlso = kSeePpAndLex,
     .implemented = true},
    {.command = Command::Parse,
     .name = "parse",
     .summary = "the syntax tree of the preprocessed stream",
     .brief = "parse <files...>",
     .usage = kParseUsage,
     .description = "The lexer, the preprocessor and the parser, in that order: the tree is "
                    "built over the preprocessed stream, which is why a file that begins with "
                    "`#define` has a tree rather than an error on the `#`.\n\n"
                    "The tree is lossless -- every byte, trivia included -- so --no-trivia is a "
                    "display filter and not a different parse.",
     .groups = kParseGroups,
     .examples = kParseExamples,
     .seeAlso = kSeeParseAndResolve,
     .implemented = true},
    {.command = Command::Pp,
     .name = "pp",
     .summary = "preprocess each file and print the record it kept",
     .brief = "pp <files...>",
     .usage = kPpUsage,
     .description = "The translation unit's token stream: includes resolved, macros expanded, "
                    "conditionals decided. The default output is that stream; the flags below "
                    "ask for the record the preprocessor kept instead -- the macros it ended "
                    "with, the include graph, the files it read, or the answer at one position.",
     .groups = kPpGroups,
     .examples = kPpExamples,
     .seeAlso = kSeeLexAndPp,
     .implemented = true},
    {.command = Command::Resolve,
     .name = "resolve",
     .summary = "resolve every name and print the declarations",
     .brief = "resolve <files...>",
     .usage = kResolveUsage,
     .description = "The first command that looks at meaning rather than at shape: the tree is "
                    "lowered, validated, and every name is tied to the declaration it denotes. "
                    "It does not look at types -- that is `check` -- so a name that resolves is "
                    "reported as resolving whatever its type turns out to be.",
     .groups = kResolveGroups,
     .examples = kResolveExamples,
     .seeAlso = kSeeCheckAndParse,
     .implemented = true},
    {.command = Command::Ir,
     .name = "ir",
     .summary = "lower each file to LLVM IR and print the module",
     .brief = "ir <files...>",
     .usage = kIrUsage,
     .description = "The typed tree into an `llvm::Module`, printed as text. The CFG, the "
                    "optimisers and the cross-platform target are LLVM's; this stage decides "
                    "nothing and materialises what the type checker recorded, so the module is "
                    "reviewable and every example is a test of the lowering.",
     .groups = kIrGroups,
     .examples = kIrExamples,
     .seeAlso = kSeeCheckAndBuild,
     .implemented = true},
    {.command = Command::Builtins,
     .name = "builtins",
     .summary = "list the names the language binds",
     .brief = "builtins",
     .usage = kBuiltinsUsage,
     .description = "The compiler's own table of builtins: the names it binds in the file "
                    "scope, what each one's signature and status are, and what it lowers to. "
                    "No file is read, and the list is the *data* the checker and the lowering "
                    "read -- so a page that describes one of these cannot describe something "
                    "the compiler does not have.\n\n"
                    "`prelude` names (`clz`, `rotl`, ...) are ordinary names the language "
                    "binds: a local declaration shadows one, and a file-scope declaration of "
                    "it is a redeclaration. `__builtin_*` is the compiler's own and cannot be "
                    "declared or `#define`d at all.",
     .groups = kBuiltinsGroups,
     .examples = kBuiltinsExamples,
     .seeAlso = kSeeBuiltinsAndCheck,
     .implemented = true},
}};

// --- the program ------------------------------------------------------------

constexpr std::array<Fact, 3> kProgramFacts{{
    {"Hosts:", "Linux, macOS, Windows (Clang, GCC, MSVC)"},
    {"Targets:", "x86_64, aarch64, riscv64, i386 -- Linux, macOS, Windows and FreeBSD, "
                 "each named by an LLVM triple, so a target is data and not a code path"},
    {"C interop:",
     "plain C ABI objects and libraries, linked by a C compiler driver (clang/cc/gcc), "
     "so a foreign linker is never second-guessed; -L and -l reach it unchanged"},
}};

constexpr std::array<std::string_view, 2> kProgramUsage{
    "mincc <command> [options] [files...]",
    "mincc help [command]",
};

constexpr std::array<OptionGroup, 1> kGlobalGroups{kGlobalGroup};

constexpr std::array<Fact, 4> kEnvironment{{
    {"NO_COLOR", "any value disables color on top of what --color asked for"},
    {"TERM", "`dumb` disables color, the way every other tool on the system reads it"},
    {"COLUMNS", "the width help output wraps to; set it and the guess is not made"},
    {"SOURCE_DATE_EPOCH", "__DATE__ and __TIME__ become reproducible, or an error when "
                          "it is unset -- the clock is never read"},
}};

constexpr std::array<Fact, 4> kExitStatus{{
    {"0", "success"},
    {"1", "a diagnostic was printed: this compiler, the toolchain, or a program that "
          "`run` could not execute or that did not exit normally"},
    {"2", "the command line was invalid"},
    {"n", "`run` only: the program's own status, which is why `run` is a launcher and "
          "not an interpreter"},
}};

constexpr ProgramSpec kProgram{
    .name = "mincc",
    .tagline = "minc+ - minimal C with extras, full C interoperability",
    .facts = kProgramFacts,
    .usage = kProgramUsage,
    .groups = kGlobalGroups,
    .environment = kEnvironment,
    .exitStatus = kExitStatus,
};

} // namespace

const ProgramSpec& programSpec() {
  return kProgram;
}

std::span<const CommandSpec> allCommands() {
  return kCommands;
}

std::span<const OptionGroup> globalOptionGroups() {
  return kGlobalGroups;
}

const char* toString(Command command) {
  for (const CommandSpec& spec : kCommands) {
    if (spec.command == command) {
      return spec.name.data();
    }
  }
  return "unknown";
}

std::optional<Command> commandFromName(std::string_view name) {
  for (const CommandSpec& spec : kCommands) {
    if (spec.name == name) {
      return spec.command;
    }
  }
  return std::nullopt;
}

const CommandSpec& commandSpec(Command command) {
  for (const CommandSpec& spec : kCommands) {
    if (spec.command == command) {
      return spec;
    }
  }
  // Unreachable: the table has a row per enumerator and a test walks every one.
  return kCommands.front();
}

std::span<const OptionSpec> allOptions() {
  return kOptions;
}

const OptionSpec& optionById(OptionId id) {
  for (const OptionSpec& option : kOptions) {
    if (option.id == id) {
      return option;
    }
  }
  // Unreachable for the same reason, and the same test: every id has a row.
  return kOptions.front();
}

bool optionAppliesTo(OptionId id, Command command) {
  for (const OptionGroup& group : commandSpec(command).groups) {
    for (const OptionId listed : group.ids) {
      if (listed == id) {
        return true;
      }
    }
  }
  return false;
}

const OptionSpec* findOptionByName(std::string_view name) {
  for (const OptionSpec& option : kOptions) {
    if (option.name == name) {
      return &option;
    }
  }
  return nullptr;
}

const OptionSpec* findOptionByShortName(char letter) {
  for (const OptionSpec& option : kOptions) {
    if (option.shortName == letter) {
      return &option;
    }
  }
  return nullptr;
}

const OptionSpec* findJoinedValueOption(std::string_view arg) {
  // Longest wins, so a future `-isystemfoo`-shaped pair cannot shadow the more
  // specific name that begins the same way.
  const OptionSpec* best = nullptr;
  for (const OptionSpec& option : kOptions) {
    if (!option.takesValue() || option.name.size() >= arg.size()) {
      continue;
    }
    // A single dash and no second: `-D`, `-I`, `-isystem`. A `--long` name never
    // accepts a joined value, because `--emit=obj` is spelled with the `=`.
    if (option.name.size() < 2 || option.name[0] != '-' || option.name[1] == '-') {
      continue;
    }
    if (arg.rfind(option.name, 0) != 0) {
      continue;
    }
    if (best == nullptr || option.name.size() > best->name.size()) {
      best = &option;
    }
  }
  return best;
}

std::span<const Command> ownersOf(OptionId id) {
  // One vector per option, filled from the command table itself: a new command
  // cannot forget to declare what it owns, because it owns whatever its groups
  // list.
  static const std::vector<std::vector<Command>> table = [] {
    std::vector<std::vector<Command>> built(kOptions.size());
    for (const CommandSpec& spec : kCommands) {
      for (const OptionGroup& group : spec.groups) {
        for (const OptionId listed : group.ids) {
          built[static_cast<std::size_t>(listed)].push_back(spec.command);
        }
      }
    }
    return built;
  }();
  return table[static_cast<std::size_t>(id)];
}

bool isGlobalOption(OptionId id) {
  for (const OptionId listed : kGlobalIds) {
    if (listed == id) {
      return true;
    }
  }
  return false;
}

std::span<const std::string_view> allOptionNames() {
  static const std::vector<std::string_view> names = [] {
    std::vector<std::string_view> built;
    built.reserve(kOptions.size());
    for (const OptionSpec& option : kOptions) {
      built.push_back(option.name);
    }
    return built;
  }();
  return names;
}

std::span<const std::string_view> optionNamesOf(Command command) {
  static const std::vector<std::vector<std::string_view>> table = [] {
    // The bound is **derived** from the table and not written down. It used to be
    // `Command::Ir + 1`, which is a second copy of "how many commands are there"
    // -- and the day a ninth command was added, this function wrote past the end
    // of the table for it, so the *nearest option* suggestion for that command's
    // arguments answered with whatever was in the next allocation. A bound that a
    // new enumerator cannot widen is a bound that will be wrong.
    std::size_t count = 0;
    for (const CommandSpec& spec : kCommands) {
      count = std::max(count, static_cast<std::size_t>(spec.command) + 1);
    }
    std::vector<std::vector<std::string_view>> built{count};
    for (const CommandSpec& spec : kCommands) {
      std::vector<std::string_view>& names = built[static_cast<std::size_t>(spec.command)];
      for (const OptionGroup& group : spec.groups) {
        for (const OptionId listed : group.ids) {
          names.push_back(optionById(listed).name);
        }
      }
    }
    return built;
  }();
  return table[static_cast<std::size_t>(command)];
}

std::span<const std::string_view> commandNames() {
  static const std::vector<std::string_view> names = [] {
    std::vector<std::string_view> built;
    built.reserve(kCommands.size());
    for (const CommandSpec& spec : kCommands) {
      built.push_back(spec.name);
    }
    return built;
  }();
  return names;
}

} // namespace minc::driver
