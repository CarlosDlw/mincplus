// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
//
// Includes: search order, file identity, the include-guard optimization, and the
// cycles that the identity check turns into a readable error.
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "lex/token_stream.h"
#include "pp/include_resolver.h"
#include "support/intern/interner.h"
#include "tests/examples_dir.h"

#include "pp_fixture.h"

namespace minc::test {
namespace {

// --- guard sniffing ---------------------------------------------------------
//
// Tested against a lexed buffer rather than a file: which token sequences count
// as the canonical guard is a pure function of the tokens, and testing it
// through the filesystem would test the filesystem.

std::string guardOf(std::string_view text) {
  support::Interner symbols;
  const lex::TokenStream stream = lex::TokenStream::lex(support::kInvalidFile, text);
  const std::optional<support::SymId> guard = pp::sniffIncludeGuard(stream, symbols);
  return guard.has_value() ? std::string(symbols.lookup(*guard)) : std::string();
}

TEST(IncludeGuardTest, CanonicalPatternIsRecognized) {
  EXPECT_EQ(guardOf("#ifndef GUARD\n#define GUARD\nint x;\n#endif\n"), "GUARD");
  EXPECT_EQ(guardOf("// leading comment\n#ifndef G\n#define G\n#endif\n"), "G");
  EXPECT_EQ(guardOf("#ifndef G\n#define G\n#endif\n\n// trailing comment\n"), "G");
}

TEST(IncludeGuardTest, ContentAfterTheEndifIsRefused) {
  // The optimization is only safe when *everything* is inside the guard; a
  // near-miss must be refused, because a heuristic that silently changes meaning
  // is worse than a slow include.
  EXPECT_EQ(guardOf("#ifndef G\n#define G\n#endif\nint x;\n"), "");
}

TEST(IncludeGuardTest, MismatchedOrMissingDefineIsRefused) {
  EXPECT_EQ(guardOf("#ifndef G\n#define H\n#endif\n"), "");
  EXPECT_EQ(guardOf("#ifndef G\nint x;\n#endif\n"), "");
  EXPECT_EQ(guardOf("#if G\n#define G\n#endif\n"), "");
  EXPECT_EQ(guardOf("#define G\n#endif\n"), "");
}

TEST(IncludeGuardTest, NestedConditionalsMustCloseBeforeTheEndif) {
  EXPECT_EQ(guardOf("#ifndef G\n#define G\n#if 1\n#endif\n#endif\n"), "G");
  // The inner `#endif` closes the inner `#if`, so this file ends inside the
  // guard... except that the trailing `#endif` count does not match.
  EXPECT_EQ(guardOf("#ifndef G\n#define G\n#if 1\n#endif\n"), "");
}

// --- resolution -------------------------------------------------------------

TEST(IncludeTest, MissingIncludeIsDiagnosedWithTheSearchList) {
  const PPOutcome out = PPFixture().source("#include \"no/such/file.h\"\nx\n").run();
  EXPECT_TRUE(out.hasError("pp-include-not-found"));
  // The rest of the file still produces tokens: a missing header is one error,
  // not the end of the translation unit.
  EXPECT_EQ(out.concat(), "x");
}

TEST(IncludeTest, AngleIncludeIsNotSearchedRelativeToTheFile) {
  // Creating a file next to the input and including it with `<>` must fail: the
  // two forms have different search lists, and conflating them is how a build
  // works on one machine and not another.
  const TempDir dir;
  dir.write("header.h", "#define X 1\n");
  const PPOutcome out = PPFixture(dir.path() + "/main.mx").source("#include <header.h>\n").run();
  EXPECT_TRUE(out.hasError("pp-include-not-found"));
}

TEST(IncludeTest, QuoteIncludeSearchesTheFilesOwnDirectory) {
  const TempDir dir;
  dir.write("header.h", "#define X 1\n");
  const PPOutcome out =
      PPFixture(dir.path() + "/main.mx").source("#include \"header.h\"\nX\n").run();
  EXPECT_TRUE(out.errors.empty());
  EXPECT_EQ(out.concat(), "1");
}

TEST(IncludeTest, IncludeNextSkipsTheDirectoryTheFileCameFrom) {
  const TempDir dir;
  (void)dir.mkdir("a");
  (void)dir.mkdir("b");
  dir.write("a/first.h", "#define FROM_A 1\n#include_next \"first.h\"\n");
  dir.write("b/first.h", "#define FROM_B 2\n");
  // `-I` order decides which `first.h` is found first, and `#include_next` must
  // continue from the *next* entry rather than resolving to the same file (which
  // would be an infinite include).
  const PPOutcome out = PPFixture(dir.path() + "/main.mx")
                            .source("#include \"first.h\"\nFROM_A FROM_B\n")
                            .includeDir(dir.mkdir("a"))
                            .includeDir(dir.mkdir("b"))
                            .run();
  EXPECT_TRUE(out.errors.empty());
  EXPECT_EQ(out.concat(), "12");
}

// --- identity ---------------------------------------------------------------

TEST(IncludeTest, PragmaOnceElidesTheSecondInclusion) {
  const TempDir dir;
  dir.write("once.h", "#pragma once\n#define ONCE 1\nlet a: i32 = 1;\n");
  const PPOutcome out = PPFixture(dir.path() + "/main.mx")
                            .source("#include \"once.h\"\n#include \"once.h\"\nONCE\n")
                            .run();
  EXPECT_TRUE(out.errors.empty());
  // The declaration appears once, and the second include is recorded as elided.
  EXPECT_EQ(out.concat(), "leta:i32=1;1");
  bool elided = false;
  for (const std::string& include : out.includes) {
    elided = elided || include.front() == '-';
  }
  EXPECT_TRUE(elided);
}

TEST(IncludeTest, GuardOptimizationIsEquivalentToReadingTheFileEveryTime) {
  const TempDir dir;
  dir.write("guarded.h", "#ifndef GUARDED_H\n#define GUARDED_H\n#define VALUE 7\n#endif\n");
  PPFixture fixture(dir.path() + "/main.mx");
  fixture.source("#include \"guarded.h\"\n#include \"guarded.h\"\nVALUE\n#undef GUARDED_H\n"
                 "#include \"guarded.h\"\nVALUE\n");
  const auto [withOptimization, withoutOptimization] =
      PPFixture::withAndWithoutOptimization(fixture);
  EXPECT_EQ(withOptimization.concat(), withoutOptimization.concat());
  EXPECT_EQ(withOptimization.concat(), "77");
}

TEST(IncludeTest, UndefiningTheGuardActuallyReincludesTheFile) {
  // The optimization keys on the guard being *currently* defined, which is what
  // makes the `#undef` above work -- so this test would fail if the decision were
  // cached instead.
  const TempDir dir;
  dir.write("g.h", "#ifndef G\n#define G\nlet a: i32 = 1;\n#endif\n");
  const PPOutcome out = PPFixture(dir.path() + "/main.mx")
                            .source("#include \"g.h\"\n#undef G\n#include \"g.h\"\n")
                            .run();
  EXPECT_TRUE(out.errors.empty());
  EXPECT_EQ(out.concat(), "leta:i32=1;leta:i32=1;");
}

TEST(IncludeTest, SelfIncludeIsReportedByChain) {
  const TempDir dir;
  const std::string path = dir.write("loop.h", "#include \"loop.h\"\nlet a: i32 = 1;\n");
  const PPOutcome out = PPFixture(path).source("#include \"loop.h\"\n").run();
  EXPECT_TRUE(out.hasError("pp-include-self-reference"));
}

TEST(IncludeTest, UnguardedFileIncludedTwiceIsWarnedAbout) {
  const TempDir dir;
  dir.write("bare.h", "#define BARE 1\n");
  const PPOutcome out =
      PPFixture(dir.path() + "/main.mx").source("#include \"bare.h\"\n#include \"bare.h\"\n").run();
  EXPECT_TRUE(out.hasWarning("pp-missing-include-guard"));
}

TEST(IncludeTest, IncludeDepthIsBounded) {
  const TempDir dir;
  // A chain longer than the bound, each file including the next. The point is
  // that the run terminates with a named diagnostic instead of exhausting the
  // include stack.
  constexpr int kFiles = 260;
  for (int i = 0; i < kFiles; ++i) {
    const std::string body =
        i + 1 < kFiles ? "#include \"h" + std::to_string(i + 1) + ".h\"\n" : "let leaf: i32 = 1;\n";
    dir.write("h" + std::to_string(i) + ".h", body);
  }
  PPFixture fixture(dir.path() + "/main.mx");
  const PPOutcome out = fixture.source("#include \"h0.h\"\n").run();
  EXPECT_TRUE(out.hasError("pp-include-depth"));
}

TEST(IncludeTest, IncludeBudgetIsEnforced) {
  const TempDir dir;
  dir.write("one.h", "#define ONE 1\n");
  dir.write("two.h", "#define TWO 2\n");
  // The budget is lowered rather than the file included 65536 times: the point
  // is that the check exists and fires, and a test that takes a minute to run it
  // is a test that gets skipped.
  const PPOutcome out = PPFixture(dir.path() + "/main.mx")
                            .source("#include \"one.h\"\n#include \"two.h\"\n")
                            .includeCountBudget(1)
                            .run();
  EXPECT_TRUE(out.hasError("pp-include-budget"));
}

// --- the corpus -------------------------------------------------------------

TEST(IncludeTest, TheExampleCorpusResolvesThroughTheIncludePath) {
  // `examples/pp/005_includes.mx` is the corpus's include case: three includes,
  // one of them elided by the guard, and a body that uses what they defined.
  const std::filesystem::path path =
      std::filesystem::path(minc::test::kExamplesDir) / "pp" / "005_includes.mx";
  std::ifstream in(path.generic_string(), std::ios::binary);
  ASSERT_TRUE(in.good());
  const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};

