// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// A **generic alias**: `type Pair<T, K> = (T, K);` — the first half of
// `generics.md`, and the half that needs no instantiation machinery.
//
// The declaration reads its target *once*, with one row per binder pushed on the
// name stack the reader already had, and what it produces is a **template**: the
// target with `Param`s in it. A use with arguments substitutes into it, and the
// substitution is a type the store already has — so `Pair<i32, bool>` *is*
// `(i32, bool)`, the check is an id equality, and the file's last test is the one
// that says an alias is an abbreviation and not a new kind of type (decision 8).
//
// Every refusal here names the character or the number that is wrong: a generic
// name with no arguments, the wrong count, arguments on a name that takes none.
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "sema/sema_error.h"

#include "sema_fixture.h"

namespace minc::sema {
namespace {

using test::SemaFixture;

[[nodiscard]] std::string allMessages(const SemaFixture& fixture) {
  std::string out;
  for (const SemaError& error : fixture.errors()) {
    if (!out.empty()) {
      out += "; ";
    }
    out += std::string(toString(error.code));
    out += ": ";
    out += error.message;
  }
  return out;
}

[[nodiscard]] std::string firstTypeError(const SemaFixture& fixture) {
  for (const SemaError& error : fixture.errors()) {
    if (error.code == SemaErrorCode::MalformedType || error.code == SemaErrorCode::UnknownType) {
      return error.message;
    }
  }
  return {};
}

TEST(GenericAliasTest, AnInstantiationIsTheTypeItNames) {
  SemaFixture f;
  f.source("type Pair<T, K> = (T, K);\n"
           "fn i32 main() {\n"
           "  let p: Pair<i32, bool> = (1, true);\n"
           "  return p.0;\n"
           "}\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u) << allMessages(f);
  EXPECT_EQ(f.bindingType("p"), "(i32, bool)");
}

TEST(GenericAliasTest, TheSubstitutedTypeIsTheStructuralOne) {
  // The decision the feature rests on: an alias is an *abbreviation*. A binding
  // annotated with the generic name and one annotated with the expansion are the
  // same type, so the second accepts the first with no conversion in between.
  SemaFixture f;
  f.source("type Pair<T, K> = (T, K);\n"
           "fn i32 main() {\n"
           "  let p: Pair<i32, bool> = (1, true);\n"
           "  let q: (i32, bool) = p;\n"
           "  return q.0;\n"
           "}\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u) << allMessages(f);
  EXPECT_EQ(f.bindingType("q"), "(i32, bool)");
}

TEST(GenericAliasTest, ANestedUseNests) {
  SemaFixture f;
  f.source("type Pair<T, K> = (T, K);\n"
           "fn i32 main() {\n"
           "  let p: Pair<Pair<i32, i32>, bool> = ((1, 2), true);\n"
           "  let inner: Pair<i32, i32> = p.0;\n"
           "  return inner.1;\n"
           "}\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u) << allMessages(f);
  EXPECT_EQ(f.bindingType("p"), "((i32, i32), bool)");
  EXPECT_EQ(f.bindingType("inner"), "(i32, i32)");
}

TEST(GenericAliasTest, AConstructorAroundAUseWrapsTheSubstitutedType) {
  SemaFixture f;
  f.source("type Pair<T, K> = (T, K);\n"
           "fn i32 main() {\n"
           "  let p: Pair<i32, bool> = (1, true);\n"
           "  let q: *Pair<i32, bool> = &p;\n"
           "  return p.0;\n"
           "}\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u) << allMessages(f);
  EXPECT_EQ(f.bindingType("q"), "*(i32, bool)");
}

TEST(GenericAliasTest, AnArrayOfAParameterInstantiatesToARealArray) {
  // `[4]T` is a type whose *size* is the argument's: the template skips the
  // arithmetic, and `substitute` builds the result through the same `arrayOf`,
  // so the count and the element meet the checks on the type that gets lowered.
  SemaFixture f;
  f.source("type Rows<T> = [4]T;\n"
           "fn i32 main() {\n"
           "  let r: Rows<i32> = [1, 2, 3, 4];\n"
           "  return r[0];\n"
           "}\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u) << allMessages(f);
  EXPECT_EQ(f.bindingType("r"), "[4]i32");
}

TEST(GenericAliasTest, OneGenericAliasCanBeBuiltOnAnother) {
  SemaFixture f;
  f.source("type Vec<T> = []T;\n"
           "type Pair<T, K> = (T, K);\n"
           "type Item = Pair<Vec<u8>, u32>;\n"
           "fn i32 main() {\n"
           "  let i: *Item = null;\n"
           "  return 0;\n"
           "}\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u) << allMessages(f);
  EXPECT_EQ(f.bindingType("i"), "*([]u8, u32)");
}

TEST(GenericAliasTest, AUseInsideAProductMemberIsReadLikeAnyOtherUse) {
  SemaFixture f;
  f.source("type Vec<T> = []T;\n"
           "fn i32 main() {\n"
           "  let a: [4]u8 = [1, 2, 3, 4];\n"
           "  let mm: (Vec<u8>, bool) = (a[..], true);\n"
           "  return 0;\n"
           "}\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u) << allMessages(f);
  EXPECT_EQ(f.bindingType("mm"), "([]u8, bool)");
}

TEST(GenericAliasTest, TwoDeclarationsMayEachCallTheirBinderT) {
  // A binder's identity is `(owner, position)` and not its spelling, so two
  // declarations that both write `T` do not share a type -- and because an
  // instantiation is structural, `A<i32>` and `B<i32>` that name the same
  // expansion *are* one type. Both halves are asserted here.
  SemaFixture f;
  f.source("type A<T> = (T, T);\n"
           "type B<T> = (T, T);\n"
           "fn i32 main() {\n"
           "  let a: A<i32> = (1, 2);\n"
           "  let b: B<i32> = a;\n"
           "  return b.1;\n"
           "}\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u) << allMessages(f);
  EXPECT_EQ(f.bindingType("a"), "(i32, i32)");
  EXPECT_EQ(f.bindingType("b"), "(i32, i32)");
}

TEST(GenericAliasTest, AGenericNameWithNoArgumentsIsRefusedAtTheName) {
  SemaFixture f;
  f.source("type Pair<T, K> = (T, K);\n"
           "fn i32 main() {\n"
           "  let p: Pair = (1, true);\n"
           "  return 0;\n"
           "}\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-malformed-type")) << allMessages(f);
  const std::string message = firstTypeError(f);
  EXPECT_NE(message.find("is generic"), std::string::npos) << message;
  EXPECT_NE(message.find("`Pair<...>`"), std::string::npos) << message;
}

TEST(GenericAliasTest, TheWrongNumberOfArgumentsIsRefusedWithBothNumbers) {
  SemaFixture f;
  f.source("type Pair<T, K> = (T, K);\n"
           "fn i32 main() {\n"
           "  let p: Pair<i32> = (1, 2);\n"
           "  return 0;\n"
           "}\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-malformed-type")) << allMessages(f);
  const std::string message = firstTypeError(f);
  EXPECT_NE(message.find("takes 2 type arguments"), std::string::npos) << message;
  EXPECT_NE(message.find("1 were written"), std::string::npos) << message;
}

TEST(GenericAliasTest, ANameWithNoBindersTakesNoArguments) {
  SemaFixture f;
  f.source("type Q = i32;\n"
           "fn i32 main() {\n"
           "  let x: Q<i32> = 1;\n"
           "  return x;\n"
           "}\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-malformed-type")) << allMessages(f);
  EXPECT_NE(firstTypeError(f).find("takes no type arguments"), std::string::npos);
}

TEST(GenericAliasTest, AnUnknownNameWithArgumentsIsStillAnUnknownName) {
  SemaFixture f;
  f.source("fn i32 main() {\n"
           "  let x: Nope<i32> = 1;\n"
           "  return x;\n"
           "}\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-unknown-type")) << allMessages(f);
}

TEST(GenericAliasTest, ABinderIsNotANameOfTheUnit) {
  // The scope of a binder is its declaration: the rows the target is read with
  // are pushed for that read and dropped with it, so `T` outside the `type` that
  // introduced it is exactly the unknown word it looks like.
  SemaFixture f;
  f.source("type Pair<T, K> = (T, K);\n"
           "fn i32 main() {\n"
           "  let t: T = 1;\n"
           "  return t;\n"
           "}\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-unknown-type")) << allMessages(f);
}

TEST(GenericAliasTest, AnAliasWhoseTargetMentionsItselfIsStillACycle) {
  // The dependency walk sees the words inside the arguments too (`typeRunWords`),
  // so a circle written through a use is a circle: `Pair<T, K> = (Pair<T, K>, K)`
  // has no expansion, and the sentence is the one the table already had.
  SemaFixture f;
  f.source("type Pair<T, K> = (Pair<T, K>, K);\n"
           "fn i32 main() { return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-type-alias-cycle")) << allMessages(f);
}

TEST(GenericAliasTest, AnArgumentTheSubstitutedTargetRefusesIsASentence) {
  // The template is read without the array arithmetic -- `[4]T` has no size until
  // `T` does -- so the *substituted* type is where the target's own rules meet the
  // arguments, and a use that breaks them is refused where it was written. `void`
  // is the shortest argument that does it: a `void` element is a view of nothing.
  SemaFixture f;
  f.source("type Rows<T> = [4]T;\n"
           "fn i32 main() {\n"
           "  let r: Rows<void>;\n"
           "  return 0;\n"
           "}\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-malformed-type")) << allMessages(f);
  EXPECT_NE(firstTypeError(f).find("has no type"), std::string::npos) << firstTypeError(f);
}

TEST(GenericAliasTest, ADeclarationInstantiatedBelowItsOwnBoundIsAccepted) {
  // The count that is refused is the *argument's*, so `[18446744073709551615]T`
  // is a template the store accepts -- the element has no width and there is no
  // product to compute -- while `Rows<u64>` is the type that overflows and is
  // refused where it is written.
  SemaFixture f;
  f.source("type Rows<T> = [18446744073709551615]T;\n"
           "fn i32 main() {\n"
           "  let r: Rows<u64>;\n"
           "  return 0;\n"
           "}\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-malformed-type")) << allMessages(f);
  EXPECT_NE(firstTypeError(f).find("has no type"), std::string::npos) << firstTypeError(f);
}

} // namespace
} // namespace minc::sema
