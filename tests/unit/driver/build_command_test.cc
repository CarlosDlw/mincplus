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
  EXPECT_TRUE(std::filesystem::exists(request.output));

  // `run` is `build` plus an exec, so the program's status is the driver's.
  BuildRequest runRequest = requestFor(source);
  runRequest.run = true;
  const Outcome executed = run(runRequest, /*execute=*/true);
  EXPECT_EQ(executed.code, 42) << executed.err;
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
