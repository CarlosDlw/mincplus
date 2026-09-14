// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The conversions: promotion, the usual arithmetic conversions, assignability,
// narrowing, and the range check a deferred literal gets. These are the rules
// that decide what a program computes, so they are pinned directly rather than
// only through an input that happens to exercise them.
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

#include "sema/convert.h"
#include "sema/target.h"
#include "sema/type.h"
#include "sema/type_store.h"
#include "support/consteval/const_int.h"

namespace minc::sema {
namespace {

TEST(ConvertTest, TheBottomTypeConvertsIntoEverything) {
  TypeStore types;
  const TypeId pointer = types.pointerTo(kTypeI32);
  const TypeId fn = types.function(kTypeVoid, {}, /*variadic=*/false);

  // `!` converts into every type there is, and the list is deliberately the
  // *whole* list rather than the arithmetic ones: there is no value to be wrong
  // about, so there is nothing for a target type to be incompatible with. This is
  // what makes `let x: i32 = die();` legal and `c ? 1 : die()` an `i32`.
  for (const TypeId target :
       {kTypeI32, kTypeU8, kTypeF64, kTypeBool, kTypeStr, kTypeChar, kTypeVoid, pointer, fn}) {
    EXPECT_TRUE(convertible(types, kTypeNever, target)) << types.spelling(target);
  }
  // And nothing converts *into* it: a type that admits no values cannot be
  // arrived at, so a value that is not a `!` cannot become one.
  for (const TypeId source : {kTypeI32, kTypeBool, kTypeStr, kTypeVoid, pointer}) {
    EXPECT_FALSE(convertible(types, source, kTypeNever)) << types.spelling(source);
  }
  // `!` to `!` is the identity, and narrows nothing: there is no width to lose.
  EXPECT_TRUE(convertible(types, kTypeNever, kTypeNever));
  EXPECT_FALSE(narrows(types, kTypeNever, kTypeI32));
}

TEST(ConvertTest, PromotionWidensTheSmallIntegersToInt) {
  TypeStore types;
  EXPECT_EQ(promote(types, kTypeBool), kTypeI32);
  EXPECT_EQ(promote(types, kTypeChar), kTypeI32);
  EXPECT_EQ(promote(types, kTypeI8), kTypeI32);
  EXPECT_EQ(promote(types, kTypeU16), kTypeI32);
  // Already at or above `int`: unchanged.
  EXPECT_EQ(promote(types, kTypeI32), kTypeI32);
  EXPECT_EQ(promote(types, kTypeU32), kTypeU32);
  EXPECT_EQ(promote(types, kTypeI64), kTypeI64);
  EXPECT_EQ(promote(types, kTypeF64), kTypeF64);
  // A deferred literal stays deferred: its width is decided by its context.
  EXPECT_EQ(promote(types, kTypeIntLiteral), kTypeIntLiteral);
}

TEST(ConvertTest, TwoLiteralsStayDeferredAndOnlyTheirClassIsDecided) {
  TypeStore types;
  EXPECT_EQ(usualArithmetic(types, kTypeIntLiteral, kTypeIntLiteral), kTypeIntLiteral);
  EXPECT_EQ(usualArithmetic(types, kTypeIntLiteral, kTypeFloatLiteral), kTypeFloatLiteral);
  EXPECT_EQ(usualArithmetic(types, kTypeFloatLiteral, kTypeIntLiteral), kTypeFloatLiteral);
}

TEST(ConvertTest, ALiteralAdoptsTheOtherSidePromoted) {
  TypeStore types;
  // An integer side is taken promoted, so `1 + u8` is `i32` and not `u8`.
  EXPECT_EQ(usualArithmetic(types, kTypeIntLiteral, kTypeU8), kTypeI32);
  EXPECT_EQ(usualArithmetic(types, kTypeU8, kTypeIntLiteral), kTypeI32);
  // A float on either side wins, and it wins *narrowly*: `1 + f32` is `f32`.
  EXPECT_EQ(usualArithmetic(types, kTypeIntLiteral, kTypeF32), kTypeF32);
  EXPECT_EQ(usualArithmetic(types, kTypeF32, kTypeIntLiteral), kTypeF32);
}

TEST(ConvertTest, TheUsualArithmeticConversions) {
  TypeStore types;
  // Same signedness: the wider one.
  EXPECT_EQ(usualArithmetic(types, kTypeI32, kTypeI64), kTypeI64);
  EXPECT_EQ(usualArithmetic(types, kTypeU8, kTypeU32), kTypeU32);
  // Equal rank, different signedness: the unsigned one wins.
  EXPECT_EQ(usualArithmetic(types, kTypeI32, kTypeU32), kTypeU32);
  // A wider signed type represents every value of the unsigned one, so it wins.
  EXPECT_EQ(usualArithmetic(types, kTypeU32, kTypeI64), kTypeI64);
  // Floats: the wider of the two, and an integer converts to the float.
  EXPECT_EQ(usualArithmetic(types, kTypeF32, kTypeF64), kTypeF64);
  EXPECT_EQ(usualArithmetic(types, kTypeI64, kTypeF32), kTypeF32);
  // The small ones are promoted first, so this is `i32`, not `u16`.
  EXPECT_EQ(usualArithmetic(types, kTypeU16, kTypeI16), kTypeI32);
}

TEST(ConvertTest, BoolAndStrAreNotArithmetic) {
  TypeStore types;
  // The deliberate departure from C: `bool` does not become `int`, and a `str`
  // is a scalar that is not a number.
  EXPECT_EQ(usualArithmetic(types, kTypeBool, kTypeBool), kTypeError);
  EXPECT_EQ(usualArithmetic(types, kTypeStr, kTypeStr), kTypeError);
  EXPECT_EQ(usualArithmetic(types, kTypeBool, kTypeI32), kTypeError);
  // The poison spreads without a comment.
  EXPECT_EQ(usualArithmetic(types, kTypeError, kTypeI32), kTypeError);
}

TEST(ConvertTest, Assignability) {
  TypeStore types;
  EXPECT_TRUE(convertible(types, kTypeI32, kTypeI64));
  EXPECT_TRUE(convertible(types, kTypeI64, kTypeI32)); // narrowing is allowed...
  EXPECT_TRUE(convertible(types, kTypeI32, kTypeF64));
  EXPECT_TRUE(convertible(types, kTypeF64, kTypeI32));
  EXPECT_TRUE(convertible(types, kTypeI32, kTypeU8));
  // ... but `bool` and `str` only convert to themselves.
  EXPECT_TRUE(convertible(types, kTypeBool, kTypeBool));
  EXPECT_FALSE(convertible(types, kTypeI32, kTypeBool));
  EXPECT_FALSE(convertible(types, kTypeBool, kTypeI32));
  EXPECT_TRUE(convertible(types, kTypeStr, kTypeStr));
  EXPECT_FALSE(convertible(types, kTypeStr, kTypeI32));
  // `void` is not a value anywhere.
  EXPECT_FALSE(convertible(types, kTypeI32, kTypeVoid));
  EXPECT_FALSE(convertible(types, kTypeVoid, kTypeI32));
  // The poison converts silently: the mistake is elsewhere and already reported.
  EXPECT_TRUE(convertible(types, kTypeError, kTypeI32));
  EXPECT_TRUE(convertible(types, kTypeI32, kTypeError));
}

TEST(ConvertTest, NarrowingIsWhatTheLintReads) {
  TypeStore types;
  EXPECT_TRUE(narrows(types, kTypeI32, kTypeI8));
  EXPECT_TRUE(narrows(types, kTypeI32, kTypeU32)); // signed to unsigned of equal width
  EXPECT_TRUE(narrows(types, kTypeF64, kTypeI32)); // truncation
  EXPECT_TRUE(narrows(types, kTypeF64, kTypeF32));
  EXPECT_FALSE(narrows(types, kTypeI8, kTypeI32));
  EXPECT_FALSE(narrows(types, kTypeU8, kTypeI32));
  EXPECT_FALSE(narrows(types, kTypeI32, kTypeF64));
  EXPECT_FALSE(narrows(types, kTypeI32, kTypeI32));
  // A deferred literal is not a narrowing: its range is checked as an error.
  EXPECT_FALSE(narrows(types, kTypeIntLiteral, kTypeU8));
}

TEST(ConvertTest, DoesTheValueFit) {
  TypeStore types;
  EXPECT_TRUE(fitsIn(types, kTypeU8, support::ConstInt::fromSigned(255)));
  EXPECT_FALSE(fitsIn(types, kTypeU8, support::ConstInt::fromSigned(256)));
  EXPECT_FALSE(fitsIn(types, kTypeU8, support::ConstInt::fromSigned(-1)));
  EXPECT_TRUE(fitsIn(types, kTypeI8, support::ConstInt::fromSigned(-128)));
  EXPECT_TRUE(fitsIn(types, kTypeI8, support::ConstInt::fromSigned(127)));
  EXPECT_FALSE(fitsIn(types, kTypeI8, support::ConstInt::fromSigned(128)));
  EXPECT_FALSE(fitsIn(types, kTypeI8, support::ConstInt::fromSigned(-129)));
  EXPECT_TRUE(fitsIn(types, kTypeBool, support::ConstInt::fromSigned(1)));
  EXPECT_FALSE(fitsIn(types, kTypeBool, support::ConstInt::fromSigned(2)));
  EXPECT_TRUE(fitsIn(types, kTypeChar, support::ConstInt::fromSigned(255)));
  EXPECT_FALSE(fitsIn(types, kTypeChar, support::ConstInt::fromSigned(300)));
  // Beyond the 64-bit core the answer would be a guess, and the caller has
  // already refused a literal that did not fit the core.
  EXPECT_TRUE(fitsIn(types, kTypeI128, support::ConstInt::fromSigned(-1)));
}

TEST(ConvertTest, IntegerDivisionAndShiftRefuseWhatHasNoValue) {
  const support::ConstInt seven = support::ConstInt::fromSigned(7);
  const support::ConstInt zero = support::ConstInt::fromSigned(0);
  EXPECT_TRUE(support::divide(seven, zero).has_value() == false);
  EXPECT_TRUE(support::remainder(seven, zero).has_value() == false);
  // A shift at or past the width is the one shift condition that is not a value.
  EXPECT_FALSE(support::shiftLeft(seven, support::ConstInt::fromSigned(64)).has_value());
  EXPECT_TRUE(support::shiftLeft(seven, support::ConstInt::fromSigned(63)).has_value());
}

TEST(ConvertTest, SignedDivisionIsSigned) {
  // `-7 / 2` truncates toward zero, which is only true if the dividend is read
  // as a negative number and not as its bit pattern.
  const std::optional<support::ConstInt> quotient =
      support::divide(support::ConstInt::fromSigned(-7), support::ConstInt::fromSigned(2));
  ASSERT_TRUE(quotient.has_value());
  EXPECT_EQ(quotient->signedValue(), -3);
  EXPECT_FALSE(quotient->isUnsigned);
}

} // namespace
} // namespace minc::sema
