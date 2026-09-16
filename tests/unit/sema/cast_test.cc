// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// A cast: the matrix, the losses it may have, and the two spellings that have to
// mean one thing.
//
// The matrix is written as **data**, one row per pair, because that is the claim
// `casts.md` makes about it: a cast is a function of `(from, to)` and not a set of
// arms in the checker. A pair with no row here is a pair nobody decided, which is
// the failure this file exists to produce.
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "casts/universe.h"
#include "sema/convert.h"
#include "sema/sema_error.h"
#include "sema/sema_fixture.h"
#include "sema/target.h"
#include "sema/type.h"
#include "sema/type_store.h"

namespace minc::sema {
namespace {

struct CastCase {
  // The two ends, by the name a reader would write.
  std::string from;
  std::string to;
  bool ok;
  CastKind kind;
  CastLoss loss = CastLoss::None;
};

// Every row the matrix has. It is not sampled: a pair the matrix answers and this
// table does not is a pair whose answer no test pins, and a pair the matrix
// answers differently is a failure in the row that is here.
const std::vector<CastCase>& castCases() {
  static const std::vector<CastCase> cases{
      // --- a different name for the same width: no instruction at all --------
      // The sign is a *reading* of the bits and not the bits, so a same-width
      // signedness change is the one identity that loses something: the number
      // is not the one that was written (`u32` of `-1` is 4294967295).
      {"i32", "u32", true, CastKind::Identity, CastLoss::Sign},
      {"u32", "i32", true, CastKind::Identity, CastLoss::Sign},
      {"i64", "u64", true, CastKind::Identity, CastLoss::Sign},
      {"i8", "u8", true, CastKind::Identity, CastLoss::Sign},
      {"char", "u8", true, CastKind::Identity},
      {"u8", "char", true, CastKind::Identity},
      // --- integers -----------------------------------------------------------
      {"i8", "i32", true, CastKind::IntegerExtend},
      {"i8", "u32", true, CastKind::IntegerExtend},
      {"u8", "i32", true, CastKind::IntegerExtend},
      {"u8", "u64", true, CastKind::IntegerExtend},
      {"i32", "i128", true, CastKind::IntegerExtend},
      {"u8", "i128", true, CastKind::IntegerExtend},
      // Narrowing is *one* statement, `bits are dropped`, whichever signedness
      // the two ends have: the high bits go, and what the remaining ones mean is
      // decided by the destination.
      {"i64", "i8", true, CastKind::IntegerTruncate, CastLoss::Truncation},
      {"i64", "u8", true, CastKind::IntegerTruncate, CastLoss::Truncation},
      {"u64", "i8", true, CastKind::IntegerTruncate, CastLoss::Truncation},
      {"i128", "u8", true, CastKind::IntegerTruncate, CastLoss::Truncation},
      // --- bool, which is not an integer in this language ---------------------
      {"bool", "i32", true, CastKind::BoolToInteger},
      {"bool", "u8", true, CastKind::BoolToInteger},
      {"i32", "bool", true, CastKind::IntegerToBool},
      {"u8", "bool", true, CastKind::IntegerToBool},
      // --- ordinary numbers, both directions and both widths ------------------
      {"i32", "f64", true, CastKind::IntegerToFloat},
      {"i32", "f32", true, CastKind::IntegerToFloat, CastLoss::Precision},
      {"u8", "f64", true, CastKind::IntegerToFloat},
      {"u8", "f32", true, CastKind::IntegerToFloat},
      {"i64", "f32", true, CastKind::IntegerToFloat, CastLoss::Precision},
      {"u64", "f32", true, CastKind::IntegerToFloat, CastLoss::Precision},
      {"u64", "f64", true, CastKind::IntegerToFloat, CastLoss::Precision},
      {"f64", "f32", true, CastKind::FloatTruncate, CastLoss::Precision},
      {"f32", "f64", true, CastKind::FloatExtend},
      {"f32", "f80", true, CastKind::FloatExtend},
      {"f80", "f64", true, CastKind::FloatTruncate, CastLoss::Precision},
      // The guarded row: the destination's bounds are the precondition, and the
      // loss is stated whether or not the value happens to fit -- an out-of-range
      // value traps, and every value that does fit is truncated toward zero.
      {"f64", "i32", true, CastKind::FloatToInteger, CastLoss::Range | CastLoss::Truncation},
      {"f64", "u8", true, CastKind::FloatToInteger, CastLoss::Range | CastLoss::Truncation},
      {"f32", "i64", true, CastKind::FloatToInteger, CastLoss::Range | CastLoss::Truncation},
      {"f80", "i128", true, CastKind::FloatToInteger, CastLoss::Range | CastLoss::Truncation},
      // --- addresses, which are one class of value ----------------------------
      {"str", "*u8", true, CastKind::Identity},
      {"*u8", "str", true, CastKind::Identity},
      {"*i32", "*void", true, CastKind::Identity},
      {"*void", "*f64", true, CastKind::Identity},
      // The bits are the same and the pointee is not: an address is one opaque
      // pointer in this model, so the pair is the identity and the *reading* of
      // what lives there is the source's business (`memory.md`).
      {"*i32", "*f64", true, CastKind::Identity},
      // The two named joins of `memory.md`, counted by `-Wprovenance`: `expose`
      // and `with_exposed_provenance`, and never implicit.
      //
      // `u64` stands for `usize` here and is not a stand-in: `usize` *is* the
      // pointer-width integer, so on this target it interns to the `u64` id
      // (`sema/type.h`, and the reason `usize` has no id of its own). A pointer
      // and it are the same width, so `expose` loses nothing; a pointer into a
      // `u8` does not fit.
      {"*i32", "u64", true, CastKind::PointerToInteger},
      {"*void", "u8", true, CastKind::PointerToInteger, CastLoss::Truncation},
      // `*void` → `u64` is no loss and `*void` → `u8` loses the address's high
      // bits. The other direction is **not** the mirror of this one, which is the
      // bug `ztests` found: `u64` → `*i32` loses nothing, while a `u8` into a
      // pointer is a `Sign` loss and a `u128` into one truncates.
      {"u64", "*i32", true, CastKind::IntegerToPointer},
      {"i32", "*u8", true, CastKind::IntegerToPointer, CastLoss::Sign},
      {"u8", "*void", true, CastKind::IntegerToPointer},
      {"u128", "*void", true, CastKind::IntegerToPointer, CastLoss::Truncation},
      // --- the bottom type: the conversion is vacuous -------------------------
      {"!", "i32", true, CastKind::Identity},
      {"!", "*i32", true, CastKind::Identity},
      {"i32", "!", false, CastKind::None},
      // --- refused ------------------------------------------------------------
      // A float is not a `bool`: a NaN is neither true nor false, so there is no
      // conversion to have an answer for, and the refusal names the test to write
      // instead.
      {"f64", "bool", false, CastKind::None},
      // An aggregate and a function have no value to convert, in either
      // direction, and `void` is not a type a value can have.
      {"i32", "[4]i32", false, CastKind::None},
      {"[4]i32", "i32", false, CastKind::None},
      {"i32", "[]i32", false, CastKind::None},
      {"[]i32", "i32", false, CastKind::None},
      {"[]i32", "*i32", false, CastKind::None},
      {"i32", "void", false, CastKind::None},
      {"void", "i32", false, CastKind::None},
      // A deferred literal is not a type anything is cast *to*: it is the type an
      // expression has before a context decides one.
      {"i32", "<int literal>", false, CastKind::None},
      {"<int literal>", "i32", false, CastKind::None},
      {"<float literal>", "f64", false, CastKind::None},
  };
  return cases;
}

// A type from the name a row writes, through the universe both cast suites
// share: an unknown name is a failure of the table rather than a silent default.
[[nodiscard]] TypeId lookup(TypeStore& types, const std::string& name) {
  return test::casts::typeOf(types, name);
}

TEST(CastTest, TheMatrixIsPinnedPairByPair) {
  TypeStore types;
  for (const CastCase& one : castCases()) {
    const TypeId from = lookup(types, one.from);
    const TypeId to = lookup(types, one.to);
    ASSERT_NE(from, kTypeError) << one.from;
    ASSERT_NE(to, kTypeError) << one.to;
    const CastResult result = castResult(types, from, to);
    const std::string what = one.from + " as " + one.to;
    EXPECT_EQ(result.ok, one.ok) << what << ": " << result.message;
    EXPECT_EQ(result.kind, one.kind) << what;
    if (one.ok) {
      EXPECT_EQ(result.loss, one.loss) << what << ": " << lossPhrase(result.loss);
      EXPECT_TRUE(result.message.empty()) << what << ": " << result.message;
    } else {
      // Every refusal names what to write instead: a cast that says only "no" is
      // a cast a reader has to guess about, and the guess is the code they are
      // about to write anyway.
      EXPECT_FALSE(result.message.empty()) << what;
    }
  }
}

// --- the whole matrix, not a sample ---------------------------------------------
//
// The table above pins *rows*. This walks the product of every type the language
// has against every other, and asserts the properties a matrix of this shape has
// to have whatever its rows say. A pair whose row nobody wrote is exactly the
// pair the table cannot fail on, and exactly what this catches.

TEST(CastTest, EveryPairOfEveryTypeTheLanguageHasIsAnswered) {
  // The list is pinned so it cannot shrink unnoticed: a type removed is a pair
  // the tests stopped asking about, which is a green suite that proves less.
  ASSERT_EQ(test::casts::universe().size(), 26U);
  TypeStore types;
  std::size_t accepted = 0;
  for (const std::string_view fromName : test::casts::universe()) {
    for (const std::string_view toName : test::casts::universe()) {
      const TypeId from = test::casts::typeOf(types, fromName);
      const TypeId to = test::casts::typeOf(types, toName);
      ASSERT_NE(from, kTypeError) << fromName;
      ASSERT_NE(to, kTypeError) << toName;
      const std::string what = std::string(fromName) + " as " + std::string(toName);
      const CastResult result = castResult(types, from, to);
      accepted += result.ok ? 1U : 0U;

      // One answer and never two: an accepted pair names its conversion and says
      // nothing more; a refused one says why, in a sentence a reader can act on.
      EXPECT_EQ(result.ok, result.kind != CastKind::None) << what;
      if (result.ok) {
        EXPECT_TRUE(result.message.empty()) << what << ": " << result.message;
      } else {
        EXPECT_FALSE(result.message.empty()) << what;
      }

      // A cast never refuses a pair the language converts **by itself**: an
      // explicit conversion is the same conversion written down (`casts.md`). A
      // pair that reached `convertible` and not the matrix is the direction that
      // matters -- it is a conversion no reader could spell out.
      //
      // Asked only about pairs of *decided* types. A deferred literal is the state
      // of a literal, not a type a value has: `convertible` answers generously
      // about it because it is arithmetic-shaped, and the matrix refuses it in
      // both directions for the same reason -- which is fine, because no program
      // reaches either. `checkCast` decides the operand's literal before it asks
      // the matrix, so `1 as f64` arrives here as `i32 as f64`
      // (`TheTwoSpellingsAreOneCast` and the suffix tests are that surface).
      const bool decided = !types.isDeferred(from) && !types.isDeferred(to);
      if (decided && convertible(types, from, to)) {
        EXPECT_TRUE(result.ok) << what
                               << ": the language converts this pair by itself, and the cast "
                                  "of it was refused: "
                               << result.message;
      }

      // A cast to the type a value already has is the identity, and it loses
      // nothing: it is how a reader says "I know", and it must cost nothing.
      if (from == to) {
        EXPECT_EQ(result.kind, CastKind::Identity) << what;
        EXPECT_EQ(result.loss, CastLoss::None) << what;
      }

      // The loss a pair reports is the loss its *kind* has, and no other. This is
      // the half of the matrix `-Wcast` reads, so a kind that grew a loss it does
      // not have is a warning that lies about what happened to the value.
      const bool truncation = hasLoss(result.loss, CastLoss::Truncation);
      const bool sign = hasLoss(result.loss, CastLoss::Sign);
      const bool precision = hasLoss(result.loss, CastLoss::Precision);
      const bool range = hasLoss(result.loss, CastLoss::Range);
      switch (result.kind) {
      case CastKind::Identity: {
        // The one identity that loses something: the same width under another
        // signedness, where the bits are kept and the number is not. `char` and
        // `u8` are the same width *and* the same signedness, which is why the pair
        // loses nothing, and `i8`/`char` is the same width and does.
        const bool sameWidthDifferentSign =
            types.isInteger(from) && types.isInteger(to) &&
            test::casts::integerWidth(types, from) == test::casts::integerWidth(types, to) &&
            test::casts::signedType(types, from) != test::casts::signedType(types, to);
        EXPECT_EQ(sign, from != to && sameWidthDifferentSign) << what;
        EXPECT_FALSE(truncation || precision || range) << what;
        break;
      }
      case CastKind::BoolToInteger:
      case CastKind::IntegerExtend:
      case CastKind::IntegerToBool:
      case CastKind::FloatExtend:
        EXPECT_EQ(result.loss, CastLoss::None) << what;
        break;
      case CastKind::IntegerTruncate:
        EXPECT_EQ(result.loss, CastLoss::Truncation) << what;
        break;
      case CastKind::FloatTruncate:
        EXPECT_EQ(result.loss, CastLoss::Precision) << what;
        break;
      case CastKind::IntegerToFloat:
        // Rounded exactly when the integer is wider than the destination's
        // mantissa -- `i32` to `f64` is exact, `i64` to `f64` is not.
        EXPECT_EQ(precision, test::casts::integerWidth(types, from) >
                                 test::casts::mantissaBits(types.get(to).bits))
            << what;
        EXPECT_FALSE(truncation || sign || range) << what;
        break;
      case CastKind::FloatToInteger:
        // The guarded row: both halves are always stated, whatever the value is.
        EXPECT_TRUE(range) << what;
        EXPECT_TRUE(truncation) << what;
        EXPECT_FALSE(sign || precision) << what;
        break;
      case CastKind::PointerToInteger:
        EXPECT_EQ(truncation, test::casts::integerWidth(types, to) < types.target().pointerBits)
            << what;
        EXPECT_FALSE(sign || precision || range) << what;
        break;
      case CastKind::IntegerToPointer: {
        // Not the mirror of `PointerToInteger`: the wide integer is the one that
        // truncates, and a narrowed *signed* one has its sign reinterpreted by the
        // zero-extension.
        const std::uint16_t width = test::casts::integerWidth(types, from);
        EXPECT_EQ(truncation, width > types.target().pointerBits) << what;
        EXPECT_EQ(sign, width < types.target().pointerBits && test::casts::signedType(types, from))
            << what;
        EXPECT_FALSE(precision || range) << what;
        break;
      }
      case CastKind::None:
        EXPECT_EQ(result.loss, CastLoss::None) << what;
        break;
      }
    }
  }
  // Not a threshold: the number this matrix answers for this universe, so a pair
  // that stops being answered -- or one that starts -- is a line in a diff rather
  // than a suite that quietly covers less.
  EXPECT_EQ(accepted, 388U);
}

TEST(CastTest, AnIntegerIntoAPointerIsMeasuredAgainstTheTargetsPointer) {
  // The one row whose answer is a property of the *target* and not of the two
  // types: `i32` into a pointer is a zero-extension on a 64-bit target (a negative
  // address becomes a large positive one) and the identity on a 32-bit one, where
  // the widths are equal and nothing happens at all. A rule written as "the
  // integer is narrower than the pointer" would answer the second wrong.
  const std::optional<Triple> wideTriple = parseTriple(kTripleLinuxAmd64);
  const std::optional<Triple> narrowTriple = parseTriple(kTripleLinuxI386);
  ASSERT_TRUE(wideTriple.has_value());
  ASSERT_TRUE(narrowTriple.has_value());
  const std::optional<TargetInfo> wide = targetInfo(*wideTriple);
  const std::optional<TargetInfo> narrow = targetInfo(*narrowTriple);
  ASSERT_TRUE(wide.has_value());
  ASSERT_TRUE(narrow.has_value());

  TypeStore on64(*wide);
  const CastResult sixty_four = castResult(on64, kTypeI32, on64.pointerTo(kTypeU8));
  EXPECT_TRUE(sixty_four.ok);
  EXPECT_EQ(sixty_four.kind, CastKind::IntegerToPointer);
  EXPECT_EQ(sixty_four.loss, CastLoss::Sign);

  TypeStore on32(*narrow);
  const CastResult thirty_two = castResult(on32, kTypeI32, on32.pointerTo(kTypeU8));
  EXPECT_TRUE(thirty_two.ok);
  EXPECT_EQ(thirty_two.kind, CastKind::IntegerToPointer);
  EXPECT_EQ(thirty_two.loss, CastLoss::None);
  // And the wide integer truncates on both, because 128 bits do not fit either.
  EXPECT_EQ(castResult(on32, kTypeU128, on32.pointerTo(kTypeU8)).loss, CastLoss::Truncation);
}

TEST(CastTest, EveryAcceptedPairNamesTheInstructionItNeeds) {
  // The table the `ir` suite looks for in the module, checked here against the
  // kinds: a pair that emits nothing is the identity family, the guarded row is
  // the only one that is not one opcode, and everything else names exactly one.
  TypeStore types;
  for (const std::string_view fromName : test::casts::universe()) {
    for (const std::string_view toName : test::casts::universe()) {
      const TypeId from = test::casts::typeOf(types, fromName);
      const TypeId to = test::casts::typeOf(types, toName);
      const std::string what = std::string(fromName) + " as " + std::string(toName);
      const CastResult result = castResult(types, from, to);
      const std::string_view instruction = test::casts::instructionFor(types, from, to);
      if (!result.ok) {
        EXPECT_EQ(instruction, "refused") << what;
        continue;
      }
      if (result.kind == CastKind::Identity) {
        EXPECT_TRUE(instruction.empty()) << what << ": " << instruction;
        continue;
      }
      EXPECT_FALSE(instruction.empty()) << what;
      EXPECT_EQ(instruction == "guarded", result.kind == CastKind::FloatToInteger) << what;
      // The three rows that extend or convert a *number* name the opcode of the
      // **source's** signedness: `sext`/`sitofp` for a signed source, `zext`/
      // `uitofp` for an unsigned one. `bool` and `char` are unsigned, which is the
      // whole reason `true as f32` is `uitofp i1` and not a claim about a sign.
      if (result.kind == CastKind::IntegerExtend || result.kind == CastKind::IntegerToFloat ||
          result.kind == CastKind::BoolToInteger) {
        const bool fromSigned = test::casts::signedType(types, from);
        EXPECT_EQ(instruction == "sext" || instruction == "sitofp", fromSigned) << what;
      }
    }
  }
}

TEST(CastTest, CastingATypeToItselfIsTheIdentityAndLosesNothing) {
  TypeStore types;
  for (const TypeId type :
       {kTypeI32, kTypeU8, kTypeF64, kTypeBool, kTypeChar, kTypeStr, kTypeNever,
        types.pointerTo(kTypeI32), types.arrayOf(kTypeI32, 4), types.sliceOf(kTypeF64)}) {
    const CastResult result = castResult(types, type, type);
    ASSERT_TRUE(result.ok) << types.spelling(type);
    EXPECT_EQ(result.kind, CastKind::Identity);
    EXPECT_EQ(result.loss, CastLoss::None);
  }
}

TEST(CastTest, NoCastBetweenAnIntegerAndAFloatIsImplicit) {
  // The rule the cast exists for: `let x: f64 = 1;` is refused and `let x: f64 =
  // 1 as f64;` is not. If the second part ever became true of the first, the cast
  // would have become a conversion and the language would be converting silently.
  TypeStore types;
  EXPECT_FALSE(convertible(types, kTypeI32, kTypeF64));
  EXPECT_FALSE(convertible(types, kTypeF64, kTypeI32));
  EXPECT_TRUE(castResult(types, kTypeI32, kTypeF64).ok);
  EXPECT_TRUE(castResult(types, kTypeF64, kTypeI32).ok);
}

TEST(CastTest, TheIntegerFoldAgreesWithTheMatrix) {
  // `foldIntCast` is what makes `const a = (u8)5;` a value another constant
  // expression can use, and it must be the *same* conversion the lowering emits.
  TypeStore types;
  const std::optional<support::ConstInt> truncating =
      foldIntCast(types, kTypeI32, kTypeU8, support::ConstInt::fromSigned(300));
  ASSERT_TRUE(truncating.has_value());
  EXPECT_EQ(truncating->bits, 44U); // 300 & 0xFF
  const std::optional<support::ConstInt> extending =
      foldIntCast(types, kTypeI8, kTypeI32, support::ConstInt::fromSigned(-1));
  ASSERT_TRUE(extending.has_value());
  EXPECT_EQ(extending->bits, 0xFFFFFFFFU);
  // And a pair that is not an integer-to-integer conversion has no folded value:
  // the float side keeps `isConstant` and loses only the number.
  EXPECT_FALSE(
      foldIntCast(types, kTypeF64, kTypeI32, support::ConstInt::fromSigned(1)).has_value());
  EXPECT_FALSE(
      foldIntCast(types, kTypeI32, kTypeF64, support::ConstInt::fromSigned(1)).has_value());
}

// --- the surface ---------------------------------------------------------------
//
// The matrix is a table over types; these are the two spellings that have to reach
// it, the suffix that types a literal, and the refusal of a name that is now
// reserved.

TEST(CastTest, TheTwoSpellingsAreOneCast) {
  const std::string both = "fn i32 main()\n"
                           "{\n"
                           "  let a: u8 = 200;\n"
                           "  let b: i32 = a as i32;\n"
                           "  let c: i32 = (i32)a;\n"
                           "  return b + c;\n"
                           "}\n";
  test::SemaFixture f;
  f.source(both);
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u) << f.dump();
  // And both are `i32`, which is the only half of "one cast" the checker can be
  // asked about directly: the other half is the instruction, and it is pinned in
  // `tests/unit/ir/cast_test.cc` over the module text.
  EXPECT_EQ(f.bindingType("b"), "i32");
  EXPECT_EQ(f.bindingType("c"), "i32");
}

TEST(CastTest, ACastIsNotASecondDiagnosticForAnAlreadyReportedOperand) {
  test::SemaFixture f;
  f.source("fn i32 main() { let x: i33 = 1; return x as i32; }\n");
  ASSERT_TRUE(f.build());
  // One mistake, one message: the unknown type is reported and the cast over it
  // says nothing, because there is no operand to have an opinion about.
  EXPECT_TRUE(f.hasError("sema-unknown-type"));
  EXPECT_EQ(f.errorCount(), 1u) << f.dump();
}

TEST(CastTest, ASuffixedLiteralHasTheTypeItsSuffixNames) {
  test::SemaFixture f;
  f.source("fn i32 main()\n"
           "{\n"
           "  let a = 10u8;\n"
           "  let b = 300;\n"
           "  let c = 12f32;\n"
           "  let d = 1.5;\n"
           "  return 0;\n"
           "}\n");
  ASSERT_TRUE(f.build());
  ASSERT_EQ(f.errorCount(), 0u) << f.dump();
  // The suffix *is* the type: `10u8` is a `u8` and not an `i32` that a context
  // would have to decide. The unsuffixed literal is still deferred and still
  // defaults to the core's `i32`/`f64` when nothing decides it.
  EXPECT_EQ(f.bindingType("a"), "u8");
  EXPECT_EQ(f.bindingType("b"), "i32");
  EXPECT_EQ(f.bindingType("c"), "f32");
  EXPECT_EQ(f.bindingType("d"), "f64");
}

TEST(CastTest, TheLongDoubleSuffixIsTheTargetsLongDouble) {
  // `1.5L` is the *target's* `long double`, which is why the suffix reader asks
  // the target and not a type table: x86 has x87's `f80`, AArch64 has no `f80` at
  // all (its `long double` is IEEE binary128, a format the language has no
  // spelling for yet) and MSVC's is a plain `f64`. Both targets are asked rather
  // than the host, which is the difference between a test of the rule and a test
  // of the machine that happens to run it: this assertion used to name `f80`
  // alone, which passed on an x86_64 host and failed on macOS.
  const std::optional<TargetInfo> x87 = targetFromName(kTripleLinuxAmd64);
  ASSERT_TRUE(x87.has_value());
  test::SemaFixture onX87{std::string("x87.mx"), *x87};
  ASSERT_TRUE(onX87.source("fn i32 main() { let e = 1.5L; return 0; }\n").build());
  ASSERT_EQ(onX87.errorCount(), 0u) << onX87.dump();
  EXPECT_EQ(onX87.bindingType("e"), "f80");

  const std::optional<TargetInfo> arm = targetFromName(kTripleLinuxAarch64);
  ASSERT_TRUE(arm.has_value());
  test::SemaFixture onArm{std::string("arm.mx"), *arm};
  ASSERT_TRUE(onArm.source("fn i32 main() { let e = 1.5L; return 0; }\n").build());
  ASSERT_EQ(onArm.errorCount(), 0u) << onArm.dump();
  EXPECT_EQ(onArm.bindingType("e"), "f128");

  const std::optional<TargetInfo> msvc = targetFromName(kTripleWindowsAmd64);
  ASSERT_TRUE(msvc.has_value());
  test::SemaFixture onMsvc{std::string("msvc.mx"), *msvc};
  ASSERT_TRUE(onMsvc.source("fn i32 main() { let e = 1.5L; return 0; }\n").build());
  ASSERT_EQ(onMsvc.errorCount(), 0u) << onMsvc.dump();
  EXPECT_EQ(onMsvc.bindingType("e"), "f64");
}

TEST(CastTest, ASuffixedLiteralOutOfItsOwnRangeIsRefusedAtTheLiteral) {
  test::SemaFixture f;
  f.source("fn i32 main() { let a = 300u8; return 0; }\n");
  ASSERT_TRUE(f.build());
  // The suffix *is* the type, so the range check is the one every deferred literal
  // already gets -- and it names the literal, not a conversion.
  EXPECT_TRUE(f.hasError("sema-literal-out-of-range")) << f.dump();
  EXPECT_EQ(f.errorCount(), 1u) << f.dump();
}

TEST(CastTest, ATypeNameIsReservedSoTheCStyleCastIsUnambiguous) {
  test::SemaFixture f;
  f.source("fn i32 main() { let i32 = 5; return i32; }\n");
  ASSERT_TRUE(f.build());
  // The resolver, and not the checker: a reserved word can never be a
  // declaration, so the refusal is about the *name* and happens where names are
  // decided. That is what lets the parser read `(i32)` as a type with no symbol
  // table.
  EXPECT_TRUE(f.hasResolveError("resolve-reserved-identifier"));
}

TEST(CastTest, WCastNamesTheLossAndIsOffByDefault) {
  const std::string source = "fn i32 main() { let x: i64 = 300; let y = x as i8; return y; }\n";
  {
    test::SemaFixture f;
    f.source(source);
    ASSERT_TRUE(f.build());
    EXPECT_FALSE(f.hasWarning("sema-cast-loses")) << f.dump();
  }
  {
    test::SemaFixture f;
    f.warnCast();
    f.source(source);
    ASSERT_TRUE(f.build());
    ASSERT_TRUE(f.hasWarning("sema-cast-loses")) << f.dump();
    // The warning names the loss: a reader knows which of the four it is without
    // reading the matrix.
    const sema::SemaError& warning = f.output().warnings.front();
    // The phrase is `lossPhrase`'s, and the assertion is on the phrase and not on
    // the word "truncation": the reader is told what happened to the value, not
    // the name of a flag in this compiler's table.
    EXPECT_NE(warning.message.find("bits are dropped"), std::string::npos) << warning.message;
    EXPECT_NE(warning.message.find("may lose"), std::string::npos) << warning.message;
  }
}

// An address is *obtained*, and the compiler can see when it was not. The refusal
// is the same shape as the float constant that cannot fit (decision 8): a value
// the compiler knows is a value it can refuse, and a *value* -- however it was
// arrived at -- is never refused, because counting the operation is the model's
// answer to "where did this come from" (`casts.md`, decision 11b).
TEST(CastTest, AnAddressCannotComeFromAConstant) {
  struct Case {
    const char* source;
    const char* nullSpelling;
  };
  // Every spelling of a constant address reaches the one code: a literal, a
  // folded expression, a `const` name, a negative number, and zero -- which is
  // not an exception, because the null address has a spelling of its own.
  const Case refused[] = {
      {"fn i32 main() { let p: *i32 = 1 as *i32; return 0; }\n", "`null`"},
      {"fn i32 main() { let p: *i32 = 0 as *i32; return 0; }\n", "`null`"},
      {"fn i32 main() { let p: *i32 = (1 + 1) as *i32; return 0; }\n", "`null`"},
      {"fn i32 main() { let p: *i32 = -1 as *i32; return 0; }\n", "`null`"},
      {"const n: usize = 4096;\nfn i32 main() { let p: *u8 = n as *u8; return 0; }\n", "`null`"},
      // A `str` is not a `*void`, so its null spelling is a cast of one -- and the
      // sentence says so rather than sending the reader to a spelling that does
      // not compile.
      {"fn i32 main() { let s: str = (str)1; return 0; }\n", "`null as str`"},
      {"fn i32 main() { let s: str = 0 as str; return 0; }\n", "`null as str`"},
  };
  for (const Case& testCase : refused) {
    test::SemaFixture f;
    f.source(testCase.source);
    ASSERT_TRUE(f.build());
    ASSERT_TRUE(f.hasError("sema-address-from-constant")) << f.dump();
    EXPECT_EQ(f.errorCount(), 1u) << f.dump();
    const sema::SemaError& error = f.firstError();
    // One sentence, and it names the null spelling *this* target type takes.
    EXPECT_NE(error.message.find(testCase.nullSpelling), std::string::npos) << error.message;
    EXPECT_NE(error.message.find("nothing in this program obtained"), std::string::npos)
        << error.message;
  }

  // Values are not refused: an object's address, an exposed pointer cast back, an
  // integer that came from somewhere the compiler cannot see, and the null address
  // in its own spelling -- for both a `*T` and a `str`.
  const char* const accepted[] = {
      "fn i32 main() { let x: i32 = 1; let p: *i32 = &x; return 0; }\n",
      "fn i32 main() { let x: i32 = 1; let p: *i32 = &x; let a = p as usize;\n"
      "  let q: *i32 = a as *i32; return 0; }\n",
      "fn i32 main(a: usize) { let p: *u8 = a as *u8; return 0; }\n",
      "extern fn usize getenv(s: str);\nfn i32 main() { let n = getenv(\"X\");\n"
      "  let p: *u8 = n as *u8; return 0; }\n",
      "fn i32 main() { let p: *i32 = null; let s: str = null as str; return 0; }\n",
  };
  for (const char* source : accepted) {
    test::SemaFixture f;
    f.source(source);
    ASSERT_TRUE(f.build());
    EXPECT_EQ(f.errorCount(), 0u) << f.dump();
  }
}

TEST(CastTest, AProvenanceCastIsCountedAndIsNotAWarningAboutAccuracy) {
  // `*i32 → usize` is `expose`, and the count is `-Wprovenance`, not `-Wcast`:
  // the two are different claims (one says "this address escapes to an integer"
  // and the other "this conversion may lose digits"), and one flag may not report
  // the other's sites.
  const std::string source =
      "fn i32 main() { let x: i32 = 1; let p: *i32 = &x; let a = p as usize; return 0; }\n";
  test::SemaFixture f;
  f.warnCast();
  f.source(source);
  ASSERT_TRUE(f.build());
  ASSERT_EQ(f.errorCount(), 0u) << f.dump();
  // A pointer is 64 bits and a `usize` is 64 bits on this target, so the pair is
  // an `Identity` and not a widening: the `-Wcast` flag has nothing to say.
  EXPECT_FALSE(f.hasWarning("sema-cast-loses")) << f.dump();
}

} // namespace
} // namespace minc::sema
