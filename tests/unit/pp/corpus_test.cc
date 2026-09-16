// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
//
// Two whole-corpus claims:
//
//   * every `.mx` file in the tree preprocesses cleanly, so the corpus cannot
//     drift into directives the preprocessor does not accept;
//   * every error code the stage can report is *reachable* from some input, so a
//     code cannot be added to the closed set and then never fire.
//
// The second is the same guarantee the lexer's flag table and the parser's error
// codes carry, and for the same reason: a diagnostic that no input can produce
// is a promise nothing keeps.
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "pp/pp_error.h"
#include "tests/examples_dir.h"

#include "pp_fixture.h"

namespace minc::test {
namespace {

std::string readFile(const std::filesystem::path& path) {
  std::ifstream in(path.generic_string(), std::ios::binary);
  return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<std::filesystem::path> filesUnder(const std::filesystem::path& dir) {
  std::vector<std::filesystem::path> files;
  std::error_code error;
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::recursive_directory_iterator(dir, error)) {
    if (entry.is_regular_file(error) && entry.path().extension() == ".mx") {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

TEST(CorpusTest, EveryExamplePreprocessesCleanly) {
  const std::filesystem::path examples(minc::test::kExamplesDir);
  const std::filesystem::path includeDir = examples / "pp" / "include";
  const std::vector<std::filesystem::path> files = filesUnder(examples);
  ASSERT_FALSE(files.empty());

  for (const std::filesystem::path& path : files) {
    PPFixture fixture(path.generic_string());
    fixture.source(readFile(path));
    // The preprocessor corpus is compiled with the include path it documents;
    // the language examples have no includes and ignore the flag.
    if (path.parent_path().filename() == "pp") {
      fixture.includeDir(includeDir.generic_string());
    } else if (path.parent_path().filename() == "pp") {
      fixture.includeDir(includeDir.generic_string());
    }
    const PPOutcome out = fixture.run();
    EXPECT_TRUE(out.errors.empty()) << path.generic_string() << ": " << out.errors.front();
    EXPECT_TRUE(out.warnings.empty()) << path.generic_string() << ": " << out.warnings.front();
  }
}

TEST(CorpusTest, EveryExampleProducesTokensOrIsOnlyDirectives) {
  // A file that produces no tokens must be a file of definitions (the
  // preprocessor corpus is allowed to be exactly that); anything else is a sign
  // the file was swallowed.
  const std::filesystem::path examples(minc::test::kExamplesDir);
  for (const std::filesystem::path& path : filesUnder(examples)) {
    if (path.parent_path().filename() == "pp") {
      continue;
    }
    PPFixture fixture(path.generic_string());
    fixture.source(readFile(path));
    EXPECT_FALSE(fixture.run().spellings.empty()) << path.generic_string();
  }
}

// --- reachability -----------------------------------------------------------
//
// One input per code. The fixture returns the codes by their stable names, so
// this table is also the check that the name a user greps for cannot drift from
// the enumerator.

using Trigger = PPOutcome (*)();

struct CodeTrigger {
  std::string_view code;
  std::string_view description;
  Trigger trigger;
};

PPOutcome invalidDirective() {
  return PPFixture().source("#nonsense\n").run();
}
PPOutcome unknownPragma() {
  return PPFixture().source("#pragma whatever\n").warnUnknownPragma().run();
}
PPOutcome notSupported() {
  return PPFixture().source("#embed \"x\"\n").run();
}
PPOutcome errorDirective() {
  return PPFixture().source("#error stop\n").run();
}
PPOutcome warningDirective() {
  return PPFixture().source("#warning look\n").run();
}
PPOutcome invalidLine() {
  return PPFixture().source("#line abc\n").run();
}
PPOutcome unterminatedConditional() {
  return PPFixture().source("#if 1\nx\n").run();
}
PPOutcome unexpectedConditional() {
  return PPFixture().source("#endif\n").run();
}
PPOutcome elseAfterElse() {
  return PPFixture().source("#if 0\na\n#else\nb\n#else\nc\n#endif\n").run();
}
PPOutcome conditionalNesting() {
  std::string text;
  for (int i = 0; i < 300; ++i) {
    text += "#if 1\n";
  }
  return PPFixture().source(text).run();
}
// A macro taking a name the compiler keeps. `#undef` of the same name is *not*
// here, deliberately: nothing reserved can be defined, so there is nothing for an
// `#undef` to take away, and a rule with no failing program behind it is a rule
// nobody can keep honest.
PPOutcome reservedIdentifier() {
  return PPFixture().source("#define __builtin_trap 1\n").run();
}
PPOutcome macroRedefined() {
  return PPFixture().source("#define A 1\n#define A 2\n").run();
}
PPOutcome macroParameterLimit() {
  std::string text = "#define BIG(";
  for (int i = 0; i < 300; ++i) {
    text += (i == 0 ? "" : ", ") + std::string("p") + std::to_string(i);
  }
  text += ") p0\n";
  return PPFixture().source(text).run();
}
PPOutcome missingMacroArguments() {
  return PPFixture().source("#define F(a, b) a\nF(1)\n").run();
}
PPOutcome tooManyMacroArguments() {
  return PPFixture().source("#define F(a) a\nF(1, 2)\n").run();
}
PPOutcome unterminatedMacroArguments() {
  return PPFixture().source("#define F(a) a\nF(1\n").run();
}
PPOutcome invalidPaste() {
  return PPFixture().source("#define C(a, b) a##b\nC(+, *)\n").run();
}
PPOutcome invalidHashOperand() {
  return PPFixture().source("#define H(x) # y\n").run();
}
PPOutcome missingMacroName() {
  return PPFixture().source("#define\n").run();
}
PPOutcome strayHashOperator() {
  // A `#` with something other than whitespace before it on its line: the
  // preprocessor has no directive to open and no macro body to paste in.
  return PPFixture().source("let x = 1 + # 2;\n").run();
}
PPOutcome invalidPragmaOperand() {
  // `_Pragma` takes one string literal; a number names no pragma.
  return PPFixture().source("_Pragma(123)\n").run();
}
PPOutcome expansionDepth() {
  std::string text;
  for (int i = 0; i < 300; ++i) {
    text += "#define M" + std::to_string(i) + " " +
            (i == 0 ? std::string("1") : "M" + std::to_string(i - 1)) + "\n";
  }
  return PPFixture().source(text + "M299\n").run();
}
PPOutcome expansionBudget() {
  return PPFixture().source("#define B x x x x\nB\n").tokenBudget(2).run();
}
PPOutcome expressionSyntax() {
  return PPFixture().source("#if 1 +\n#endif\n").run();
}
PPOutcome undefinedIdentifier() {
  return PPFixture().source("#if NOPE\n#endif\n").warnUndef().run();
}
PPOutcome includeNotFound() {
  return PPFixture().source("#include \"no/such.h\"\n").run();
}
PPOutcome includeUnreadable() {
  const TempDir dir;
  // A real file at a real path whose bytes cannot be used: the include is
  // found, and what fails is the reading. Distinct from "not found" on purpose.
  dir.write("bogus.h", std::string("\xFF\xFE", 2));
  return PPFixture(dir.path() + "/main.mx").source("#include \"bogus.h\"\n").run();
}
PPOutcome includeSelfReference() {
  const TempDir dir;
  const std::string path = dir.write("self.h", "#include \"self.h\"\n");
  return PPFixture(path).source(readFile(path)).run();
}
PPOutcome includeDepth() {
  const TempDir dir;
  dir.write("inner.h", "#define INNER 1\n");
  return PPFixture(dir.path() + "/main.mx")
      .source("#include \"inner.h\"\n")
      .includeDepthBudget(1)
      .run();
}
PPOutcome includeBudget() {
  // The budget is the number of `#include` directives a translation unit may
  // have, so a budget of one *permits* one and the second is what is refused.
  const TempDir dir;
  dir.write("one.h", "#define ONE 1\n");
  dir.write("two.h", "#define TWO 2\n");
  return PPFixture(dir.path() + "/main.mx")
      .source("#include \"one.h\"\n#include \"two.h\"\n")
      .includeCountBudget(1)
      .run();
}
PPOutcome missingIncludeGuard() {
  const TempDir dir;
  dir.write("bare.h", "#define BARE 1\n");
  return PPFixture(dir.path() + "/main.mx")
      .source("#include \"bare.h\"\n#include \"bare.h\"\n")
      .run();
}
PPOutcome preprocessedBytes() {
  return PPFixture().source("let x: i32 = 1;\n").byteBudget(4).run();
}
PPOutcome tokenTooLong() {
  const std::string big(4100, 'a');
  return PPFixture().source("#define P(a, b) a##b\nP(" + big + ", b)\n").run();
}
PPOutcome dateWithoutEpoch() {
  return PPFixture().source("__DATE__\n").run();
}

constexpr CodeTrigger kTriggers[] = {
    {"pp-invalid-directive", "#nonsense", &invalidDirective},
    {"pp-unknown-pragma", "#pragma whatever with -Wunknown-pragmas", &unknownPragma},
    {"pp-not-supported", "#embed", &notSupported},
    {"pp-error-directive", "#error", &errorDirective},
    {"pp-warning-directive", "#warning", &warningDirective},
    {"pp-invalid-line", "#line abc", &invalidLine},
    {"pp-unterminated-conditional", "eof inside a conditional", &unterminatedConditional},
    {"pp-unexpected-conditional", "#endif alone", &unexpectedConditional},
    {"pp-else-after-else", "two #else", &elseAfterElse},
    {"pp-conditional-nesting", "300 nested #if", &conditionalNesting},
    {"pp-reserved-identifier", "a #define of a reserved name", &reservedIdentifier},
    {"pp-macro-redefined", "different replacement list", &macroRedefined},
    {"pp-macro-parameter-limit", "300 parameters", &macroParameterLimit},
    {"pp-missing-macro-arguments", "one argument for two parameters", &missingMacroArguments},
    {"pp-too-many-macro-arguments", "two arguments for one parameter", &tooManyMacroArguments},
    {"pp-unterminated-macro-arguments", "eof inside an argument list", &unterminatedMacroArguments},
    {"pp-invalid-paste", "## forming two tokens", &invalidPaste},
    {"pp-invalid-hash-operand", "# before a non-parameter", &invalidHashOperand},
    {"pp-missing-macro-name", "#define without a name", &missingMacroName},
    {"pp-stray-hash", "a '#' that starts no line", &strayHashOperator},
    {"pp-invalid-pragma-operand", "_Pragma with a non-literal operand", &invalidPragmaOperand},
    {"pp-expansion-depth", "300 chained macros", &expansionDepth},
    {"pp-expansion-budget", "lowered token budget", &expansionBudget},
    {"pp-expression-syntax", "an incomplete expression", &expressionSyntax},
    {"pp-undefined-identifier", "-Wundef", &undefinedIdentifier},
    {"pp-include-not-found", "a missing header", &includeNotFound},
    {"pp-include-unreadable", "a header whose bytes are not UTF-8", &includeUnreadable},
    {"pp-include-self-reference", "a self-including file", &includeSelfReference},
    {"pp-include-depth", "lowered include depth", &includeDepth},
    {"pp-include-budget", "lowered include count", &includeBudget},
    {"pp-missing-include-guard", "an unguarded header twice", &missingIncludeGuard},
    {"pp-output-budget", "lowered output budget", &preprocessedBytes},
    {"pp-token-too-long", "a paste past the token limit", &tokenTooLong},
    {"pp-date-without-epoch", "__DATE__ with no SOURCE_DATE_EPOCH", &dateWithoutEpoch},
};

TEST(ErrorCodeTest, EveryCodeHasATrigger) {
  for (const pp::PPErrorCodeInfo& info : pp::ppErrorCodeInfos()) {
    const auto found =
        std::find_if(std::begin(kTriggers), std::end(kTriggers),
                     [&info](const CodeTrigger& trigger) { return trigger.code == info.name; });
    EXPECT_NE(found, std::end(kTriggers))
        << "no test input triggers " << info.name << "; a code with no trigger is a code nothing "
        << "keeps";
  }
}

TEST(ErrorCodeTest, EveryTriggerFires) {
  for (const CodeTrigger& trigger : kTriggers) {
    const PPOutcome out = trigger.trigger();
    bool fired = out.hasError(trigger.code) || out.hasWarning(trigger.code);
    EXPECT_TRUE(fired) << trigger.code << " (" << trigger.description << ") did not fire";
  }
}

TEST(ErrorCodeTest, NoCodeIsListedTwice) {
  for (const pp::PPErrorCodeInfo& info : pp::ppErrorCodeInfos()) {
    std::size_t count = 0;
    for (const CodeTrigger& trigger : kTriggers) {
      count += trigger.code == info.name ? 1U : 0U;
    }
    EXPECT_EQ(count, 1U) << info.name;
  }
  EXPECT_EQ(std::size(kTriggers), pp::allPPErrorCodes().size());
}

} // namespace
} // namespace minc::test
