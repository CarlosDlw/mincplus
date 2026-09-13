// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
//
// The predefined macros, and the adapter that hands the preprocessed stream to
// the parser.
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "lex/token_kind.h"
#include "parse/token_source.h"
#include "pp/pp_token.h"
#include "pp/pp_token_source.h"

#include "pp_fixture.h"

namespace minc::test {
namespace {

TEST(BuiltinTest, FixedMacrosHaveFixedValues) {
  EXPECT_EQ(
      PPFixture().source("__STDC__ __STDC_VERSION__ __STDC_HOSTED__ __minc__\n").run().concat(),
      "1201710L11");
}

TEST(BuiltinTest, LineIsTheLineOfTheInvocation) {
  EXPECT_EQ(PPFixture().source("// one\n// two\n__LINE__\n").run().concat(), "3");
}

TEST(BuiltinTest, LineInsideAMacroIsTheLineOfTheUse) {
  // The classic bug: `__LINE__` inside a macro body must report the line the
  // macro was *used* on, not the line it was defined on.
  EXPECT_EQ(PPFixture().source("#define L __LINE__\n\nL\n").run().concat(), "3");
}

TEST(BuiltinTest, FileIsThePathTheDriverWasGiven) {
  EXPECT_EQ(PPFixture("dir/main.mx").source("__FILE__\n").run().concat(), "\"dir/main.mx\"");
}

TEST(BuiltinTest, LineDirectiveCanRenameTheFile) {
  // `#line` can rename the file for the rest of the unit, which is how generated
  // code points diagnostics at its source.
  const PPOutcome out = PPFixture().source("#line 1 \"generated.mx\"\n__FILE__\n").run();
  EXPECT_EQ(out.concat(), "\"generated.mx\"");
}

TEST(BuiltinTest, CounterCountsInvocations) {
  EXPECT_EQ(PPFixture().source("__COUNTER__ __COUNTER__ __COUNTER__\n").run().concat(), "012");
}

TEST(BuiltinTest, CounterCountsPerUnitNotPerLine) {
  EXPECT_EQ(PPFixture().source("#define C __COUNTER__\nC\nC\n").run().concat(), "01");
}

TEST(BuiltinTest, DateWithoutEpochIsAnError) {
  // A build that silently embeds the current time cannot be reproduced, so this
  // is an error with the fix in the message rather than a value.
  EXPECT_TRUE(PPFixture().source("__DATE__\n").run().hasError("pp-date-without-epoch"));
  EXPECT_TRUE(PPFixture().source("__TIME__\n").run().hasError("pp-date-without-epoch"));
}

TEST(BuiltinTest, DateAndTimeComeFromTheEpoch) {
  const PPOutcome out = PPFixture().source("__DATE__ __TIME__\n").epoch(1000000000).run();
  EXPECT_EQ(out.concat(), "\"Sep  9 2001\"\"01:46:40\"");
}

TEST(BuiltinTest, DateIsStableForTheSameEpoch) {
  // The determinism claim, stated as a test: the same input and the same epoch
  // give the same bytes, whatever the clock and timezone say.
  const std::string first = PPFixture().source("__DATE__\n").epoch(0).run().concat();
  const std::string second = PPFixture().source("__DATE__\n").epoch(0).run().concat();
  EXPECT_EQ(first, "\"Jan  1 1970\"");
  EXPECT_EQ(first, second);
}

TEST(BuiltinTest, HasIncludeAnswersAboutTheRealSearchList) {
  const TempDir dir;
  dir.write("here.h", "#define HERE 1\n");
  PPFixture fixture(dir.path() + "/main.mx");
  fixture.source("#if __has_include(\"here.h\")\nyes\n#else\nno\n#endif\n"
                 "#if __has_include(\"absent.h\")\nbad\n#else\nok\n#endif\n");
  const PPOutcome out = fixture.run();
  EXPECT_TRUE(out.errors.empty());
  EXPECT_EQ(out.concat(), "yesok");
}

TEST(BuiltinTest, HasIncludeAnswersAboutExistenceNotReadability) {
  // The header is there and the path is right; what cannot be used is its
  // bytes. `__has_include` asks the filesystem question, because that is the
  // question the guarded `#include` needs answered. An implementation that read
  // the file to answer would take the `#else` branch and the real error -- the
  // one the `#include` would have raised -- would never be seen.
  const TempDir dir;
  dir.write("bogus.h", std::string("\xFF\xFE", 2));
  const PPOutcome out = PPFixture()
                            .source("#if __has_include(<bogus.h>)\nyes\n#else\nno\n#endif\n")
                            .includeDir(dir.path())
                            .run();
  EXPECT_TRUE(out.errors.empty());
  EXPECT_EQ(out.concat(), "yes");
}

TEST(BuiltinTest, HasIncludeOutsideIfIsDiagnosed) {
  // It is an operator, not a macro with a value; pretending otherwise is how a
  // header ends up with a nonsense value baked into it.
  EXPECT_TRUE(
      PPFixture().source("__has_include(\"x.h\")\n").run().hasError("pp-expression-syntax"));
}

TEST(BuiltinTest, RedefiningABuiltinNeedsTheBuiltinToExist) {
  EXPECT_TRUE(PPFixture().source("#ifdef __FILE__\nyes\n#endif\n").run().concat() == "yes");
}

// --- the parser-facing adapter ---------------------------------------------

TEST(PPTokenSourceTest, WalksThePreprocessedStream) {
  // The adapter is over `PPToken`s, so the test builds the tokens by hand --
  // including the whitespace the preprocessed stream really carries, because
  // skipping it is the adapter's job and a test without it would not test the
  // rule the parser depends on. `1` and `+` with a space between them.
  std::vector<pp::PPToken> tokens(3);
  tokens[0].kind = lex::TokenKind::IntegerLiteral;
  tokens[0].length = 1;
  tokens[1].kind = lex::TokenKind::Whitespace;
  tokens[1].length = 1;
  tokens[2].kind = lex::TokenKind::Plus;
  tokens[2].length = 1;

  const std::unique_ptr<parse::TokenSource> source = pp::makePPTokenSource(tokens);
  EXPECT_EQ(source->current(), lex::TokenKind::IntegerLiteral);
  EXPECT_EQ(source->nth(1), lex::TokenKind::Plus);
  // Past the end is `EndOfFile` rather than out of bounds: the clamp is the
  // contract, because lookahead past the end happens on every parser error path.
  EXPECT_EQ(source->nth(9), lex::TokenKind::EndOfFile);
  EXPECT_FALSE(source->atEnd());
  source->bump();
  EXPECT_EQ(source->current(), lex::TokenKind::Plus);
  source->bump();
  EXPECT_TRUE(source->atEnd());
  EXPECT_EQ(source->current(), lex::TokenKind::EndOfFile);
  // A second bump is a no-op rather than an overrun.
  source->bump();
  EXPECT_TRUE(source->atEnd());
}

} // namespace
} // namespace minc::test
