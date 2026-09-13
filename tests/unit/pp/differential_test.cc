// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
//
// The differential test: the same source through this preprocessor and through
// a reference one, compared token for token.
//
// Every other test in this directory states a rule its author thought of. This
// one states the rules nobody wrote down, because its oracle is a second
// implementation rather than a person. That is why it is the strongest check
// here and why it is still worth having next to hundreds of hand-written ones.
//
// What it is careful about:
//
//   * **The corpus is standard C.** A GNU extension would test agreement with
//     GCC, which is not the claim; the claim is agreement with C. The one
//     extension-shaped case is `__VA_ARGS__`, which is standard since C99.
//   * **Non-deterministic and location-dependent macros are absent.** `__DATE__`,
//     `__TIME__`, `__FILE__` and `__LINE__` have a different truth on each side
//     by construction, so comparing them would test the harness, not the code.
//   * **Comparison is token-wise, not byte-wise.** The two preprocessors
//     legitimately disagree about whitespace and about the blank lines a line
//     marker leaves behind; they must not disagree about tokens. Spellings are
//     read back by the lexer -- already independently tested -- so the harness
//     needs no second tokenizer of its own.
//   * **It skips, it does not fail, when no reference compiler is found.** A
//     machine without `cc` cannot run this check; that is not a machine where
//     this preprocessor is wrong. `MINC_REFERENCE_CC` overrides the search.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "lex/token_kind.h"
#include "lex/token_stream.h"
#include "support/span/file_id.h"

#include "pp_fixture.h"

