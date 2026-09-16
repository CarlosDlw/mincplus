// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The suffix table and the spelling readers: the one place a literal's type and
// value are decided before the checker sees them (`casts.md`).
//
// Three stages read this table -- the scanner (how long is the token), the reader
// (what is the number) and the checker (what is the type) -- so the tests here are
// about *agreement* as much as about values: a spelling the table lists has to be
// claimed by the scanner, split by the reader, and classified as the type the
// table names, or the three have stopped reading one table.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "support/consteval/literal.h"
#include "support/consteval/suffix.h"

namespace minc::support {
namespace {

TEST(ConstevalLiteralTest, EverySpellingInTheTableClassifiesAsItself) {
  // The walk the table's own comment promises: a suffix added to the language is
  // a row here, and a row here is a spelling `classifySuffix` answers for. Both
  // directions, because either one alone can be satisfied by a spelling nobody
  // can write.
  for (const SuffixSpelling& spelling : allSuffixSpellings()) {
    const LiteralSuffix suffix = classifySuffix(spelling.spelling, spelling.floatLiteral);
    EXPECT_EQ(suffix.status, SuffixStatus::Typed) << spelling.spelling;
    EXPECT_TRUE(suffix.typed()) << spelling.spelling;
    EXPECT_NE(suffix.type, SuffixType::None) << spelling.spelling;
    EXPECT_FALSE(toString(suffix.type).empty()) << spelling.spelling;
  }
}

TEST(ConstevalLiteralTest, BitPreciseSuffixesAreRefusedByName) {
  // C23's `wb`/`uwb` name a `_BitInt`, which this language does not have. They are
  // *known* -- the run is claimed into the token -- so the sentence can name the
  // type to write instead, which is the difference between a reader learning what
  // to type and one being told "invalid".
  for (const std::string_view spelling : {"wb", "uwb"}) {
    const LiteralSuffix suffix = classifySuffix(spelling, /*literalIsFloat=*/false);
    ASSERT_EQ(suffix.status, SuffixStatus::Refused) << spelling;
    EXPECT_NE(suffix.message.find("i64"), std::string::npos) << suffix.message;
  }
  // And they are *not* suffixes `suffixLengthAt` leaves alone...
  EXPECT_EQ(suffixLengthAt("10wb", 2, false), 2U);
  // ...while an unknown run is not a suffix at all, which is the rule that keeps
  // `1else` two tokens.
  EXPECT_EQ(suffixLengthAt("10z", 2, false), 0U);
  EXPECT_EQ(classifySuffix("z", false).status, SuffixStatus::None);
}

TEST(ConstevalLiteralTest, AFloatSuffixOnAnIntegerSpellingMakesItAFloat) {
  // Rust's rule rather than C's (`casts.md`, decision 10): `12f` is `12.0` as an
  // `f32`, and the value is exact -- there is no rounding for the suffix to be
  // wrong about.
  const LiteralSuffix suffix = classifySuffix("f", /*literalIsFloat=*/false);
  ASSERT_TRUE(suffix.typed());
  EXPECT_TRUE(suffix.makesFloat);
  EXPECT_EQ(suffix.type, SuffixType::F32);
  // `L` is the other way round: `long` on an integer, `long double` on a float.
  EXPECT_EQ(classifySuffix("L", /*literalIsFloat=*/false).type, SuffixType::CLong);
  EXPECT_EQ(classifySuffix("L", /*literalIsFloat=*/true).type, SuffixType::CLongDouble);
  // An *integer* suffix on a float is refused rather than read: a `1.5u8` has no
  // meaning to have.
  EXPECT_EQ(classifySuffix("u8", /*literalIsFloat=*/true).status, SuffixStatus::Refused);
}

TEST(ConstevalLiteralTest, ASuffixedIntegerIsOneLiteralWithTheTypeItsSuffixNames) {
  const IntegerLiteral ten = parseIntegerLiteral("10u8", IntegerBaseRule::DecimalLeadingZero);
  ASSERT_TRUE(ten.ok) << ten.message;
  EXPECT_EQ(ten.number, "10");
  EXPECT_EQ(ten.suffix.type, SuffixType::U8);
  EXPECT_EQ(ten.value.bits, 10U);
  EXPECT_TRUE(ten.value.isUnsigned);

  const IntegerLiteral hex = parseIntegerLiteral("0xFFusize", IntegerBaseRule::DecimalLeadingZero);
  ASSERT_TRUE(hex.ok) << hex.message;
  // The number keeps its prefix and loses its suffix: what a wider reader needs,
  // and the split belongs to the reader that knows where the digits stop.
  EXPECT_EQ(hex.number, "0xFF");
  EXPECT_EQ(hex.value.bits, 255U);
  EXPECT_EQ(hex.suffix.type, SuffixType::Usize);

  const IntegerLiteral binary = parseIntegerLiteral("0b1010", IntegerBaseRule::DecimalLeadingZero);
  ASSERT_TRUE(binary.ok) << binary.message;
  EXPECT_EQ(binary.number, "0b1010");
  EXPECT_EQ(binary.value.bits, 10U);
  EXPECT_EQ(binary.suffix.status, SuffixStatus::None);
}

TEST(ConstevalLiteralTest, AnIntegerLiteralWiderThanTheCoreIsTooWideAndNotMalformed) {
  // The two are kept apart on purpose: `i128` has a value for this spelling, and
  // the reader that *can* hold it (`src/ir`) asks for the digits instead. A reader
  // that reported one code for both would make a valid `i128` literal look like a
  // typo.
  const IntegerLiteral wide = parseIntegerLiteral("170141183460469231731687303715884105727",
                                                  IntegerBaseRule::DecimalLeadingZero);
  EXPECT_FALSE(wide.ok);
  EXPECT_TRUE(wide.tooWide);
  EXPECT_EQ(wide.number, "170141183460469231731687303715884105727");
  EXPECT_FALSE(wide.message.empty());

  // An unknown run is a malformed spelling and not a suffix: it is a mistake about
  // the literal, and it says so with the byte that started it.
  const IntegerLiteral unknown = parseIntegerLiteral("10z", IntegerBaseRule::DecimalLeadingZero);
  EXPECT_FALSE(unknown.ok);
  EXPECT_FALSE(unknown.tooWide);
  EXPECT_NE(unknown.message.find("10z"), std::string::npos) << unknown.message;
}

TEST(ConstevalLiteralTest, AFloatReadersNumberIsTheNumberAndNotTheToken) {
  // The whole point of the split: `APFloat`'s reader is handed a number, and a
  // trailing `f32` is not one. `1.5f32` is one token and two fields.
  const FloatLiteral withSuffix = readFloatLiteral("1.5f32");
  EXPECT_EQ(withSuffix.number, "1.5");
  EXPECT_EQ(withSuffix.suffix.type, SuffixType::F32);

  const FloatLiteral plain = readFloatLiteral("1.5");
  EXPECT_EQ(plain.number, "1.5");
  EXPECT_EQ(plain.suffix.status, SuffixStatus::None);

  // The exponent and the fraction are part of the number, and the suffix is not.
  const FloatLiteral exponent = readFloatLiteral("1.5e3f64");
  EXPECT_EQ(exponent.number, "1.5e3");
  EXPECT_EQ(exponent.suffix.type, SuffixType::F64);

  // A fraction with no integer part is split the same way: the point is part of
  // the number, and `.5e3` is one number and not `.5` and a name.
  const FloatLiteral leadingPoint = readFloatLiteral(".5e3");
  EXPECT_EQ(leadingPoint.number, ".5e3");
  EXPECT_EQ(leadingPoint.suffix.status, SuffixStatus::None);
  const FloatLiteral leadingPointSuffix = readFloatLiteral(".5f32");
  EXPECT_EQ(leadingPointSuffix.number, ".5");
  EXPECT_EQ(leadingPointSuffix.suffix.type, SuffixType::F32);

  const FloatLiteral hex = readFloatLiteral("0x1.8p1");
  EXPECT_EQ(hex.number, "0x1.8p1");
  EXPECT_EQ(hex.suffix.status, SuffixStatus::None);

  // `L` is `long double` here, and *which* `long double` is the target's answer --
  // which is why this reader names the descriptor and the checker resolves it.
  const FloatLiteral longDouble = readFloatLiteral("1.5L");
  EXPECT_EQ(longDouble.number, "1.5");
  EXPECT_EQ(longDouble.suffix.type, SuffixType::CLongDouble);
  EXPECT_TRUE(isTargetDependent(longDouble.suffix.type));
}

TEST(ConstevalLiteralTest, CaseIsNotPartOfASuffixesMeaning) {
  // `10U` and `10u`, `1.5F` and `1.5f`, `0xFFUL` and `0xFFul`: C's rule, and the
  // reason the two-letter combinations are classified rather than listed twenty
  // times.
  const std::vector<std::pair<std::string_view, std::string_view>> integers{
      {"10u", "10U"},       {"10l", "10L"},   {"10ul", "10UL"},
      {"0x1ful", "0x1fUL"}, {"10ll", "10LL"}, {"10ull", "10ULL"},
  };
  for (const auto& pair : integers) {
    const IntegerLiteral first =
        parseIntegerLiteral(pair.first, IntegerBaseRule::DecimalLeadingZero);
    const IntegerLiteral second =
        parseIntegerLiteral(pair.second, IntegerBaseRule::DecimalLeadingZero);
    ASSERT_TRUE(first.ok) << pair.first << ": " << first.message;
    ASSERT_TRUE(second.ok) << pair.second << ": " << second.message;
    EXPECT_EQ(first.suffix.type, second.suffix.type) << pair.first << " vs " << pair.second;
    EXPECT_EQ(first.value.bits, second.value.bits) << pair.first;
  }
  // And the float spellings, read by the float reader: `f` and `F` are one suffix.
  EXPECT_EQ(readFloatLiteral("1.5f").suffix.type, readFloatLiteral("1.5F").suffix.type);
}

TEST(ConstevalLiteralTest, ASeparatorIsSpellingAndNotAValue) {
  // The separator is removed before the value is read, so it cannot change what a
  // literal means -- and the *number* keeps it, because the number is the
  // spelling's own text and the one consumer that hands digits to a value reader
  // strips them itself (`literals.md`, decisions 1 and 10).
  struct Case {
    std::string_view with;
    std::string_view without;
  };
  const Case cases[] = {
      {"1_000", "1000"},     {"1'000", "1000"},
      {"0xFE'DC", "0xFEDC"}, {"0b1111_0000", "0b11110000"},
      {"0o7_5_5", "0o755"},  {"1_2_3_4u8", "1234u8"},
  };
  for (const Case& testCase : cases) {
    const IntegerLiteral with =
        parseIntegerLiteral(testCase.with, IntegerBaseRule::DecimalLeadingZero);
    const IntegerLiteral without =
        parseIntegerLiteral(testCase.without, IntegerBaseRule::DecimalLeadingZero);
    ASSERT_TRUE(with.ok) << testCase.with << ": " << with.message;
    ASSERT_TRUE(without.ok) << testCase.without << ": " << without.message;
    EXPECT_EQ(with.value.bits, without.value.bits) << testCase.with;
    EXPECT_EQ(with.value.isUnsigned, without.value.isUnsigned) << testCase.with;
    EXPECT_EQ(with.suffix.type, without.suffix.type) << testCase.with;
    // The number keeps the spelling's separators, and removing them is what gives
    // the plain spelling's number: the two describe one value.
    EXPECT_EQ(withoutSeparators(with.number), without.number) << testCase.with;
  }
}

// One sentence for every way a separator can be wrong, and it is the *placement*
// that is refused -- never a silently different number.
TEST(ConstevalLiteralTest, AMisplacedSeparatorIsRefusedWithItsPlacement) {
  for (const std::string_view spelling : {"1000_", "_1000", "1__0", "0x_FF", "10_u8", "1_e3"}) {
    const IntegerLiteral parsed =
        parseIntegerLiteral(spelling, IntegerBaseRule::DecimalLeadingZero);
    EXPECT_FALSE(parsed.ok) << spelling;
    EXPECT_FALSE(parsed.tooWide) << spelling;
    EXPECT_NE(parsed.message.find("separator"), std::string::npos)
        << spelling << ": " << parsed.message;
  }
  EXPECT_TRUE(parseIntegerLiteral("1_000", IntegerBaseRule::DecimalLeadingZero).ok);
}

TEST(ConstevalLiteralTest, TheSeparatorStripperIsWhatTheValueReadersGet) {
  EXPECT_EQ(withoutSeparators("1_000"), "1000");
  EXPECT_EQ(withoutSeparators("0xFE'DC"), "0xFEDC");
  EXPECT_EQ(withoutSeparators("1.414_213"), "1.414213");
  EXPECT_EQ(withoutSeparators("1e1_0"), "1e10");
  EXPECT_EQ(withoutSeparators("123"), "123");
  // The float split keeps the separators (they are the number's), and the reader
  // that wants a number asks for them to go.
  EXPECT_EQ(withoutSeparators(readFloatLiteral("1.000_5").number), "1.0005");
  EXPECT_EQ(withoutSeparators(readFloatLiteral("0xF_Fp1_0").number), "0xFFp10");
}

// Every row of the escape table, decoded to bytes. The byte escapes are bytes,
// the code point escapes are UTF-8, and the delimited forms are what says where a
// run of digits ends.
TEST(ConstevalLiteralTest, TheEscapeAlphabetDecodesToBytes) {
  struct Case {
    const char* spelling;
    std::vector<std::uint8_t> bytes;
  };
  const Case cases[] = {
      {"\"\\a\"", {7}},
      {"\"\\b\"", {8}},
      {"\"\\e\"", {27}},
      {"\"\\f\"", {12}},
      {"\"\\n\"", {10}},
      {"\"\\r\"", {13}},
      {"\"\\t\"", {9}},
      {"\"\\v\"", {11}},
      {"\"\\?\"", {'?'}},
      {"\"\\'\"", {'\''}},
      {"\"\\\\\"", {'\\'}},
      {"\"\\0\"", {0}},
      {"\"\\101\"", {'A'}},
      {"\"\\377\"", {255}},
      {"\"\\x41\"", {'A'}},
      {"\"\\x{41}\"", {'A'}},
      {"\"\\xFF\"", {255}},
      {"\"\\o{101}\"", {'A'}},
      {"\"\\o{7}\"", {7}},
      // C's run length is kept: `\x041` is one escape whose value is `0x41`, and
      // the delimited form is how the run is stopped instead.
      {"\"\\x041\"", {'A'}},
      {"\"\\x{41}B\"", {'A', 'B'}},
      {"\"\\1012\"", {'A', '2'}},
      {"\"\\u0041\"", {'A'}},
      {"\"\\u{e9}\"", {0xC3, 0xA9}},
      {"\"\\U0001F600\"", {0xF0, 0x9F, 0x98, 0x80}},
      {"\"\\u{1F600}\"", {0xF0, 0x9F, 0x98, 0x80}},
      {"\"\\U{1F600}\"", {0xF0, 0x9F, 0x98, 0x80}},
  };
  for (const Case& testCase : cases) {
    const StringLiteral parsed = parseStringLiteral(testCase.spelling);
    ASSERT_TRUE(parsed.ok) << testCase.spelling << ": " << parsed.message;
    EXPECT_EQ(parsed.bytes, testCase.bytes) << testCase.spelling;
  }
}

TEST(ConstevalLiteralTest, ABackslashBeforeALineEndingContributesNothing) {
  // C splices it before tokens exist and Rust spells it as an escape; either way
  // the bytes are the ones on both sides of the ending, and LF and CRLF are one
  // rule (`literals.md`, decision 18).
  const StringLiteral lf = parseStringLiteral("\"a\\\nb\"");
  ASSERT_TRUE(lf.ok) << lf.message;
  EXPECT_EQ(lf.bytes, (std::vector<std::uint8_t>{'a', 'b'}));
  const StringLiteral crlf = parseStringLiteral("\"a\\\r\nb\"");
  ASSERT_TRUE(crlf.ok) << crlf.message;
  EXPECT_EQ(crlf.bytes, (std::vector<std::uint8_t>{'a', 'b'}));
}

TEST(ConstevalLiteralTest, TheEscapesThatAreRefusedSayWhy) {
  struct Case {
    const char* spelling;
    const char* wanted;
  };
  const Case cases[] = {
      {"\"\\q\"", "not an escape"},
      {"\"\\x\"", "needs at least one hex digit"},
      {"\"\\o\"", "needs at least one octal digit"},
      {"\"\\x{}\"", "between its braces"},
      {"\"\\x{41\"", "between its braces"},
      {"\"\\u{}\"", "between its braces"},
      {"\"\\u12\"", "exactly 4 hex digits"},
      {"\"\\U1\"", "exactly 8 hex digits"},
      {"\"\\uD800\"", "does not name a character"},
      {"\"\\U00110000\"", "does not name a character"},
      // A *string* refuses an escape above a byte, because it has no byte for it;
      // a character literal hands the same value to the checker, which owns the
      // type rule and its sentence (`literals.md`, decision 14).
      {"\"\\x1FF\"", "wider than one byte"},
      // `\x41B` is one escape and not `A` and `B`: the run does not stop at a byte
      // boundary, which is exactly what the braces are for.
      {"\"\\x41B\"", "wider than one byte"},
      {"\"\\400\"", "wider than one byte"},
      {"\"\\x{1F600}\"", "wider than one byte"},
      {"\"\\N{GREEK SMALL LETTER ALPHA}\"", "name table"},
  };
  for (const Case& testCase : cases) {
    const StringLiteral parsed = parseStringLiteral(testCase.spelling);
    EXPECT_FALSE(parsed.ok) << testCase.spelling;
    EXPECT_NE(parsed.message.find(testCase.wanted), std::string::npos)
        << testCase.spelling << ": " << parsed.message;
  }
}

// The unit count is what lets the checker state "a `char` is one byte" without
// reading the spelling a second time, and the packed value is what the
// preprocessor compares a C `#if` against (`literals.md`, decision 20).
TEST(ConstevalLiteralTest, ACharacterLiteralCountsItsUnits) {
  EXPECT_EQ(parseCharLiteral("'a'").units, 1u);
  EXPECT_EQ(parseCharLiteral("'\\n'").units, 1u);
  EXPECT_EQ(parseCharLiteral("'\\u{e9}'").units, 1u);
  // One unit, and above a byte: the value is handed over and the *checker*
  // refuses it, because a `char` is the language's rule and not the reader's.
  const CharLiteral wide = parseCharLiteral("'\\u{1F600}'");
  ASSERT_TRUE(wide.ok) << wide.message;
  EXPECT_EQ(wide.units, 1u);
  EXPECT_EQ(wide.value.bits, 0x1F600U);
  // Two units, in both spellings a reader can be handed.
  EXPECT_EQ(parseCharLiteral("'ab'").units, 2u);
  EXPECT_EQ(parseCharLiteral("'\\xC3\\xA9'").units, 2u);
  EXPECT_EQ(parseCharLiteral("'ab'").value.bits, 0x6162U);
  // `'\<newline>a'` is `'a'`: a continuation is no unit at all.
  const CharLiteral continued = parseCharLiteral("'\\\na'");
  ASSERT_TRUE(continued.ok) << continued.message;
  EXPECT_EQ(continued.units, 1u);
  EXPECT_EQ(continued.value.bits, static_cast<std::uint64_t>('a'));
  // And the alphabet is the string alphabet, so an escape that is refused in one
  // is refused in the other.
  EXPECT_FALSE(parseCharLiteral("'\\q'").ok);
}

} // namespace
} // namespace minc::support
