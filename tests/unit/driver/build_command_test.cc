// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// `build` and `run`, driven through their real entry point.
//
// `buildInputs` and `runInputs` are the commands minus the choice of streams, so a
// test asserts on the *contract* -- which exit code, which file on disk, what on
// stderr -- rather than on a spawned process. The one thing that cannot be tested
// without a toolchain is a program's own status flowing back out of `run`, and
// that is checked at the end by actually linking and running.
//
// A test that needs a linker driver looks for the driver's *refusal* and skips on
// it, rather than probing the `PATH` itself: the `PATH` rules are the platform's
// and this test may not contain any platform code. Object emission needs no linker
// at all, which is why most of these do not care.
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <iterator>
#include <optional>
#include <random>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "backend/codegen.h"
#include "driver/build_command.h"
#include "driver/cli.h"
#include "driver/exit_code.h"
#include "sema/target.h"
#include "support/term/terminal.h"

// In `minc::driver`, like every other command's tests: the types under test are
// the command's, and qualifying each one would make the test harder to read than
// the code it checks.
namespace minc::driver {
namespace {

// The message `backend` prints when no driver resolved. Matched as a substring
// because the diagnostic goes through the shared renderer, which prefixes and
// wraps it.
constexpr std::string_view kNoLinker = "no C linker driver found";

// A temporary directory that removes itself, so a failing test does not leave a
// tree of objects behind for the next run to trip over. The name carries a random
// value rather than a process id: `getpid` is platform code, and this test may not
// contain any (`architecture.md`).
class ScratchDir {
public:
  ScratchDir() {
    std::error_code error;
    const std::filesystem::path base = std::filesystem::temp_directory_path(error);
    if (error) {
      return;
    }
    std::random_device device;
    for (int attempt = 0; attempt < 64; ++attempt) {
      const std::filesystem::path candidate =
          base / ("minc-build-" + std::to_string(device()) + "-" + std::to_string(attempt));
      if (std::filesystem::create_directory(candidate, error)) {
        path_ = candidate;
        return;
      }
    }
  }
  ~ScratchDir() {
    if (!path_.empty()) {
      std::error_code ignored;
      (void)std::filesystem::remove_all(path_, ignored);
    }
  }
  ScratchDir(const ScratchDir&) = delete;
  ScratchDir& operator=(const ScratchDir&) = delete;

  [[nodiscard]] bool valid() const {
    return !path_.empty();
  }
  [[nodiscard]] std::string file(std::string_view name) const {
    return (path_ / name).string();
  }
  // Writes `text` and returns the path it landed on.
  [[nodiscard]] std::string write(std::string_view name, std::string_view text) const {
    const std::string full = file(name);
    std::ofstream out(full, std::ios::binary);
    out << text;
    return full;
  }

private:
  std::filesystem::path path_;
};

// `main` returns 42, so `run`'s exit status is distinguishable from every code this
// driver uses for its own failures.
constexpr std::string_view kProgram = "fn i32 main()\n{\n  return 42;\n}\n";

struct Outcome {
  int code = 0;
  std::string err;
  [[nodiscard]] bool skippedForNoLinker() const {
    return err.find(kNoLinker) != std::string::npos;
  }
};

[[nodiscard]] Outcome run(const BuildRequest& request, bool execute) {
  std::ostringstream err;
  Outcome outcome;
  outcome.code = execute ? runInputs(request, err) : buildInputs(request, err);
  outcome.err = err.str();
  return outcome;
}

[[nodiscard]] BuildRequest requestFor(const std::string& input) {
  BuildRequest request;
  request.inputs = {input};
  request.diagnosticColor = support::ColorMode::Plain;
  return request;
}

// Redirects `std::cerr` for the life of the object.
//
// `runBuild`/`runRun` are the *driver's* entry points and report to the real
// stderr, which is correct for a program and inconvenient for a test. Swapping the
// stream buffer is standard and portable, so the assertions can read what the user
// would have seen -- and the alternative, a second streaming overload used only by
// tests, would be a second format to keep in step.
class StderrCapture {
public:
  StderrCapture() : old_(std::cerr.rdbuf(captured_.rdbuf())) {}
  ~StderrCapture() {
    std::cerr.rdbuf(old_);
  }
  StderrCapture(const StderrCapture&) = delete;
  StderrCapture& operator=(const StderrCapture&) = delete;

