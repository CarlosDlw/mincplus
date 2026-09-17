// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The product in the checker: its typing, its member access, its refusals, and the
// destructuring binding (`tuples.md`). One file for the whole surface because the
// decisions behind them are one set -- a product is a *value* with named-by-
// position members, and every rule here follows from that.
//
// The refusals are half the file on purpose: a product that silently accepted an
// index, a comparison or an `extern` signature would be a layout crossing a
// boundary nobody described, and each of those has its own sentence.
#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "sema/sema_error.h"
#include "sema/sema_fixture.h"

namespace minc::test {
namespace {

// One unit per case, with the sentence that has to appear: the *fix* is what a
// reader repairs from, and a test that only matched the code would pass with a
// message that says the wrong thing.
void expectError(std::string_view source, std::string_view code, std::string_view sentence) {
  SemaFixture f;
  f.source(std::string(source));
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError(code)) << f.errorCodes().front();
  const std::string& message = f.firstError().message;
  EXPECT_NE(message.find(sentence), std::string::npos) << message;
}

} // namespace

TEST(TupleSemaTest, AProductIsItsMembersInTheOrderWritten) {
  SemaFixture f;
  f.source("fn i32 main() {\n  let p = (1, true);\n  let q: (i32, bool) = p;\n  return p.0;\n}\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);

  // The type is *structural*: the literal's members and the annotation's are the
  // same `TypeId`, and its spelling is what a reader can type back.
  EXPECT_EQ(f.bindingType("p"), "(i32, bool)");
  EXPECT_EQ(f.bindingType("q"), "(i32, bool)");
  EXPECT_EQ(f.bindingType("p"), f.bindingType("q"));
}

TEST(TupleSemaTest, AGeneratorTypesEachMemberAgainstItsPosition) {
  // The context decides *which* type each member is, and it decides it per
  // position -- the same rule a binding, a call argument and an array element
  // obey (`tuples.md`, decision 8). What it cannot do is change a literal's class:
  // an integer literal in a `f64` member stays an `i32` and is refused, because
  // this language has no silent integer-to-float conversion at all.
  {
    SemaFixture f;
    f.source("fn i32 main() {\n  let p: (i64, i64) = (1, 2);\n  return p.0;\n}\n");
    ASSERT_TRUE(f.build());
    EXPECT_EQ(f.errorCount(), 0u);
    EXPECT_EQ(f.bindingType("p"), "(i64, i64)");
  }
  expectError("fn i32 main() {\n  let p: (f64, f64) = (1, 2);\n  return 0;\n}\n",
              "sema-invalid-assignment", "cannot be used as `(f64, f64)`");
}

TEST(TupleSemaTest, AMemberIsAPlaceWhenItsBaseIsOne) {
  // `t.0 = 9` and `&t.0` reach the same member of the same object, which is what
  // makes a product an *object* and not a pair of values that happened to travel
  // together (`tuples.md`, decision 7).
  SemaFixture f;
  f.source(
      "fn i32 main() {\n  let t = (1, 2);\n  t.0 = 9;\n  let p: *i32 = &t.0;\n  return *p;\n}\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u) << (f.errorCount() == 0 ? "" : f.firstError().message);
}

TEST(TupleSemaTest, AMemberOfAValueIsAValue) {
  SemaFixture f;
  f.source("fn i32 main() {\n  let p = (1, 2);\n  let t = p.1;\n  &t;\n  return t;\n}\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
  EXPECT_EQ(f.bindingType("t"), "i32");
}

TEST(TupleSemaTest, APositionOutsideTheArityNamesBothNumbers) {
  expectError("fn i32 main() {\n  let t = (1, true);\n  return t.2;\n}\n",
              "sema-index-out-of-range", "this product has 2 members, reached as `t.0` to `t.1`");
}

TEST(TupleSemaTest, AMemberWrittenAsANameIsNotUnknownItIsTheWrongKind) {
  // A product's members have no names at all, so `.len` is the wrong *kind* of
  // component rather than a component that is missing -- and the sentence says
  // which spelling this type has (`tuples.md`, decision 3).
  expectError("fn i32 main() {\n  let t = (1, true);\n  return t.len;\n}\n", "sema-unknown-member",
              "a product's members have no names");
}

TEST(TupleSemaTest, AMemberOfSomethingWithNoMembersSaysSo) {
  expectError("fn i32 main() {\n  let x = 1;\n  return x.0;\n}\n", "sema-unknown-member",
              "has no members, so a position is not one");
}

TEST(TupleSemaTest, ThereIsNoRuntimeIndex) {
  // `t[i]` has no typing rule: a product is not a sequence and has no element type
  // for a subscript to produce. The sentence points at the member spelling, which
  // is how this type is walked (`tuples.md`, decision 3).
  expectError("fn i32 main() {\n  let t = (1, 2);\n  let i = 1;\n  return t[i];\n}\n",
              "sema-deref-not-pointer", "`(i32, i32)` is not one");
}

TEST(TupleSemaTest, AProductOfOneIsItsMember) {
  // Both spellings are refused by the *parser*, because both are a group of one
  // and the group is what the comma means. The checker is not reached.
  {
    SemaFixture f;
    f.source("fn i32 main() {\n  let t = (1,);\n  return 0;\n}\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasParseError());
    EXPECT_EQ(f.errorCount(), 0u);
  }
  {
    SemaFixture f;
    f.source("fn i32 main() {\n  let t = ();\n  return 0;\n}\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasParseError());
  }
}

TEST(TupleSemaTest, AProductIsNotEquatable) {
  // `==` is refused rather than answered member by member: equality of an
  // aggregate is a question about padding on some targets, and this language does
  // not answer questions it cannot answer exactly.
  expectError("fn i32 main() {\n  let a = (1, 2);\n  let b = (1, 2);\n  return a == b;\n}\n",
              "sema-invalid-operands", "==`");
}

TEST(TupleSemaTest, AProductCannotCrossTheCLanguageBoundary) {
  // The ABI for an aggregate is this compiler's internal convention (`tuples.md`,
  // decision 12), so a signature that promises one to a foreign compiler is
  // refused where the signature is written -- not discovered at link time.
  expectError("extern fn i32 f(p: (i32, bool));\nfn i32 main() { return 0; }\n",
              "sema-extern-aggregate", "extern");
}

TEST(TupleSemaTest, AProductHasNoSlotAtAVariadicBoundary) {
  // The default argument promotions are built from values that fit one register or
  // one pair, and a product is neither. Refused by name, and the fix is a pointer
  // or the members one by one (`tuples.md`, decision 18).
  expectError("extern fn i32 printf(fmt: str, ...);\n"
              "fn i32 main() {\n  let p = (1, 2);\n  return printf(\"%d\\n\", p);\n}\n",
              "sema-variadic-aggregate", "cannot be a variadic argument");
}

TEST(TupleSemaTest, AValuelessMemberIsRefusedWhereItIsWritten) {
  expectError("fn void nothing() { return; }\n"
              "fn i32 main() {\n  let p = (1, nothing());\n  return 0;\n}\n",
              "sema-invalid-assignment", "cannot be a member of a product");
}

TEST(TupleSemaTest, ADestructuringBindsOneNamePerMember) {
  SemaFixture f;
  f.source("fn (i32, bool) divmod(a: i32, b: i32) { return (a / b, a > b); }\n"
           "fn i32 main() {\n  let (q, r) = divmod(7, 2);\n  return r ? q : 0;\n}\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u) << f.firstError().message;
  // Each name is a binding with its own type -- the members of the value, in the
  // order the value's type wrote them.
  EXPECT_EQ(f.bindingType("q"), "i32");
  EXPECT_EQ(f.bindingType("r"), "bool");
}

TEST(TupleSemaTest, ADestructuringCopiesItsValue) {
  // Real bindings and not aliases (`tuples.md`, decision 6): writing through one
  // name does not touch the other or the product it came from.
  SemaFixture f;
  f.source("fn i32 main() {\n  let p = (1, 2);\n  let (a, b) = p;\n  a = 9;\n  return p.0;\n}\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u) << f.firstError().message;
}

TEST(TupleSemaTest, ASkippedPositionBindsNothing) {
  // `_` is the way to say \"this member is not wanted\": it introduces no binding,
  // so reading it is an unknown name and the member's copy is never made.
  SemaFixture f;
  f.source("fn i32 main() {\n  let (q, _) = (1, 2);\n  return q;\n}\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
  EXPECT_EQ(f.bindingType("q"), "i32");
  // The *type* of the position is recorded -- the checker typed the pattern -- but
  // no declaration is made, which is the decision: `_` is not a name a program can
  // read, so it must not be one a program can resolve either.
  bool skipDeclared = false;
  for (const resolve::Def& def : f.map().defs) {
    if (def.name != support::kInvalidSym && f.symbols().lookup(def.name) == "_") {
      skipDeclared = true;
    }
  }
  EXPECT_FALSE(skipDeclared);
  EXPECT_FALSE(f.hasResolveError("resolve-unknown-name"));
}

TEST(TupleSemaTest, ASkippedPositionIsNotANameAProgramCanRead) {
  SemaFixture f;
  f.source("fn i32 main() {\n  let (q, _) = (1, 2);\n  return _;\n}\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasResolveError("resolve-unknown-name"));
}

TEST(TupleSemaTest, ADestructuringWithTheWrongNumberOfNamesNamesBothNumbers) {
  expectError("fn i32 main() {\n  let (a, b) = (1, 2, 3);\n  return a;\n}\n",
              "sema-destructuring-arity",
              "this pattern has 2 names, and `(i32, i32, i32)` has 3 members");
}

TEST(TupleSemaTest, ADestructuringOfSomethingWithNoMembersNamesBothFixes) {
  {
    SemaFixture f;
    f.source("fn i32 main() {\n  let (a, b) = 5;\n  return a;\n}\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-destructuring-not-product"));
    EXPECT_NE(f.firstError().message.find("`i32` has no members to bind"), std::string::npos)
        << f.firstError().message;
  }
  // An annotation has to be a product of the arity the pattern has, and the
  // message says both numbers: the reader wrote one of them wrong and only they
  // know which.
  expectError("fn i32 main() {\n  let (a, b): i32 = 5;\n  return a;\n}\n",
              "sema-destructuring-not-product",
              "this binds 2 names, so the annotation has to be a product of 2");
}

TEST(TupleSemaTest, ADestructuringCanBeAnnotatedAndLeftUninitialized) {
  // `let (a, b): (i32, bool);` is two objects with no value, which is the same
  // declaration `let x: i32;` is -- and the definite-assignment pass is what
  // proves a read comes after a write.
  {
    SemaFixture f;
    f.source(
        "fn i32 main() {\n  let (a, b): (i32, bool);\n  a = 1;\n  b = true;\n  return a;\n}\n");
    ASSERT_TRUE(f.build());
    EXPECT_EQ(f.errorCount(), 0u);
    EXPECT_EQ(f.bindingType("a"), "i32");
    EXPECT_EQ(f.bindingType("b"), "bool");
  }
  expectError("fn i32 main() {\n  let (a, b): (i32, bool);\n  return a;\n}\n",
              "sema-use-before-assignment", "before");
}

TEST(TupleSemaTest, ADestructuringNeedsATypeOrAValue) {
  SemaFixture f;
  f.source("fn i32 main() {\n  let (a, b);\n  return 0;\n}\n");
  ASSERT_TRUE(f.build());
  // The pass that owns the shape reports it, and the checker adds nothing: a
  // pattern with no annotation and no value has nothing to take its members from,
  // and the sentence is the validator's.
  EXPECT_TRUE(f.hasAstError());
  EXPECT_EQ(f.errorCount(), 0u) << f.firstError().message;
}

TEST(TupleSemaTest, ADestructuringIsNotAFileScopeDeclaration) {
  // One object per name is what a global is, and the item table -- what every
  // later stage reads a file-scope declaration through -- records one name per
  // declaration (`tuples.md`, decision 6). Refused where the item is, with the fix.
  SemaFixture f;
  f.source("const (w, h) = (16, 9);\nfn i32 main() { return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasAstError());
  EXPECT_EQ(f.errorCount(), 0u); // and nothing about the initializer a second time
  ASSERT_FALSE(f.astErrors().empty());
  EXPECT_NE(f.astErrors().front().message.find("const W = 16; const H = 9;"), std::string::npos)
      << f.astErrors().front().message;
}

TEST(TupleSemaTest, AConstantDestructuringPublishesItsMembers) {
  // A `const` whose value is a literal, all of whose members are literals, gives
  // each name the same compile-time value a single `const` would -- which is what
  // makes `W` usable where a constant is required.
  SemaFixture f;
  f.source("fn i32 main() {\n  const (w, h) = (16, 9);\n  let x: i32 = w * h;\n  return x;\n}\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u) << f.firstError().message;
}

TEST(TupleSemaTest, ADuplicateNameInOnePatternIsARedeclaration) {
  SemaFixture f;
  f.source("fn i32 main() {\n  let (a, a) = (1, 2);\n  return a;\n}\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasResolveError("resolve-redeclaration"));
}

TEST(TupleSemaTest, AConstDestructuringIsNotAssignable) {
  SemaFixture f;
  f.source("fn i32 main() {\n  const (a, b) = (1, 2);\n  a = 3;\n  return b;\n}\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-assign-to-const"));
}

} // namespace minc::test
