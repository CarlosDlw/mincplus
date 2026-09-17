// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Every parse error code must be reachable, and each must come from the input
// that is supposed to produce it.
//
// A code no input can produce is a diagnostic the user will never see; a code
// produced by the wrong construct is worse, because the message then lies about
// the cause. Both are caught here instead of by reading the grammar, and the
// table is checked for the same properties the lexer's flag table is.
#include <cstddef>
#include <set>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "parse/parse_error.h"
#include "parse/parse_fixture.h"
#include "parse/parse_report.h"
#include "parse/parser.h"
#include "support/diag/diag_bag.h"
#include "support/diag/diagnostic.h"

namespace minc::parse {
namespace {

using test::ParseFixture;

struct CodeCase {
  const char* label;
  const char* source;
  ParseErrorCode expected;
};

// One input per code, all deliberately tiny: when this fails, the input is the
// whole reproduction. The bail-out case is separate because it needs a big
// input rather than a small one.
constexpr CodeCase kCases[] = {
    {"missing semicolon", "fn i32 main() { let x = 1 }\n", ParseErrorCode::ExpectedToken},
    {"junk at top level", "%%%\n", ParseErrorCode::ExpectedItem},
    {"no type and no name", "fn () {}\n", ParseErrorCode::ExpectedName},
    {"name with no return type", "fn main() {}\n", ParseErrorCode::ExpectedType},
    {"type position with no type", "fn i32 main() { let x: = 1; }\n", ParseErrorCode::ExpectedType},
    {"let with no name", "fn i32 main() { let : i32 = 1; }\n", ParseErrorCode::ExpectedName},
    {"initializer with no expression", "fn i32 main() { let x = ; }\n",
     ParseErrorCode::ExpectedExpression},
    {"token that cannot start a statement", "fn i32 main() { , }\n",
     ParseErrorCode::ExpectedStatement},
    // The two halves of a parameter a reader can leave out. `(i32)` gives the
    // type and no name; `(x:)` gives the name and no type. Something that can
    // start neither is reported at the name, because the name is what the one
    // spelling the grammar has puts first (`name: type`); without a colon the
    // token cannot be the type half either.
    {"parameter with no name", "fn i32 main(i32) {}\n", ParseErrorCode::ExpectedName},
    {"parameter with no type", "fn i32 main(x:) {}\n", ParseErrorCode::ExpectedType},
    {"parameter with neither", "fn i32 main(; ) {}\n", ParseErrorCode::ExpectedName},
    // The two ways to write one of the two forms and mean the other. Each names
    // the word that is missing or misplaced, because the reader already knows
    // which function they meant and only needs to know how to spell it.
    {"body-less function with no `extern`", "fn i32 main();\n", ParseErrorCode::MissingExtern},
    {"`extern` with a body", "extern fn i32 main() { return 0; }\n",
     ParseErrorCode::ExternWithBody},
    // The variadic marker, in the two positions the grammar does not have. Both
    // are about *where* it is, and both name where it goes.
    {"`...` in a definition", "fn i32 f(a: i32, ...) { return a; }\n",
     ParseErrorCode::VariadicDefinition},
    {"`...` with no parameter before it", "extern fn i32 f(...);\n",
     ParseErrorCode::VariadicPosition},
    // The file scope: a binding is an item, and the two prefixes that do not fit
    // one are refused where the words are.
    {"`extern` on a binding", "extern let x: i32;\n", ParseErrorCode::ExternBinding},
    {"`static` before nothing that declares", "static x: i32;\n", ParseErrorCode::StaticPosition},
    {"`static` and `extern` on one declaration", "static extern fn i32 f();\n",
     ParseErrorCode::ConflictingLinkage},
    // Neither linker word has anything to say about a name for a type: the name
    // never reaches a linker, so there is no symbol to make internal and no
    // definition to say is elsewhere.
    {"`extern` on a type name", "extern type T = i32;\n", ParseErrorCode::TypeAliasLinkage},
    {"`static` on a type name", "static type T = i32;\n", ParseErrorCode::TypeAliasLinkage},
    // The two ways an `[N]` group is malformed. The count is a literal number, so
    // a name or an expression there is the first; a group with no `]` is the
    // second. `[]` -- nothing between the brackets -- is deliberately neither: it
    // is a slice, a complete type with no count to be malformed.
    {"array count that is not a number", "fn i32 main() { let x: [n]i32 = 1; }\n",
     ParseErrorCode::ExpectedArrayCount},
    {"array count with no closing bracket", "fn i32 main() { let x: [4 i32 = 1; }\n",
     ParseErrorCode::ExpectedArrayCountClose},
    // C's brace spelling, in the two positions it can appear in. The braces
    // belong to a *type* and a group of elements is a value, so the token is an
    // error with a sentence about which grouping is which -- and the group is read
    // anyway, so the reader gets one message and not a cascade out of a tree the
    // parser refused to build.
    {"braces for a value with the type annotated",
     "fn i32 main() { let a: [3]i32 = {1, 2, 3}; return 0; }\n", ParseErrorCode::BraceWithoutType},
    {"nested braces inside an initializer",
     "fn i32 main() { let a: [2][3]i16 = {{1, 2, 3}, {4, 5, 6}}; return 0; }\n",
     ParseErrorCode::BraceWithoutType},
    // A literal written against a name. The scanner claims a suffix only when it
    // is one the language knows (`suffix.h`), so `10z` arrives as two tokens --
    // and two tokens with nothing between them is the mistake, not a missing
    // operator. `10 z` is *not* this code: the space is the difference.
    {"a literal written against a name", "fn i32 main() { return 10z; }\n",
     ParseErrorCode::InvalidLiteralSuffix},
    {"a character literal with a suffix", "fn i32 main() { return 'a'u8; }\n",
     ParseErrorCode::InvalidLiteralSuffix},
};

[[nodiscard]] std::string deepInput() {
  // Twice the nesting limit, so the guard is what stops it rather than luck.
  return "fn i32 main() { return " + std::string(support::kMaxNestingDepth * 2U, '(') + "1; }\n";
}

[[nodiscard]] std::set<std::string> codesOf(const ParseFixture& fixture) {
  std::set<std::string> codes;
  for (const ParseError& error : fixture.tree().errors()) {
    codes.insert(std::string(error.codeName()));
  }
  return codes;
}

[[nodiscard]] std::string join(const std::set<std::string>& codes) {
  std::string out;
  for (const std::string& code : codes) {
    if (!out.empty()) {
      out += ", ";
    }
    out += code;
  }
  return out;
}

TEST(ParseErrorCodeTest, EachCaseProducesItsCode) {
  for (const CodeCase& testCase : kCases) {
    const ParseFixture fixture(testCase.source);
    ASSERT_TRUE(fixture.built()) << testCase.label;
    const std::set<std::string> codes = codesOf(fixture);
    EXPECT_EQ(codes.count(std::string(toString(testCase.expected))), 1u)
        << testCase.label << ": expected " << toString(testCase.expected) << ", got "
        << join(codes);
  }
}

TEST(ParseErrorCodeTest, DeepNestingBailsOut) {
  const ParseFixture fixture(deepInput());
  ASSERT_TRUE(fixture.built());
  EXPECT_TRUE(fixture.bailedOut());
  EXPECT_EQ(codesOf(fixture).count(std::string(toString(ParseErrorCode::Aborted))), 1u)
      << join(codesOf(fixture));
}

TEST(ParseErrorCodeTest, EveryCodeIsReachable) {
  std::set<std::string> seen;
  for (const CodeCase& testCase : kCases) {
    const ParseFixture fixture(testCase.source);
    seen.merge(codesOf(fixture));
  }
  seen.merge(codesOf(ParseFixture(deepInput())));

  for (const ParseErrorCode code : allParseErrorCodes()) {
    EXPECT_EQ(seen.count(std::string(toString(code))), 1u)
        << "no input produces " << toString(code) << "; add one to kCases";
  }
}

TEST(ParseErrorCodeTest, TableAndEnumAgree) {
  EXPECT_EQ(parseErrorCodeInfos().size(), allParseErrorCodes().size());

  for (const ParseErrorCode code : allParseErrorCodes()) {
    std::size_t rows = 0;
    for (const ParseErrorCodeInfo& info : parseErrorCodeInfos()) {
      if (info.code == code) {
        ++rows;
      }
    }
    EXPECT_EQ(rows, 1u) << "code " << static_cast<int>(code);
  }

  for (const ParseErrorCodeInfo& info : parseErrorCodeInfos()) {
    const std::string_view name(info.name);
    // Two codes sharing a name would make grep useless.
    std::size_t sameName = 0;
    for (const ParseErrorCodeInfo& other : parseErrorCodeInfos()) {
      if (std::string_view(other.name) == name) {
        ++sameName;
      }
    }
    EXPECT_EQ(sameName, 1u) << name;
    // Stable and grep-able: lowercase, hyphenated, and namespaced by stage so a
    // user can tell a syntax code from a lexical one at a glance.
    EXPECT_EQ(name.rfind("parse-", 0), 0u) << name;
    for (const char c : name) {
      EXPECT_TRUE((c >= 'a' && c <= 'z') || c == '-') << name;
    }
  }
}

// The code a user sees comes from the table, not from a second string built at
// the reporting site, so this pins the path from enum to rendered diagnostic.
TEST(ParseErrorCodeTest, ReportCarriesTheCodeName) {
  const ParseFixture fixture("fn i32 main() { let x = 1 }\n");
  ASSERT_GT(fixture.tree().errors().size(), 0u);

  support::DiagBag bag;
  const std::size_t added = reportParseErrors(fixture.tree().errors(), bag);
  EXPECT_EQ(added, fixture.tree().errors().size());
  ASSERT_EQ(bag.size(), fixture.tree().errors().size());
  for (std::size_t i = 0; i < bag.size(); ++i) {
    EXPECT_EQ(bag.all()[i].code, fixture.tree().errors()[i].codeName());
    EXPECT_EQ(bag.all()[i].message, fixture.tree().errors()[i].message);
    EXPECT_EQ(bag.all()[i].span.begin, fixture.tree().errors()[i].span.begin);
    EXPECT_EQ(bag.all()[i].span.end, fixture.tree().errors()[i].span.end);
  }
}

} // namespace
} // namespace minc::parse