  [[nodiscard]] std::string str() const {
    return captured_.str();
  }

private:
  std::ostringstream captured_;
  std::streambuf* old_;
};

// The parsed-options path, so the option validation (which lives above
// `BuildRequest`) is exercised too.
[[nodiscard]] Outcome runFromArgs(const std::vector<std::string>& arguments, bool execute) {
  std::vector<const char*> argv;
  argv.reserve(arguments.size() + 1);
  argv.push_back("mincc");
  for (const std::string& argument : arguments) {
    argv.push_back(argument.c_str());
  }
  const CliOptions options = parseArgs(static_cast<int>(argv.size()), argv.data());
  Outcome outcome;
  if (!options.error.empty()) {
    outcome.code = exitCode(ExitCode::Usage);
    outcome.err = options.error;
    return outcome;
  }
  StderrCapture capture;
  outcome.code = execute ? runRun(options) : runBuild(options);
  outcome.err = capture.str();
  return outcome;
}

[[nodiscard]] std::string readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

TEST(BuildCommandTest, ObjectEmissionWritesAnObjectAndNothingElse) {
  ScratchDir scratch;
  ASSERT_TRUE(scratch.valid());
  const std::string source = scratch.write("ok.mx", kProgram);
  const std::string object = scratch.file("ok.o");

  BuildRequest request = requestFor(source);
  request.kind = OutputKind::Object;
  request.output = object;
  const Outcome outcome = run(request, /*execute=*/false);

  EXPECT_EQ(outcome.code, 0) << outcome.err;
  EXPECT_TRUE(std::filesystem::exists(object));
  EXPECT_FALSE(readFile(object).empty());
}

TEST(BuildCommandTest, TheDefaultObjectNameIsTheInputWithItsExtensionReplaced) {
  ScratchDir scratch;
  ASSERT_TRUE(scratch.valid());
  const std::string source = scratch.write("default_name.mx", kProgram);

  BuildRequest request = requestFor(source);
  request.kind = OutputKind::Object;
  const Outcome outcome = run(request, /*execute=*/false);

  EXPECT_EQ(outcome.code, 0) << outcome.err;
  // An absolute path, so the object lands next to the source rather than in
  // whatever directory the test happened to run in.
  EXPECT_TRUE(std::filesystem::exists(scratch.file("default_name.o")));
}

TEST(BuildCommandTest, AssemblyListingIsText) {
  ScratchDir scratch;
  ASSERT_TRUE(scratch.valid());
  const std::string source = scratch.write("asm.mx", kProgram);
  const std::string listing = scratch.file("asm.s");

  BuildRequest request = requestFor(source);
  request.kind = OutputKind::Assembly;
  request.output = listing;
  const Outcome outcome = run(request, /*execute=*/false);

  EXPECT_EQ(outcome.code, 0) << outcome.err;
  const std::string text = readFile(listing);
  EXPECT_FALSE(text.empty());
  // A listing is printable text that names the function it contains. It is not
  // asserted against the text section's directive, because that is the target's
  // spelling and not the language's: `.text` is ELF's, and Mach-O calls it
  // `__TEXT,__text`, which is what this test read on the first macOS run.
  EXPECT_EQ(text.find('\0'), std::string::npos) << "a listing is made of characters";
  EXPECT_NE(text.find("main"), std::string::npos) << text.substr(0, 200);
}

TEST(BuildCommandTest, AUnitThatFailsToCheckWritesNoObjectAtAll) {
  // The contract that matters most: a module built from a tree with an error is a
  // module nobody decided the meaning of, so there is nothing to emit -- and a
  // partial link is worse than a failure.
  ScratchDir scratch;
  ASSERT_TRUE(scratch.valid());
  const std::string source =
      scratch.write("bad.mx", "fn i32 main()\n{\n  return \"not a number\";\n}\n");
  const std::string object = scratch.file("bad.o");

  BuildRequest request = requestFor(source);
  request.kind = OutputKind::Object;
  request.output = object;
  const Outcome outcome = run(request, /*execute=*/false);

  EXPECT_EQ(outcome.code, 1);
  EXPECT_FALSE(outcome.err.empty());
  EXPECT_FALSE(std::filesystem::exists(object));
}

TEST(BuildCommandTest, VerbosePrintsTheCommandsTheBuildRuns) {
  ScratchDir scratch;
  ASSERT_TRUE(scratch.valid());
  const std::string source = scratch.write("verbose.mx", kProgram);
  const std::string object = scratch.file("verbose.o");

  BuildRequest request = requestFor(source);
  request.kind = OutputKind::Object;
  request.output = object;
  request.verbose = true;
  const Outcome outcome = run(request, /*execute=*/false);

  EXPECT_EQ(outcome.code, 0) << outcome.err;
  EXPECT_NE(outcome.err.find("emit=object"), std::string::npos) << outcome.err;
  EXPECT_NE(outcome.err.find(object), std::string::npos) << outcome.err;
}

TEST(BuildCommandTest, UnknownEmitKindIsAUsageError) {
  const Outcome outcome = runFromArgs({"build", "--emit", "wasm", "a.mx"}, false);
  EXPECT_EQ(outcome.code, exitCode(ExitCode::Usage));
  EXPECT_NE(outcome.err.find("wasm"), std::string::npos);
  EXPECT_NE(outcome.err.find("'exe', 'obj' and 'asm'"), std::string::npos);
}

TEST(BuildCommandTest, UnknownOptimisationLevelIsAUsageError) {
  const Outcome outcome = runFromArgs({"build", "-O4", "a.mx"}, false);
  EXPECT_EQ(outcome.code, exitCode(ExitCode::Usage));
  EXPECT_NE(outcome.err.find("-O4"), std::string::npos);
}

TEST(BuildCommandTest, EveryStatedOptimisationLevelIsAccepted) {
  // The list is the `OptLevel` enum; this is the test that catches a level added
  // to the enum and not to the parser's accepted spellings.
  for (const char* level : {"-O0", "-O1", "-O2", "-O3", "-Os", "-Oz", "-O"}) {
    const CliOptions options = [&] {
      const char* argv[] = {"mincc", "build", level, "a.mx"};
      return parseArgs(4, argv);
    }();
    EXPECT_TRUE(options.error.empty()) << level << ": " << options.error;
    EXPECT_TRUE(backend::optLevelFromName(options.optLevel).has_value()) << level;
  }
}

TEST(BuildCommandTest, NoInputsIsAUsageError) {
  const Outcome outcome = runFromArgs({"build"}, false);
  EXPECT_EQ(outcome.code, exitCode(ExitCode::Usage));
  EXPECT_NE(outcome.err.find("no input files"), std::string::npos);
}

TEST(BuildCommandTest, WritingAnExecutableToStandardOutputIsAUsageError) {
  const Outcome outcome = runFromArgs({"build", "-o", "-", "a.mx"}, false);
  EXPECT_EQ(outcome.code, exitCode(ExitCode::Usage));
  EXPECT_NE(outcome.err.find("cannot write an executable"), std::string::npos);
}

TEST(BuildCommandTest, OneOutputForSeveralObjectsIsAUsageErrorRatherThanAGuess) {
  ScratchDir scratch;
  ASSERT_TRUE(scratch.valid());
  const std::string a = scratch.write("a.mx", kProgram);
  const std::string b = scratch.write("b.mx", kProgram);

  std::ostringstream err;
  BuildRequest request;
  request.inputs = {a, b};
  request.kind = OutputKind::Object;
  request.output = scratch.file("one.o");
  const int code = buildInputs(request, err);

  EXPECT_EQ(code, exitCode(ExitCode::Usage)) << err.str();
  EXPECT_NE(err.str().find("would write each one to the same file"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(scratch.file("one.o")));
}

TEST(BuildCommandTest, SeveralObjectsNamedAfterTheirInputsAreFine) {
  ScratchDir scratch;
  ASSERT_TRUE(scratch.valid());
  const std::string a = scratch.write("one.mx", kProgram);
  const std::string b = scratch.write("two.mx", kProgram);

  BuildRequest request;
  request.inputs = {a, b};
  request.kind = OutputKind::Object;
  const Outcome outcome = run(request, /*execute=*/false);

  EXPECT_EQ(outcome.code, 0) << outcome.err;
  EXPECT_TRUE(std::filesystem::exists(scratch.file("one.o")));
  EXPECT_TRUE(std::filesystem::exists(scratch.file("two.o")));
}

TEST(BuildCommandTest, AnUnknownTargetIsRefusedWithTheOnesThatExist) {
  const Outcome outcome = runFromArgs({"build", "--target", "sparc64-nope-linux", "a.mx"}, false);
  EXPECT_EQ(outcome.code, exitCode(ExitCode::Usage));
  EXPECT_NE(outcome.err.find("sparc64-nope-linux"), std::string::npos);
}

TEST(BuildCommandTest, CrossLinkingIsRefusedWithoutADriverAndASysroot) {
  // The objects are emitted for the foreign target -- that needs no toolchain --
  // and the *link* is what is refused, because the target's C runtime is not the
  // host's and the failure would otherwise be a link error about `libc`. No linker
  // is needed for this test to be meaningful, which is the point of doing the check
  // before anything is spawned.
  ScratchDir scratch;
  ASSERT_TRUE(scratch.valid());
  const std::string source = scratch.write("cross.mx", kProgram);

  BuildRequest request = requestFor(source);
  const std::optional<sema::TargetInfo> target = sema::targetFromName("aarch64-unknown-linux-gnu");
  ASSERT_TRUE(target.has_value());
  request.target = *target;
  request.output = scratch.file("cross-out");
  const Outcome outcome = run(request, /*execute=*/false);

  EXPECT_EQ(outcome.code, 1) << outcome.err;
  EXPECT_NE(outcome.err.find("--linker"), std::string::npos) << outcome.err;
  EXPECT_NE(outcome.err.find("--sysroot"), std::string::npos) << outcome.err;
}

TEST(BuildCommandTest, CrossCompilingAnObjectNeedsNoToolchainAtAll) {
  ScratchDir scratch;
  ASSERT_TRUE(scratch.valid());
  const std::string source = scratch.write("cross_obj.mx", kProgram);
  const std::string object = scratch.file("cross_obj.o");

  BuildRequest request = requestFor(source);
  const std::optional<sema::TargetInfo> target = sema::targetFromName("aarch64-unknown-linux-gnu");
  ASSERT_TRUE(target.has_value());
  request.target = *target;
  request.kind = OutputKind::Object;
  request.output = object;
  const Outcome outcome = run(request, /*execute=*/false);

  EXPECT_EQ(outcome.code, 0) << outcome.err;
  EXPECT_FALSE(readFile(object).empty());
}

TEST(BuildCommandTest, WhatEmissionWritesIsARealObjectAndNotAnErrorPage) {
  // The bytes matter: `emitModule` returning no diagnostics proves it *tried*, and
  // an object with no `0x7fELF` in it would be a truncated or empty file the linker
  // then rejects with a message about the file rather than about the compiler.
  ScratchDir scratch;
  ASSERT_TRUE(scratch.valid());
  const std::string source = scratch.write("reloc.mx", kProgram);
  const std::string object = scratch.file("reloc.o");

  BuildRequest request = requestFor(source);
  request.kind = OutputKind::Object;
  request.output = object;
  const Outcome outcome = run(request, /*execute=*/false);
  ASSERT_EQ(outcome.code, 0) << outcome.err;

  const std::string bytes = readFile(object);
  ASSERT_GE(bytes.size(), 5u);
  if (std::string_view(bytes).substr(0, 4) != std::string_view("\x7f"
                                                               "ELF",
                                                               4)) {
    GTEST_SKIP() << "the host target's objects are not ELF";
  }
  // `ELFCLASS64`, and little-endian for every 64-bit target this compiler states.
  EXPECT_EQ(static_cast<unsigned char>(bytes[4]), 2u);
  EXPECT_EQ(static_cast<unsigned char>(bytes[5]), 1u);
}

// The name the linker actually wrote. The C driver appends its platform's
// executable suffix -- `-o linked` produces `linked.exe` next to the `linked` a
// user asked for -- and that is the linker's convention, not this compiler's, so
// there is nothing for the compiler to normalise here. Probing for both names
// rather than being told which platform this is keeps the file's rule: a test may
// contain no platform code.
[[nodiscard]] std::string linkedOutput(const std::string& requested) {
  if (std::filesystem::exists(requested)) {
    return requested;
  }
  const std::string suffixed = requested + ".exe";
  return std::filesystem::exists(suffixed) ? suffixed : requested;
}

TEST(BuildCommandTest, LinkingProducesAnExecutableAndRunReturnsItsStatus) {
  ScratchDir scratch;
  ASSERT_TRUE(scratch.valid());
  const std::string source = scratch.write("linked.mx", kProgram);

  BuildRequest request = requestFor(source);
  request.output = scratch.file("linked");
  const Outcome built = run(request, /*execute=*/false);
  if (built.skippedForNoLinker()) {
    GTEST_SKIP() << "no C linker driver on PATH";
  }
  EXPECT_EQ(built.code, 0) << built.err;
  EXPECT_TRUE(std::filesystem::exists(linkedOutput(request.output)))
      << "no executable at " << request.output;

  // `run` is `build` plus an exec, so the program's status is the driver's.
  BuildRequest runRequest = requestFor(source);
  runRequest.run = true;
  const Outcome executed = run(runRequest, /*execute=*/true);
  EXPECT_EQ(executed.code, 42) << executed.err;
}

// --- the checked build --------------------------------------------------------

// A program that reads one element past the end of a two-element view. The program
// is *wrong* and the checker was right to accept it -- the index is a value, and
// the compiler cannot see that it is out of range -- which is exactly the situation
// `checks.md` is about: the checker cannot prove it, so the build guards it.
// The program the checked-build tests use, and its shape is the whole point: the
// subscript steps **past the view's length and inside the array it views**. The
// index is a parameter, so the checker has no value to refuse and accepts it; the
// guard traps, because the view is two long and three is not; and the unchecked
// program reads a *real* element of a real object, which is what makes its exit
// status the same number on every machine. Reading past the object instead would
// be undefined behaviour -- the value would be whatever the stack happened to
// hold, and a test that asserts "not 1" against a garbage byte fails on the
// machine that happens to hold a 1 (which is how this one first failed, on
// macOS).
// The element the unchecked program reads: past `v`'s length, inside `t`, and
// deliberately not 1 -- the driver's own failure code.
constexpr int kOutOfViewElement = 12;

constexpr std::string_view kOutOfBoundsProgram = "fn i32 at(v: []i32, i: i32)\n"
                                                 "{\n"
                                                 "  return v[i];\n"
                                                 "}\n"
                                                 "fn i32 main()\n"
                                                 "{\n"
                                                 "  let t: [4]i32 = [7, 8, 9, 12];\n"
                                                 "  let v: []i32 = t[0..2];\n"
                                                 "  return at(v, 3);\n"
                                                 "}\n";

TEST(BuildCommandTest, TheCheckedBuildStopsAProgramTheCheckerCannotRefuse) {
  ScratchDir scratch;
  ASSERT_TRUE(scratch.valid());
  const std::string source = scratch.write("oob.mx", kOutOfBoundsProgram);

  // `-O0` is the checked build, so the default build traps. The program's own
  // message goes to its stderr, which the harness inherits rather than captures --
  // what is asserted here is the *status*: a trap is a death, and the driver
  // reports it as a failure rather than as an exit code the program chose.
  BuildRequest checked = requestFor(source);
  checked.checks = true;
  checked.run = true;
  const Outcome trapped = run(checked, /*execute=*/true);
  if (trapped.skippedForNoLinker()) {
    GTEST_SKIP() << "no C linker driver on PATH";
  }
  EXPECT_EQ(trapped.code, exitCode(ExitCode::Failure)) << trapped.err;
  EXPECT_NE(trapped.err.find("terminated abnormally"), std::string::npos) << trapped.err;

  // The same program, built without the guards: the access reads the element past
  // the view -- a real one, because the array it views is longer -- and the
  // program *exits with that value*, which is the difference the flag buys and
  // the reason it exists. The number is asserted exactly and not as "not 1":
  // the element is `12` on every machine and in both builds, so the test says
  // what it means (`kOutOfBoundsProgram` above).
  BuildRequest unchecked = requestFor(source);
  unchecked.checks = false;
  unchecked.run = true;
  const Outcome unguarded = run(unchecked, /*execute=*/true);
  EXPECT_EQ(unguarded.code, kOutOfViewElement)
      << "an unchecked build must not be killed by a guard it never emitted: " << unguarded.err;
}

TEST(BuildCommandTest, TheGuardsSurviveOptimisation) {
  // `memory.md` decision 19: a check the optimizer can delete is not a check. The
  // guards are in the checked build at *every* level, so `-fcheck -O2` traps where
  // `-O2` alone does not.
  ScratchDir scratch;
  ASSERT_TRUE(scratch.valid());
  const std::string source = scratch.write("oob2.mx", kOutOfBoundsProgram);

  BuildRequest optimised = requestFor(source);
  optimised.checks = true;
  optimised.level = backend::OptLevel::O2;
  optimised.run = true;
  const Outcome checked = run(optimised, /*execute=*/true);
  if (checked.skippedForNoLinker()) {
    GTEST_SKIP() << "no C linker driver on PATH";
  }
  EXPECT_EQ(checked.code, exitCode(ExitCode::Failure)) << checked.err;

  BuildRequest release = requestFor(source);
  release.level = backend::OptLevel::O2;
  release.checks = false;
  release.run = true;
  const Outcome unchecked = run(release, /*execute=*/true);
  EXPECT_EQ(unchecked.code, kOutOfViewElement) << unchecked.err;
}

TEST(BuildCommandTest, APeListingIsPositionIndependent) {
  // The measured failure, and the reason this test reads a *listing* rather than
  // a module: with LLVM's `Static` relocation model the guard's message was
  // addressed absolutely, so the object could not be linked -- `ld` refuses a
  // 32-bit absolute address of a `.data` object with "relocation truncated to
  // fit: IMAGE_REL_AMD64_ADDR32", because a PE image is based at `0x140000000`.
  // What made it a Windows-only, `-O2`-only failure is that the *form* depends on
  // the optimiser: `-O0` emitted `movabsq`, a 64-bit relocation, which links.
  // The listing is where the addressing is visible, and the target is a foreign
  // one because the question is about COFF and not about this machine.
  ScratchDir scratch;
  ASSERT_TRUE(scratch.valid());
  const std::string source = scratch.write("oob3.mx", kOutOfBoundsProgram);
  const std::string listing = scratch.file("oob3.s");

  const std::optional<sema::TargetInfo> target = sema::targetFromName(sema::kTripleWindowsAmd64);
  ASSERT_TRUE(target.has_value());
  BuildRequest request = requestFor(source);
  request.target = *target;
  request.kind = OutputKind::Assembly;
  request.output = listing;
  request.level = backend::OptLevel::O2;
  request.checks = true;
  const Outcome outcome = run(request, /*execute=*/false);
  ASSERT_EQ(outcome.code, 0) << outcome.err;

  const std::string text = readFile(listing);
  // Every reference to the guard's own message goes through the instruction
  // pointer -- `leaq .Lcheck.site(%rip), %rcx` -- which is the only form this
  // platform's linker can resolve, and (as `target.cc` records) what `clang`
  // emits for the target under every `-fPIC` setting.
  EXPECT_NE(text.find("(%rip)"), std::string::npos) << text.substr(0, 400);
  EXPECT_EQ(text.find("$.Lcheck.site"), std::string::npos)
      << "the guard's message is addressed absolutely, which PE cannot link";
}

// --- builtins, as answers -----------------------------------------------------
//
// The only tests in the suite where a builtin's *answer* is checked against the
// machine rather than against the module it emitted. They live here because the
// harness that compiles, links and runs is here, and they are worth having for
// the two rows whose answer is a *language* decision and not a hardware one:
// `clz(0)` is the width (LLVM's raw intrinsic would be poison), and a rotate's
// count is taken modulo the width (LLVM's raw intrinsic would be poison there
// too). A module that compiled and a machine that answers differently is exactly
// the failure a checker-only test cannot see.
[[nodiscard]] Outcome runProgram(ScratchDir& scratch, const std::string& name,
                                 const std::string& source) {
  const std::string path = scratch.write(name, source);
  BuildRequest request = requestFor(path);
  request.run = true;
  return run(request, /*execute=*/true);
}

// A program whose exit status is the value of `expression`, so the assertion is
// one number the machine produced. Exit statuses are a byte, which is why every
// value below is small.
[[nodiscard]] std::string returning(const std::string& expression) {
  return "fn i32 main() { return " + expression + "; }\n";
}

TEST(BuildCommandTest, TheBitOperationsAnswerWhatTheLanguageSays) {
  ScratchDir scratch;
  ASSERT_TRUE(scratch.valid());

  const Outcome leading = runProgram(scratch, "clz.mx", returning("clz(1)"));
  if (leading.skippedForNoLinker()) {
    GTEST_SKIP() << "no C linker driver on PATH";
  }
  EXPECT_EQ(leading.code, 31) << leading.err;

  const Outcome count = runProgram(scratch, "pop.mx", returning("popcount(255)"));
  EXPECT_EQ(count.code, 8) << count.err;

  const Outcome bytes =
      runProgram(scratch, "bswap.mx", "fn i32 main() { let x: u32 = 1; return bswap(x) >> 24; }\n");
  EXPECT_EQ(bytes.code, 1) << bytes.err;
}

TEST(BuildCommandTest, ZeroIsTheWidthAndNotUndefined) {
  // The promise, on the machine: LLVM's `ctlz(0)` is poison unless the flag says
  // otherwise, and this language defines it as the width. A compiler that passed
  // the raw intrinsic would be a program whose answer depends on what the
  // optimizer did that day.
  ScratchDir scratch;
  ASSERT_TRUE(scratch.valid());

  const Outcome leading = runProgram(scratch, "clz0.mx", returning("clz(0)"));
  if (leading.skippedForNoLinker()) {
    GTEST_SKIP() << "no C linker driver on PATH";
  }
  EXPECT_EQ(leading.code, 32) << leading.err;

  const Outcome trailing = runProgram(scratch, "ctz0.mx", returning("ctz(0)"));
  EXPECT_EQ(trailing.code, 32) << trailing.err;
}

TEST(BuildCommandTest, ARotateCountIsTakenModuloTheWidth) {
  ScratchDir scratch;
  ASSERT_TRUE(scratch.valid());

  // A full turn and a bit: `1` rotated left by 32 is `1` (the count reduces to
  // zero) and by 33 is `2`. Without the modulo, LLVM's funnel shift is poison for
  // both, and the answer would be whatever the backend decided.
  const Outcome full = runProgram(scratch, "rotl32.mx", returning("rotl(1, 32)"));
  if (full.skippedForNoLinker()) {
    GTEST_SKIP() << "no C linker driver on PATH";
  }
  EXPECT_EQ(full.code, 1) << full.err;

  const Outcome over = runProgram(scratch, "rotl33.mx", returning("rotl(1, 33)"));
  EXPECT_EQ(over.code, 2) << over.err;

  const Outcome right = runProgram(scratch, "rotr.mx", returning("rotr(2, 33)"));
  EXPECT_EQ(right.code, 1) << right.err;
}

// --- slices, as answers -------------------------------------------------------
//
// The view is the one feature whose *whole* meaning is aliasing: a slice that did
// not reach the storage it names would still type-check, still lower, and still
// pass every module assertion. So the proof is a program whose exit status
// changes only if the write through the view landed on the array -- the same
// argument the global-initializer tests make, made with a machine instead of a
// sentence.
TEST(BuildCommandTest, AWriteThroughAViewReachesTheArrayItViews) {
  ScratchDir scratch;
  ASSERT_TRUE(scratch.valid());

  // `view[0]` is `table[2]`, and the exit status is `table[2]` -- so the answer is
  // 30 only if the descriptor's first word is the address of the array's third
  // element. A slice that copied its elements would return 3.
  const Outcome aliased = runProgram(scratch, "view.mx",
                                     "fn i32 main() {\n"
                                     "  let table: [4]i32 = [1, 2, 3, 4];\n"
                                     "  let view: []i32 = table[2..4];\n"
                                     "  view[0] = 30;\n"
                                     "  return table[2];\n"
                                     "}\n");
  if (aliased.skippedForNoLinker()) {
    GTEST_SKIP() << "no C linker driver on PATH";
  }
  EXPECT_EQ(aliased.code, 30) << aliased.err;

  // ... and the *other* half, which is the one a value type would get wrong in
  // the opposite direction: a view of a view indexes from its own start, so
  // `inner[0]` is `table[3]` and not `table[2]`.
  const Outcome composed = runProgram(scratch, "view2.mx",
                                      "fn i32 main() {\n"
                                      "  let table: [8]i32 = [1, 2, 3, 4, 5, 6, 7, 8];\n"
                                      "  let mid: []i32 = table[2..6];\n"
                                      "  let inner: []i32 = mid[1..3];\n"
                                      "  mid[2] = 40;\n"
                                      "  return inner[0] + inner[1];\n"
                                      "}\n");
  EXPECT_EQ(composed.code, 44) << composed.err;
}

TEST(BuildCommandTest, ASliceCrossesACallAsAValue) {
  // Two words, by value: the callee sees the descriptor, so a slice *parameter*
  // and a slice *return* both have to be the same shape on both sides. The exit
  // status is read out of the returned view, and a return that lost its length or
  // its pointer would be a different number (or a crash) rather than a wrong
  // lookup.
  ScratchDir scratch;
  ASSERT_TRUE(scratch.valid());

  const Outcome crossed = runProgram(scratch, "cross.mx",
                                     "fn []i32 tail(a: []i32) { return a[1..3]; }\n"
                                     "fn i32 main() {\n"
                                     "  let table: [4]i32 = [5, 6, 7, 8];\n"
                                     "  return tail(table[..])[0];\n"
                                     "}\n");
  if (crossed.skippedForNoLinker()) {
    GTEST_SKIP() << "no C linker driver on PATH";
  }
  EXPECT_EQ(crossed.code, 6) << crossed.err;

  // From a pointer, which is the unchecked form and the only one where the two
  // bounds are both written: `p[1..3]` is `table[1]` and `table[2]`.
  const Outcome fromPointer = runProgram(scratch, "ptr.mx",
                                         "fn i32 sum(s: []i32) { return s[0] + s[1]; }\n"
                                         "fn i32 main() {\n"
                                         "  let table: [4]i32 = [5, 6, 7, 8];\n"
                                         "  let p: *i32 = &table[0];\n"
                                         "  return sum(p[1..3]);\n"
                                         "}\n");
  EXPECT_EQ(fromPointer.code, 13) << fromPointer.err;
}

TEST(BuildCommandTest, AChildThatDidNotExitIsNotAnExitStatus) {
  // Two platforms spell "the child died" differently, and the portable layer
  // documents one spelling. The rule is therefore pinned with the number each
  // platform actually produces, the Windows one included: it is the value that
  // used to be read as a program's own status, so on Linux, macOS and MinGW this
  // project reported a trap and on the Windows runner it reported nothing while
  // the test passed -- which is why the guard is arithmetic over the range rather
  // than an equality against one code.

  // POSIX: `WIFSIGNALED` (a trap is `SIGILL`), and the timeout path.
  EXPECT_TRUE(backend::abnormalTermination(-2));
  // Windows: `ud2` -- what `__builtin_trap` lowers to -- raises an
  // illegal-instruction exception, `GetExitCodeProcess` reports `0xC000001D`, and
  // `sys::Wait` returns it with its sign intact, so what arrives here is that code
  // read as a signed 32-bit value.
  EXPECT_TRUE(backend::abnormalTermination(static_cast<int>(0xC000001DU)));
  // An access violation, reported the same way: `0xC0000005`.
  EXPECT_TRUE(backend::abnormalTermination(static_cast<int>(0xC0000005U)));

  // What is *not* a death: `-1` is "could not execute", which is `spawnFailed`
  // and not `crashed` -- nothing ran, so nothing died -- and a program's own
  // status is never negative on either platform.
  EXPECT_FALSE(backend::abnormalTermination(-1));
  EXPECT_FALSE(backend::abnormalTermination(0));
  EXPECT_FALSE(backend::abnormalTermination(1));
  EXPECT_FALSE(backend::abnormalTermination(255));
}

TEST(BuildCommandTest, ATrapStopsTheProgramWhereItStands) {
  // `__builtin_trap` is the primitive `assert` is built on: not a return, not an
  // exit status the program chose. Two facts, and both are asserted: nothing after
  // it runs, and what `run` reports is not the status the program would have
  // returned. The `return 0;` after the call is what makes the second fact
  // checkable -- a compiler that let the call return would exit 0 -- and the sema
  // calling it unreachable is the warning it already is.
  ScratchDir scratch;
  ASSERT_TRUE(scratch.valid());

  const Outcome executed =
      runProgram(scratch, "trap.mx",
                 "fn i32 fail() { __builtin_trap(); }\nfn i32 main() { fail(); return 0; }\n");
  if (executed.skippedForNoLinker()) {
    GTEST_SKIP() << "no C linker driver on PATH";
  }
  EXPECT_NE(executed.code, 0) << executed.err;
  EXPECT_NE(executed.err.find("terminated abnormally"), std::string::npos)
      << "the driver did not report the death: " << executed.err;
}

TEST(BuildCommandTest, ACastOfAValueThatCannotFitTrapsAtBothOptimizationLevels) {
  // `casts.md`: float → integer is *defined as a trap* on a value the destination
  // cannot hold, and a check the optimizer can delete is not a check -- so the
  // program runs at both ends of the pipeline. The value is computed rather than
  // written (`1.0e30` in a binding, added to), because a literal operand is folded
  // at compile time and would prove nothing about the guard that is emitted.
  ScratchDir scratch;
  ASSERT_TRUE(scratch.valid());

  const std::string source = "fn i32 main()\n"
                             "{\n"
                             "  let x: f64 = 1.0e30;\n"
                             "  let y: f64 = x + 1.0;\n"
                             "  let n: i32 = y as i32;\n"
                             "  return n;\n"
                             "}\n";
  for (const auto level : {backend::OptLevel::O0, backend::OptLevel::O2}) {
    const std::string path =
        scratch.write("cast_" + std::to_string(static_cast<int>(level)) + ".mx", source);
    BuildRequest request = requestFor(path);
    request.run = true;
    request.level = level;
    const Outcome executed = run(request, /*execute=*/true);
    if (executed.skippedForNoLinker()) {
      GTEST_SKIP() << "no C linker driver on PATH";
    }
    // Not a return, and not a status the program chose: the trap.
    EXPECT_NE(executed.code, 0) << executed.err;
    EXPECT_NE(executed.err.find("terminated abnormally"), std::string::npos) << executed.err;
  }
}

TEST(BuildCommandTest, ACastOfAConstantThatCannotFitIsRefusedAndWritesNoProgram) {
  // The other half of the same rule, and the reason it is a *diagnostic*: the
  // compiler can see this value, so the program could only ever trap -- which
  // makes it a mistake about the source and not a property of the run.
  ScratchDir scratch;
  ASSERT_TRUE(scratch.valid());

  const std::string path =
      scratch.write("cast_const.mx", "fn i32 main() { let n = 1.0e30 as i32; return n; }\n");
  BuildRequest request = requestFor(path);
  request.output = scratch.file("cast_const.out");
  const Outcome built = run(request, /*execute=*/false);
  EXPECT_NE(built.code, 0);
  EXPECT_NE(built.err.find("ir-cast-out-of-range"), std::string::npos) << built.err;
  // No artifact: a refusal that still wrote an executable would leave the previous
  // one in place for a runner to pick up.
  std::ifstream produced(request.output, std::ios::binary);
  EXPECT_FALSE(produced.good()) << request.output;
}

TEST(BuildCommandTest, RunWithDebugInformationStillReturnsTheProgramsStatus) {
  // `-g` composes with `run`: it is a normal binary with debug information in it,
  // and the exit status still belongs to the program.
  ScratchDir scratch;
  ASSERT_TRUE(scratch.valid());
  const std::string source = scratch.write("run_g.mx", kProgram);

  BuildRequest request = requestFor(source);
  request.debugInfo = true;
  request.run = true;
  const Outcome executed = run(request, /*execute=*/true);
  if (executed.skippedForNoLinker()) {
    GTEST_SKIP() << "no C linker driver on PATH";
  }
  EXPECT_EQ(executed.code, 42) << executed.err;
}

TEST(BuildCommandTest, ADoubleDashInARunSendsEverythingAfterItToTheProgram) {
  // The parser's half of the contract: after `--`, a `run`'s positionals are the
  // program's and not the compiler's, so `-o` is an argument rather than an option.
  const char* argv[] = {"mincc", "run", "p.mx", "--", "-o", "out", "x.mx"};
  const CliOptions options = parseArgs(7, argv);
  EXPECT_TRUE(options.error.empty()) << options.error;
  ASSERT_EQ(options.inputs.size(), 1u);
  EXPECT_EQ(options.inputs[0], "p.mx");
  ASSERT_EQ(options.programArgs.size(), 3u);
  EXPECT_EQ(options.programArgs[0], "-o");
  EXPECT_EQ(options.programArgs[1], "out");
  EXPECT_EQ(options.programArgs[2], "x.mx");
}

TEST(BuildCommandTest, ADoubleDashInABuildStillMeansFiles) {
  // The separator keeps its other meaning where there is no program to give
  // arguments to, so a file whose name begins with `-` can still be compiled.
  const char* argv[] = {"mincc", "build", "--", "-weird-name.mx"};
  const CliOptions options = parseArgs(4, argv);
  EXPECT_TRUE(options.error.empty()) << options.error;
  ASSERT_EQ(options.inputs.size(), 1u);
  EXPECT_EQ(options.inputs[0], "-weird-name.mx");
  EXPECT_TRUE(options.programArgs.empty());
}

} // namespace
} // namespace minc::driver