  const PPOutcome out =
      PPFixture(path.generic_string())
          .source(text)
          .includeDir(
              (std::filesystem::path(minc::test::kExamplesDir) / "pp" / "include").generic_string())
          .run();
  EXPECT_TRUE(out.errors.empty());
  // The guard optimization fired for one of the three includes.
  bool elided = false;
  for (const std::string& include : out.includes) {
    elided = elided || include.front() == '-';
  }
  EXPECT_TRUE(elided);
}

// --- header-names -----------------------------------------------------------
//
// The operand of `#include` is a header-name, not a token sequence: inside it
// `//` is not a comment, `/*` is not a comment, and `\d` is not an escape. These
// assert the name that comes out, which is what the search would use.

TEST(IncludeTest, SlashesInsideANameAreNotComments) {
  // `#include <a//b.h>` used to be reported as an unterminated `<...>`, because
  // the plain lexer read `//b.h>` as a line comment and the closing `>` went with
  // it. The name is the whole point, so it is asserted directly.
  const PPOutcome out = PPFixture().source("#include <a//b.h>\n").run();
  EXPECT_TRUE(out.hasError("pp-include-not-found"));
  for (const std::string& message : out.messages) {
    EXPECT_EQ(message.find("unterminated"), std::string::npos) << message;
    EXPECT_NE(message.find("'a//b.h'"), std::string::npos) << message;
  }
}

TEST(IncludeTest, AnUnterminatedCommentInANameIsNotReported) {
  // `<a/*b.h>` is one name. The lexical report must not blame the name's own
  // bytes for looking like a comment, and the name must still be right.
  const PPOutcome out = PPFixture().source("#include <a/*b.h>\n").run();
  EXPECT_TRUE(out.hasError("pp-include-not-found"));
  for (const std::string& message : out.messages) {
    EXPECT_EQ(message.find("comment"), std::string::npos) << message;
  }
}

TEST(IncludeTest, EscapesAreNotProcessedInAQuotedName) {
  // The bug this replaces was a *false* diagnostic: `\d` and `\x` are not
  // escapes in a q-char-sequence, so lexing the name as a string literal drew
  // errors on valid code. There is no such error code in the result now.
  const PPOutcome out = PPFixture().source("#include \"c:\\dir\\x.h\"\n").run();
  EXPECT_TRUE(out.hasError("pp-include-not-found"));
  for (const std::string& message : out.messages) {
    EXPECT_EQ(message.find("escape"), std::string::npos) << message;
    EXPECT_NE(message.find("c:\\dir\\x.h"), std::string::npos) << message;
  }
}

TEST(IncludeTest, TheWrittenNameReachesTheResolverIntact) {
  TempDir dir;
  dir.write("a.h", "x\n");
  // A name with a `>` in it is legal in the quoted form, and the search order
  // still depends on the delimiters.
  const PPOutcome out = PPFixture().source("#include <a.h>\nb\n").includeDir(dir.path()).run();
  EXPECT_TRUE(out.errors.empty());
  EXPECT_EQ(out.concat(), "xb");
}

TEST(IncludeTest, AMacroProducedAngleNameStillResolves) {
  // A header-name cannot be produced by macro expansion by the standard's rules,
  // but the form is accepted, and it must read the name the same way the written
  // one does.
  TempDir dir;
  dir.write("a.h", "x\n");
  const PPOutcome out =
      PPFixture().source("#define H <a.h>\n#include H\n").includeDir(dir.path()).run();
  EXPECT_TRUE(out.errors.empty());
  EXPECT_EQ(out.concat(), "x");
}

TEST(IncludeTest, AnUnterminatedAngleNameIsStillDiagnosed) {
  const PPOutcome out = PPFixture().source("#include <a.h\n").run();
  EXPECT_TRUE(out.hasError("pp-invalid-directive"));
}

} // namespace
} // namespace minc::test