namespace minc::test {
namespace {

// The reference, found once and remembered: it is a property of the machine, not
// of a test case, and probing for it per case would run a compiler for nothing.
struct ReferenceCompiler {
  std::string command;   // the program to run, as written on the command line
  std::string directory; // where its scratch files go
};

// Reads a file whole, or nothing when it is not there. The reference compiler's
// output is the one thing here that is not this project's code, so its absence
// is a normal answer rather than an error.
std::optional<std::string> readFileOrNothing(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

// Runs `command` through the platform's shell, which is what makes the
// redirection below portable: `std::system` is `/bin/sh -c` on POSIX and
// `cmd /c` on Windows, and both understand `>` and double quotes. The one thing
// neither is asked to do is survive an argument containing a quote, because no
// compiler name or generated path does.
void runShell(const std::string& command) {
  // The result is deliberately ignored: whether the compiler succeeded is
  // decided by whether its output file holds what was asked for, which is a
  // question about the file and not about a wait status (whose encoding differs
  // between the two shells).
  const int ignored = std::system(command.c_str());
  (void)ignored;
}

std::string quote(const std::string& text) {
  return "\"" + text + "\"";
}

// Preprocesses `source` with `compiler` and returns what it wrote, or nothing
// when it wrote nothing usable.
std::optional<std::string> referenceOutput(const ReferenceCompiler& compiler,
                                           const std::string& stem, const std::string& source) {
  const std::string in = compiler.directory + "/" + stem + ".mx";
  const std::string out = compiler.directory + "/" + stem + ".out";
  const std::string err = compiler.directory + "/" + stem + ".err";
  {
    std::ofstream file(in, std::ios::binary);
    if (!file) {
      return std::nullopt;
    }
    file << source;
  }

  // `-x c`: the corpus is C, whatever extension this project's file names use,
  // and without it GCC treats an unknown extension as a linker input.
  // `-E -P`: preprocess, and suppress the line markers -- the token stream is
  // the comparison, and a `# 1 "file"` line is not a token of the language.
  runShell(compiler.command + " -x c -E -P " + quote(in) + " > " + quote(out) + " 2> " +
           quote(err));
  return readFileOrNothing(out);
}

// The reference compiler, or nothing. A candidate counts as usable only when it
// turns a known input into a known output: that is also what proves the shell
// quoting above works on this machine, so a harness that cannot run is skipped
// rather than failed.
std::optional<ReferenceCompiler> findReferenceCompiler(const std::string& directory) {
  std::vector<std::string> candidates;
  if (const char* fromEnv = std::getenv("MINC_REFERENCE_CC");
      fromEnv != nullptr && *fromEnv != '\0') {
    candidates.emplace_back(fromEnv);
  }
  // `cc` first: it is the portable name, and on a machine that also has `gcc`
  // and `clang` it is the one whose presence is a promise about the platform.
  candidates.emplace_back("cc");
  candidates.emplace_back("gcc");
  candidates.emplace_back("clang");

  constexpr const char* kProbeText = "minc_reference_probe_marker\n";
  for (const std::string& candidate : candidates) {
    const ReferenceCompiler compiler{candidate, directory};
    const std::optional<std::string> output =
        referenceOutput(compiler, "minc_reference_probe", kProbeText);
    if (output.has_value() && output->find("minc_reference_probe_marker") != std::string::npos) {
      return compiler;
    }
  }
  return std::nullopt;
}

// The significant tokens of a preprocessed text, as spellings. The lexer is the
// tool this project already trusts for "where does a token end"; using it here
// keeps the comparison about preprocessing and not about lexing twice.
std::vector<std::string> spellings(const std::string& text) {
  const lex::TokenStream stream = lex::TokenStream::lex(support::kInvalidFile, text);
  std::vector<std::string> out;
  out.reserve(stream.significantCount());
  for (const std::uint32_t index : stream.significantIndices()) {
    const lex::Token& token = stream.tokens()[index];
    // The stream's end-of-file is the parser's stop condition, not part of any
    // file's text, so it is not a token either side can be asked to agree on.
    if (token.is(lex::TokenKind::EndOfFile)) {
      continue;
    }
    out.emplace_back(text.substr(token.offset, token.length));
  }
  return out;
}

std::string joined(const std::vector<std::string>& values) {
  std::string out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out.push_back(' ');
    }
    out += values[i];
  }
  return out;
}

// A corpus of standard-C macro behavior. Each case is short on purpose: when one
// fails, the whole of it is visible in the failure message.
struct CorpusCase {
  const char* name;
  const char* source;
};

constexpr CorpusCase kCorpus[] = {
    {"object-like expansion", R"(#define A 1
A A
)"},
    {"function-like expansion", R"(#define ADD(a, b) ((a) + (b))
ADD(1, 2)
)"},
    {"nested expansion", R"(#define X Y
#define Y 3
X
)"},
    {"stringification", R"(#define S(x) #x
S(hello world)
)"},
    {"token pasting", R"(#define C(a, b) a##b
C(foo, bar)
)"},
    {"variadic arguments", R"(#define V(...) f(__VA_ARGS__)
V(1, 2, 3)
)"},
    {"a macro does not expand inside itself", R"(#define A A + 1
A
)"},
    {"argument prescan", R"(#define ONE 1
#define ID(x) x
ID(ONE)
)"},
    // `E` is an object-like macro with an empty body, and `##` does *not* expand
    // its operands, so the paste is of the name as written: `E1`, not `1`.
    {"an empty macro pasted is its name", R"(#define E
#define CAT(a, b) a##b
CAT(E, 1)
)"},
    {"conditional selection", R"(#if 1
one
#else
two
#endif
)"},
    {"#elif", R"(#if 0
a
#elif 1
b
#else
c
#endif
)"},
    {"constant arithmetic", R"(#if 2 + 2 * 2 == 6
ok
#endif
)"},
    {"defined and short-circuit", R"(#define D 1
#if defined(D) && D
ok
#endif
)"},
    {"undef", R"(#define D 1
#undef D
#ifdef D
bad
#else
ok
#endif
)"},
    {"comments are whitespace", "/* c */ x // y\n"},
    {"adjacent invocations", R"(#define P(a) a
P(x)P(y)
)"},
};

// The one input the two preprocessors do not agree on, recorded rather than
// hidden.
//
// `lexer.md` decision 4 keeps line splicing out of the lexical grammar: a `\` at
// the end of a line is an `Invalid` token and not the phase-2 deletion the C
// standard makes it, so a macro definition cannot continue onto the next line.
// That is a *language* decision and not a preprocessing bug, which is why the
// input is not in `kCorpus`: the corpus is for behavior both sides must agree
// on.
//
// It is pinned here because this harness is what made the cost of the decision
// visible. `#define X a \` + newline is the ordinary way a macro is written, and
// every C-family preprocessor accepts it. When the lexer gains phase 2, the fix
// is to move this input into `kCorpus` and delete this test -- and this test
// going red is how that gets noticed rather than forgotten.
TEST(DifferentialTest, LineSplicingIsTheKnownDivergence) {
  const TempDir dir;
  const std::optional<ReferenceCompiler> compiler = findReferenceCompiler(dir.path());
  if (!compiler.has_value()) {
    GTEST_SKIP() << "no reference preprocessor (cc/gcc/clang) on this machine";
  }
  constexpr const char* kSource = "#define LONG a \\\n  b\nLONG\n";

  const std::optional<std::string> reference = referenceOutput(*compiler, "splice", kSource);
  ASSERT_TRUE(reference.has_value());
  // The standard reading: the splice makes the definition one line, so `LONG`
  // is `a b`.
  EXPECT_EQ(joined(spellings(*reference)), "a b");

  const PPOutcome ours = PPFixture().source(kSource).run();
  // Ours: the `\` is an invalid character, the definition ends there, and the
  // line below is an unrelated `b`. Deliberately wrong, deliberately recorded.
  EXPECT_EQ(joined(ours.spellings), "b a \\");
}

TEST(DifferentialTest, AgreesWithTheReferencePreprocessor) {
  const TempDir dir;
  const std::optional<ReferenceCompiler> compiler = findReferenceCompiler(dir.path());
  if (!compiler.has_value()) {
    GTEST_SKIP() << "no reference preprocessor (cc/gcc/clang) on this machine; set "
                    "MINC_REFERENCE_CC to run this check";
  }

  std::size_t checked = 0;
  for (const CorpusCase& testCase : kCorpus) {
    SCOPED_TRACE(testCase.name);
    const std::optional<std::string> reference =
        referenceOutput(*compiler, "case", testCase.source);
    ASSERT_TRUE(reference.has_value()) << "the reference compiler produced no output";

    const PPOutcome ours = PPFixture().source(testCase.source).run();
    // The corpus is standard C: this preprocessor must not merely agree with the
    // reference, it must have nothing to complain about.
    EXPECT_TRUE(ours.errors.empty()) << "our own diagnostics: " << joined(ours.messages);

    EXPECT_EQ(joined(ours.spellings), joined(spellings(*reference)));
    ++checked;
  }
  // A guard against the harness quietly covering nothing, which would look
  // exactly like success.
  EXPECT_EQ(checked, std::size(kCorpus));
}

} // namespace
} // namespace minc::test
