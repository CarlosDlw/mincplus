// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// A **generic function** is read for one instantiation at a time: the
// `TypeStore` gains the substituted signature, the call reaches that instance,
// and the template's own body is checked once against its binders.
//
// The record is `docs/architectures/generics.md`; this is the four rules it
// depends on, each pinned where it can be seen:
//
//  - a *use* written with arguments (`id::<i32>(x)`) and one written without
//    them (inference from the arguments, then from the context);
//  - one instance per **pair** of declaration and arguments, whatever the number
//    of call sites -- the table is what makes a recursive generic terminate;
//  - a binder nothing decides is a sentence, not a guess, and it names the form
//    to write;
//  - an operation on a binder needs the constraint that allows it, and until
//    constraints land that refusal is the honest answer.
#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "sema/sema.h"
#include "sema/sema_fixture.h"

namespace minc::sema {
namespace {

using test::SemaFixture;

TEST(GenericCallTest, AnExplicitInstanceIsOneInstanceAndTheCallReachesIt) {
  SemaFixture fixture;
  fixture.source(R"(
fn T identity<T>(value: T) {
  return value;
}

fn i32 main() {
  return identity::<i32>(3);
}
)");
  ASSERT_TRUE(fixture.build());
  ASSERT_FALSE(fixture.hasParseError());
  ASSERT_EQ(fixture.errorCount(), 0u) << fixture.errorCodes().front();

  // One instance, named for the argument and symbolised for the linker -- the
  // two names of § 7 of the record: the first is what `gdb` and a diagnostic
  // print, the second is what the object file holds.
  EXPECT_EQ(fixture.instances(),
            (std::vector<std::string>{"identity<i32> args=(i32) __M8_identityi32"}));
}

TEST(GenericCallTest, InferenceDecidesTheBinderFromTheArgument) {
  SemaFixture fixture;
  fixture.source(R"(
fn T identity<T>(value: T) {
  return value;
}

fn i32 main() {
  let a: f64 = identity(2.5);
  return 0;
}
)");
  ASSERT_TRUE(fixture.build());
  ASSERT_EQ(fixture.errorCount(), 0u) << fixture.errorCodes().front();
  // The argument decided it, so there is nothing to write at the call.
  EXPECT_EQ(fixture.instanceCount(), 1u);
  EXPECT_EQ(fixture.instances().front(), "identity<f64> args=(f64) __M8_identityf64");
}

TEST(GenericCallTest, TwoCallsAtOneTypeAreOneInstance) {
  SemaFixture fixture;
  fixture.source(R"(
fn T identity<T>(value: T) {
  return value;
}

fn i32 twice(x: i32) {
  return identity(identity(x));
}

fn i32 main() {
  return twice(4);
}
)");
  ASSERT_TRUE(fixture.build());
  ASSERT_EQ(fixture.errorCount(), 0u) << fixture.errorCodes().front();
  // The dedup rule, and the reason a recursive generic terminates: the pair
  // `(declaration, arguments)` is the key, and a repeat is never expanded twice.
  EXPECT_EQ(fixture.instanceCount(), 1u);
}

TEST(GenericCallTest, ABinderNothingDecidesNamesTheFormToWrite) {
  SemaFixture fixture;
  fixture.source(R"(
fn i32 f<T>(x: i32) {
  return x;
}

fn i32 main() {
  return f(3);
}
)");
  ASSERT_TRUE(fixture.build());
  // `T` is in no parameter and in nothing the context wants, so there is no
  // instance to reach -- which is a sentence and not a guess, and the sentence
  // names the form to write, because that is the reader's next keystroke
  // (`generics.md`, § 2).
  ASSERT_TRUE(fixture.hasError("sema-generic-not-inferable"));
  EXPECT_NE(fixture.firstError().message.find("f::<i32>"), std::string::npos)
      << fixture.firstError().message;
}

TEST(GenericCallTest, ABindersValueCannotComeFromNowhereYet) {
  SemaFixture fixture;
  fixture.source(R"(
fn T make<T>() {
  return 0;
}

fn i32 main() {
  let a: i32 = make();
  return a;
}
)");
  ASSERT_TRUE(fixture.build());
  // The *context* is what would decide `T` here -- `let a: i32 = make();` wants
  // an `i32`, and § 2 of the record lets it fill in what the arguments left
  // open. It does not get that far, and the reason is the honest one: the body
  // is refused first, because a literal is not a `T` until a constraint says `T`
  // is a number, and constraints are the next stage. This test pins the refusal
  // rather than a program the language cannot express yet.
  ASSERT_TRUE(fixture.hasError("sema-generic-operation"));
  EXPECT_NE(fixture.firstError().message.find("T: Num"), std::string::npos)
      << fixture.firstError().message;
}

TEST(GenericCallTest, AGenericAliasIsTheReturnTypeOfAnInstantiatedFunction) {
  SemaFixture fixture;
  fixture.source(R"(
type Pair<T, K> = (T, K);

fn Pair<T, K> make<T, K>(left: T, right: K) {
  return (left, right);
}

fn i32 main() {
  let p: Pair<i32, f64> = make(7, 1.5);
  return 0;
}
)");
  ASSERT_TRUE(fixture.build());
  // The two halves of the feature meeting: the return type is a *use* of a
  // generic alias whose arguments are the function's binders, and the instance's
  // signature is the substituted product -- so `Pair<i32, f64>` and
  // `(i32, f64)` are one type and the assignment needs no conversion.
  ASSERT_EQ(fixture.errorCount(), 0u) << fixture.errorCodes().front();
  EXPECT_EQ(fixture.instanceCount(), 1u);
  EXPECT_EQ(fixture.instances().front(), "make<i32, f64> args=(i32, f64) __M4_makei32f64");
}

TEST(GenericCallTest, AnOperationOnABinderNeedsItsConstraint) {
  SemaFixture fixture;
  fixture.source(R"(
fn T half<T>(value: T) {
  return value / 2;
}

fn i32 main() {
  return half::<i32>(4);
}
)");
  ASSERT_TRUE(fixture.build());
  // `value / 2` needs a binder that allows `/`, and a constraint is the next
  // stage. The refusal is named rather than the division being accepted on faith
  // for every argument the declaration might be given.
  ASSERT_TRUE(fixture.hasError("sema-generic-operation"));
  EXPECT_NE(fixture.firstError().message.find("T: Num"), std::string::npos)
      << fixture.firstError().message;
}

TEST(GenericCallTest, TheWrongNumberOfTypeArgumentsIsOneSentence) {
  SemaFixture fixture;
  fixture.source(R"(
fn T identity<T>(value: T) {
  return value;
}

fn i32 main() {
  return identity::<i32, i32>(3);
}
)");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.hasError("sema-generic-type-args"));
  EXPECT_NE(fixture.firstError().message.find("takes 1 type argument, and 2 were written"),
            std::string::npos)
      << fixture.firstError().message;
}

TEST(GenericCallTest, ATypeArgumentThatCannotBeStoredIsRefused) {
  SemaFixture fixture;
  fixture.source(R"(
fn T identity<T>(value: T) {
  return value;
}

fn i32 main() {
  return identity::<void>(0);
}
)");
  ASSERT_TRUE(fixture.build());
  // An argument is substituted into the declaration and *stored*, so it has to be
  // a type with a representation -- which is the rule that keeps `[4]void` and a
  // `(void, i32)` from existing one stage later.
  ASSERT_TRUE(fixture.hasError("sema-generic-type-args"));
}

TEST(GenericCallTest, ADoublingStructureIsRefusedRatherThanBuilt) {
  // `type P1 = (P0, P0);` doubles per line, so sixteen lines is a type made of
  // 65536 nodes -- four of them are enough to be past any bound a *test* wants,
  // and the point is the shape of the answer: one sentence, fast, and not a store
  // that spends a minute laying the structure out (`support/limits.h`,
  // `kMaxTypeNodes`).
  SemaFixture fixture;
  fixture.source(R"(
type P0 = (i32, i32);
type P1 = (P0, P0);
type P2 = (P1, P1);
type P3 = (P2, P2);
type P4 = (P3, P3);
type P5 = (P4, P4);
type P6 = (P5, P5);
type P7 = (P6, P6);
type P8 = (P7, P7);
type P9 = (P8, P8);
type P10 = (P9, P9);
type P11 = (P10, P10);
type P12 = (P11, P11);
type P13 = (P12, P12);
type P14 = (P13, P13);
type P15 = (P14, P14);
type P16 = (P15, P15);
type P17 = (P16, P16);
fn i32 use(x: P17) {
  return 0;
}
fn i32 main() {
  return 0;
}
)");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.hasError("sema-malformed-type"));
  EXPECT_NE(fixture.firstError().message.find("made of more than"), std::string::npos)
      << fixture.firstError().message;
}

TEST(GenericCallTest, AWideProductIsOrdinaryAndIsBuilt) {
  // The other side of the bound: a product whose members are written out is
  // linear in the source and is exactly what a product is for. 300 members is far
  // past any hand-written product and far below the bound, and the walk over it
  // is one pass per consumer -- which is what the cursor's stored child offsets
  // are for (`syntax/green.h`).
  std::string members;
  for (int i = 0; i < 300; ++i) {
    members += (i == 0 ? "" : ", ") + std::string("i32");
  }
  SemaFixture fixture;
  fixture.source("type Wide = (" + members + ");\nfn i32 main() { return 0; }\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_EQ(fixture.errorCount(), 0u) << fixture.errorCodes().front();

  const TypeStore& types = fixture.types();
  bool found = false;
  for (std::size_t i = 0; i < types.count(); ++i) {
    const TypeId id{static_cast<std::uint32_t>(i)};
    if (!types.isTuple(id)) {
      continue;
    }
    found = true;
    EXPECT_EQ(types.get(id).paramCount, 300u);
    // The layout of a wide product is one pass over its members, and its size is
    // what that pass says: 300 four-byte `i32`s, laid out in order.
    EXPECT_EQ(types.sizeOf(id), 1200u);
  }
  EXPECT_TRUE(found);
}

} // namespace
} // namespace minc::sema
