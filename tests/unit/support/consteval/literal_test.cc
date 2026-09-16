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

} // namespace
} // namespace minc::support
