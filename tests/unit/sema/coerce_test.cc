// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The coercion record: what `sema` publishes so the lowering never re-derives a
// conversion, and the deferred types it decides so no operand reaches the IR
// without a width.
//
// The four properties these tests hold, in the order they matter:
//
//   * **The record is complete**, checked by *enumeration* over every
//     `(from, to)` pair `convertible` permits -- not by sampling, because the
//     failure mode of an incomplete table is a missing conversion in a program
//     nobody wrote yet.
//   * **The record is consistent with the tree**: `from` is always the operand's
//     own final type, and both ends are types the IR can map.
//   * **The operation type of a compound assignment** is published, because it
//     is the one fact the tree cannot show.
//   * **Nothing deferred survives**: a deferred type has no width, so a single
//     one reaching the lowering is a wrong instruction, not a missing one.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "sema/convert.h"
#include "sema/sema_fixture.h"
#include "sema/type.h"
#include "sema/type_store.h"
#include "sema/typed_ast.h"

namespace minc::test {
namespace {

using Pair = std::pair<std::string, std::string>;

[[nodiscard]] std::string pairText(const sema::Coercion& coercion, const sema::TypeStore& types) {
  return types.spelling(coercion.from) + "->" + types.spelling(coercion.to);
}

[[nodiscard]] std::set<Pair> recordedPairs(const SemaFixture& f) {
  std::set<Pair> out;
  for (const sema::Coercion& coercion : f.typed().coercions()) {
    out.emplace(f.types().spelling(coercion.from), f.types().spelling(coercion.to));
  }
  return out;
}

// The coercion one consumer applies to one operand, or nullptr.
[[nodiscard]] const sema::Coercion* findCoercion(const SemaFixture& f, ast::AstId consumer,
                                                 std::uint8_t operand) {
  return f.typed().coercionAt(consumer, operand);
}

// The first node of a kind in the unit, for a test that knows the shape of what
// it wrote.
[[nodiscard]] ast::AstId firstOfKind(const SemaFixture& f, ast::NodeKind kind) {
  for (std::uint32_t i = 0; i < f.lowered().nodeCount(); ++i) {
    const ast::AstId id{i};
    if (f.lowered().at(id).kind == kind) {
      return id;
    }
  }
  return ast::AstId{};
}

// The first coercion a consumer of that kind applies, and the node it belongs
// to. Tests that write several statements of one kind use this rather than
// guessing which one the index order will find first.
[[nodiscard]] std::pair<ast::AstId, const sema::Coercion*> firstCoercionOfKind(const SemaFixture& f,
                                                                               ast::NodeKind kind) {
  for (const sema::Coercion& coercion : f.typed().coercions()) {
    if (f.lowered().at(coercion.consumer).kind == kind) {
      return {coercion.consumer, &coercion};
    }
  }
  return {ast::AstId{}, nullptr};
}

TEST(CoerceTest, TheInitializerRecordsItsConversion) {
  SemaFixture f;
  f.source("fn i32 main() { let a: i8 = 1; let b: i64 = a; return 0; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_EQ(f.errorCount(), 0u);

  const std::pair<ast::AstId, const sema::Coercion*> found =
      firstCoercionOfKind(f, ast::NodeKind::LetStmt);
  ASSERT_NE(found.second, nullptr);
  EXPECT_EQ(pairText(*found.second, f.types()), "i8->i64");
  // The consumer is the declaration, and the operand is its initializer -- not
  // the name, which would put the conversion in a place with no value.
  EXPECT_EQ(found.first, found.second->consumer);
  EXPECT_EQ(found.second->operand, 0);
  EXPECT_EQ(findCoercion(f, found.first, 0), found.second);
}

TEST(CoerceTest, EveryConsumerKindIsRecorded) {
  // One program, one consumer of each kind. Each assertion names the *node* the
  // conversion belongs to and the pair, so a conversion moved from one seam to
  // another is caught rather than merely counted.
  SemaFixture f;
  f.source("fn i64 id(v: i64) { return v; }\n"
           "fn i8 take(v: i8) { return v; }\n"
           "fn i32 main() {\n"
           "  let a: i8 = 1;\n"
           "  let n: u8 = 2;\n"
           "  let q: u16 = 1;\n"
           "  let b: i64 = a;\n"              // initializer: i8 -> i64
           "  b = id(a);\n"                   // argument: i8 -> i64
           "  let c: i8 = take(a);\n"         // no conversion on either side
           "  let d: i32 = a + a;\n"          // binary operands: i8 -> i32
           "  let e: u32 = n << n;\n"         // shift operands: u8 -> i32
           "  let g: i32 = ~a;\n"             // prefix promotion: i8 -> i32
           "  let h: i32 = b == 1 ? a : q;\n" // arms: i8 -> i32, u16 -> i32
           "  let k: i32 = id(a);\n"          // initializer: i64 -> i32
           "  return 0;\n"
           "}\n");
  ASSERT_TRUE(f.build());
  ASSERT_EQ(f.errorCount(), 0u);

  const std::set<Pair> pairs = recordedPairs(f);
  EXPECT_TRUE(pairs.contains({"i8", "i64"}));  // initializer and argument
  EXPECT_TRUE(pairs.contains({"i8", "i32"}));  // binary operand, prefix, arm
  EXPECT_TRUE(pairs.contains({"u8", "i32"}));  // shift operand at `promote(u8)`
  EXPECT_TRUE(pairs.contains({"i32", "u32"})); // the shift's result into `u32`
  EXPECT_TRUE(pairs.contains({"u16", "i32"})); // the `?:` else arm
  EXPECT_TRUE(pairs.contains({"i64", "i32"})); // `id(a)` into an `i32` binding

  // The argument conversion specifically, on the call node: the callee is
  // operand 0, so the argument is operand 1.
  const ast::AstId call = firstOfKind(f, ast::NodeKind::CallExpr);
  ASSERT_TRUE(call.valid());
  const sema::Coercion* argument = findCoercion(f, call, 1);
  ASSERT_NE(argument, nullptr);
  EXPECT_EQ(pairText(*argument, f.types()), "i8->i64");

  // The `return` conversion, on the statement.
  SemaFixture r;
  r.source("fn i64 f(a: i32) { return a; }\n");
  ASSERT_TRUE(r.build());
  ASSERT_EQ(r.errorCount(), 0u);
  const ast::AstId ret = firstOfKind(r, ast::NodeKind::ReturnStmt);
  ASSERT_TRUE(ret.valid());
  const sema::Coercion* returning = findCoercion(r, ret, 0);
  ASSERT_NE(returning, nullptr);
  EXPECT_EQ(pairText(*returning, r.types()), "i32->i64");

  // The `?:` arms, on the conditional node: operands 1 and 2. The arms have
  // different types, so the conditional has to unify them and both arms convert.
  SemaFixture c;
  c.source("fn i32 f(flag: bool) { let a: i8 = 1; let u: u16 = 2; let b: i32 = flag ? a : u; "
           "return 0; }\n");
  ASSERT_TRUE(c.build());
  ASSERT_EQ(c.errorCount(), 0u);
  const ast::AstId cond = firstOfKind(c, ast::NodeKind::ConditionalExpr);
  ASSERT_TRUE(cond.valid());
  const sema::Coercion* thenArm = findCoercion(c, cond, 1);
  const sema::Coercion* elseArm = findCoercion(c, cond, 2);
  ASSERT_NE(thenArm, nullptr);
  ASSERT_NE(elseArm, nullptr);
  EXPECT_EQ(pairText(*thenArm, c.types()), "i8->i32");
  EXPECT_EQ(pairText(*elseArm, c.types()), "u16->i32");
  // The condition is operand 0 and never converts: it is `bool`, and a `bool` is
  // not arithmetic (`convert.h`).
  EXPECT_EQ(findCoercion(c, cond, 0), nullptr);
}

TEST(CoerceTest, EveryRecordedPairIsTheOperandsOwnFinalType) {
  SemaFixture f;
  f.source("fn i64 id(v: i64) { return v; }\n"
           "fn i32 main() {\n"
           "  let a: i8 = 1;\n"
           "  let b: i64 = a;\n"
           "  let c: i32 = a + a;\n"
           "  let d: i64 = c;\n"
           "  let e: i8 = -a;\n"
           "  b = id(a);\n"
           "  return 0;\n"
           "}\n");
  ASSERT_TRUE(f.build());
  ASSERT_FALSE(f.typed().coercions().empty());

  for (const sema::Coercion& coercion : f.typed().coercions()) {
    // `from` is the operand's own type, not a remembered one: the lowering looks
    // up `typeOf(node)` and must find exactly what the record promised.
    EXPECT_EQ(coercion.from, f.typed().typeOf(coercion.node))
        << pairText(coercion, f.types()) << " disagrees with node " << coercion.node.index;
    // ... and neither end is a deferred literal or the poison, because both are
    // types the IR has to be able to name.
    EXPECT_FALSE(f.types().isDeferred(coercion.from));
    EXPECT_FALSE(f.types().isDeferred(coercion.to));
    EXPECT_FALSE(f.types().isError(coercion.from));
    EXPECT_FALSE(f.types().isError(coercion.to));
    EXPECT_NE(coercion.from, coercion.to);
  }
}

TEST(CoerceTest, AConversionThatChangesNothingIsNotRecorded) {
  SemaFixture f;
  // Every one of these is typed at its own type, so the record is empty: an
  // entry for `i32->i32` would make the list longer without saying anything, and
  // the lowering reads "no entry" as "no conversion".
  f.source("fn i32 main() {\n"
           "  let a: i32 = 1;\n"
           "  let b: i32 = a;\n"
           "  let c: u8 = 1;\n"
           "  let d: u8 = c;\n"
           "  return 0;\n"
           "}\n");
  ASSERT_TRUE(f.build());
  ASSERT_EQ(f.errorCount(), 0u);
  EXPECT_TRUE(f.typed().coercions().empty());
}

TEST(CoerceTest, NothingDeferredReachesTheArtifact) {
  SemaFixture f;
  f.source("fn i32 main() {\n"
           "  let a = 1;\n"
           "  let b = 1.0 + 2.0;\n"
           "  let c: i64 = 1 + 2;\n"
           "  let d: u8 = 1 + 2;\n"
           "  let e: i32 = 1;\n"
           "  let g: i8 = -128;\n"
           "  return a + e;\n"
           "}\n");
  ASSERT_TRUE(f.build());
  ASSERT_EQ(f.errorCount(), 0u);
  for (std::uint32_t i = 0; i < f.lowered().nodeCount(); ++i) {
    const sema::TypeId type = f.typed().typeOf(ast::AstId{i});
    EXPECT_FALSE(f.types().isDeferred(type)) << "node " << i << " is still deferred";
  }
  // The context is what decides the shared subexpression, so an `f64` binding
  // must make `1 + 2.0` two `f64`s and not `i32` + `f64`.
  EXPECT_EQ(f.bindingType("b"), "f64");
  EXPECT_EQ(f.typeOfSpelling("1.0 + 2.0"), "f64");
  EXPECT_EQ(f.bindingType("d"), "u8");
  // ... and a narrow context decides the operation *and* its operands, which is
  // what makes the instruction `add i8` with two `i8` operands.
  {
    SemaFixture narrow;
    narrow.source("fn i32 main() { let d: u8 = 1 + 2; return 0; }\n");
    ASSERT_TRUE(narrow.build());
    ASSERT_EQ(narrow.errorCount(), 0u);
    EXPECT_EQ(narrow.typeOfSpelling("1 + 2"), "u8");
  }
}

TEST(CoerceTest, TheOperationTypeOfACompoundAssignmentIsPublished) {
  SemaFixture f;
  // The measured hole: `x <<= 9` on a `u16` is defined at `i32`, and the
  // `AssignExpr`'s own type is `u16`. Without the operation type the lowering
  // would emit `shl i16 %x, 9` -- an out-of-range shift, a poison value.
  f.source("fn i32 main() { let x: u16 = 1; x <<= 9; x += 1; return 0; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_EQ(f.errorCount(), 0u);

  const ast::AstId assign = firstOfKind(f, ast::NodeKind::AssignExpr);
  ASSERT_TRUE(assign.valid());
  const sema::ExprInfo& info = f.typed().infoOf(assign);
  ASSERT_TRUE(info.opType.valid());
  EXPECT_EQ(f.types().spelling(info.opType), "i32");
  // The assignment's own type is the store's, so the two are genuinely
  // different: `opType` is not a restatement of the node's type.
  EXPECT_EQ(f.types().spelling(f.typed().typeOf(assign)), "u16");
  const sema::Coercion* target = findCoercion(f, assign, 0);
  ASSERT_NE(target, nullptr);
  EXPECT_EQ(pairText(*target, f.types()), "u16->i32");
}

TEST(CoerceTest, AShiftCountIsCheckedAtTheOperationWidth) {
  {
    // `promote(u8)` is `i32`, so 31 is a legal count and 40 is not -- the count
    // is checked against the width the shift happens at, not the operand's.
    SemaFixture f;
    f.source("fn i32 main() { let x: u8 = 1; x <<= 31; return 0; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_EQ(f.errorCount(), 0u);
  }
  {
    SemaFixture f;
    f.source("fn i32 main() { let x: u8 = 1; x <<= 40; return 0; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-shift-count-out-of-range"));
    EXPECT_EQ(f.errorCount(), 1u);
  }
  {
    SemaFixture f;
    f.source("fn i32 main() { let x: i8 = 1; return x << 40; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-shift-count-out-of-range"));
    EXPECT_EQ(f.errorCount(), 1u);
  }
}

TEST(CoerceTest, ALiteralIsRefusedWhenTheDecidedTypeCannotHoldIt) {
  {
    // The context reaches the literals through the operation, so `300` is
    // refused rather than silently divided as `44`: without this the folded
    // value (`100`) and the emitted code (`14`) would disagree.
    SemaFixture f;
    f.source("fn i32 main() { let y: u8 = 300 / 3; return 0; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-literal-out-of-range"));
    EXPECT_EQ(f.errorCount(), 1u);
  }
  {
    SemaFixture f;
    f.source("fn i32 main() { let y: u8 = 300 * 0; return 0; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-literal-out-of-range"));
    EXPECT_EQ(f.errorCount(), 1u);
  }
  {
    // ... and the negation is the exception, because `-128` is representable
    // where `128` is not and the two spell the same literal.
    SemaFixture f;
    f.source("fn i32 main() { let x: i8 = -128; return x; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_EQ(f.errorCount(), 0u);
  }
  {
    SemaFixture f;
    f.source("fn i32 main() { let x: i8 = 128; return x; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-literal-out-of-range"));
    EXPECT_EQ(f.errorCount(), 1u);
  }
}

TEST(CoerceTest, AConversionThatIsNotPermittedIsNotRecorded) {
  // `bool` and `str` are not arithmetic, so those pairs never convert and never
  // appear: a record of a pair the language refuses would describe a conversion
  // no legal program has.
  SemaFixture f;
  f.source("fn i32 main() { let b: bool = true; let c: char = 'x'; return 0; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_EQ(f.errorCount(), 0u);
  for (const sema::Coercion& coercion : f.typed().coercions()) {
    EXPECT_TRUE(sema::convertible(f.types(), coercion.from, coercion.to));
  }
}

// --- the enumeration ---------------------------------------------------------

// Every arithmetic type the language names, and the spelling a program uses to
// reach it. `bool` and `str` are deliberately absent: they are not arithmetic
// (`convert.h`), so no pair involving them converts.
struct ArithmeticType {
  sema::TypeId id;
  const char* spelling;
};

constexpr ArithmeticType kArithmetic[] = {
    {sema::kTypeChar, "char"}, {sema::kTypeI8, "i8"},     {sema::kTypeI16, "i16"},
    {sema::kTypeI32, "i32"},   {sema::kTypeI64, "i64"},   {sema::kTypeI128, "i128"},
    {sema::kTypeU8, "u8"},     {sema::kTypeU16, "u16"},   {sema::kTypeU32, "u32"},
    {sema::kTypeU64, "u64"},   {sema::kTypeU128, "u128"}, {sema::kTypeF32, "f32"},
    {sema::kTypeF64, "f64"},   {sema::kTypeF80, "f80"},
};

// The enumeration runs against a **stated** target and not the host, because
// `f80` is a format and not a width (`sema.md` decision 26): on a machine without
// x87 the parameter `p_f80: f80` is refused by the type reader, and this test
// would then be measuring the CI machine instead of the coercion record. The row
// is one with x87, so every arithmetic type the language names has a spelling.
[[nodiscard]] sema::TargetInfo enumerationTarget() {
  const std::optional<sema::TargetInfo> target = sema::targetFromName(sema::kTripleLinuxAmd64);
  return target.value_or(sema::defaultTarget());
}

TEST(CoerceTest, TheRecordCoversEveryPermittedPair) {
  // The enumerated form of the rule, and the reason the doc calls the record a
  // *table* rather than a shortcut: for every ordered pair of arithmetic types
  // the language permits, a program that performs that conversion must produce
  // that entry. Sampling would pass on a record that is missing the pairs nobody
  // writes today.
  sema::TypeStore table(enumerationTarget());
  std::set<Pair> expected;
  for (const ArithmeticType& from : kArithmetic) {
    for (const ArithmeticType& to : kArithmetic) {
      if (from.id == to.id) {
        continue; // no conversion, no entry
      }
      if (sema::convertible(table, from.id, to.id)) {
        expected.emplace(from.spelling, to.spelling);
      }
    }
  }
  ASSERT_FALSE(expected.empty());

  // One program: a parameter per source type, and one binding per pair. A single
  // run covers the table, because the property is about the table and not about
  // the programs.
  std::string source = "fn i32 main(";
  bool first = true;
  for (const ArithmeticType& type : kArithmetic) {
    if (!first) {
      source += ", ";
    }
    first = false;
    source += std::string("p_") + type.spelling + ": " + type.spelling;
  }
  source += ") {\n";
  for (const ArithmeticType& from : kArithmetic) {
    for (const ArithmeticType& to : kArithmetic) {
      if (from.id == to.id || !expected.contains(Pair{from.spelling, to.spelling})) {
        continue;
      }
      source += "  let t_" + std::string(from.spelling) + "_" + to.spelling + ": " + to.spelling +
                " = p_" + from.spelling + ";\n";
    }
  }
  source += "  return 0;\n}\n";

  SemaFixture f("test.mx", enumerationTarget());
  f.source(source);
  ASSERT_TRUE(f.build());
  ASSERT_EQ(f.errorCount(), 0u);
  EXPECT_EQ(recordedPairs(f), expected);
  // One binding per pair, and each records exactly one conversion, so the count
  // and the table agree as well as the sets.
  EXPECT_EQ(f.typed().coercions().size(), expected.size());
}

} // namespace
} // namespace minc::test
