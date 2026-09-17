// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// `type Name = T;` as a *name*: which namespace it lands in, which scope it
// belongs to, and what happens when it hides something.
//
// The checker reads the name; this stage is what makes it a definition an editor
// can find, rename and hover (`type_alias.md`, decision 3). The property that
// carries most of the weight here is the **namespace**: a name for a type goes in
// `Tag` and everything a program names goes in `Ordinary`, which is what lets the
// same spelling be both -- and what C needs `typedef struct T T;` for.
#include <string>

#include <gtest/gtest.h>

#include "resolve/def.h"
#include "resolve/resolve_fixture.h"
#include "resolve/scope.h"

namespace minc::resolve {
namespace {

using test::ResolveFixture;

TEST(TypeAliasResolveTest, TheNameIsADefinitionInTheTypeNamespace) {
  ResolveFixture fixture;
  fixture.source("type Word = u32;\nfn i32 main() { return 0; }\n");
  ASSERT_TRUE(fixture.build());

  const Def* const def = fixture.defNamed("Word");
  ASSERT_NE(def, nullptr);
  EXPECT_EQ(def->kind, DefKind::TypeAlias);
  EXPECT_EQ(def->ns, Namespace::Tag);
}

TEST(TypeAliasResolveTest, ATypeNameAndAValueMayShareASpelling) {
  ResolveFixture fixture;
  // Allowed, and it is the namespace that makes it allowed: the grammar decides by
  // *position* which one a name is, so `Word` before a `*` is the type and `Word`
  // in an expression is the value. C puts both in one namespace, which is why it
  // cannot write this program at all.
  fixture.source("type Word = u32;\n"
                 "fn u32 use(Word: u32) { return Word; }\n"
                 "fn i32 main() { return 0; }\n");
  ASSERT_TRUE(fixture.build());
  EXPECT_EQ(fixture.defCount("Word"), 2u);
}

TEST(TypeAliasResolveTest, ARepeatedNameIsARedeclaration) {
  ResolveFixture fixture;
  fixture.source("type Word = u32;\ntype Word = u64;\nfn i32 main() { return 0; }\n");
  ASSERT_TRUE(fixture.build());
  // One name, one declaration: a second is reported by the redeclaration rule and
  // not by the alias pass, so the sentence is the same one every other name gets.
  EXPECT_TRUE(fixture.hasResolveError("resolve-redeclaration"));
}

TEST(TypeAliasResolveTest, ATypeWordIsReserved) {
  ResolveFixture fixture;
  fixture.source("type i32 = i64;\nfn i32 main() { return 0; }\n");
  ASSERT_TRUE(fixture.build());
  // The refusal belongs here and not to the checker: the reason a type name is
  // reserved is that it is what makes `(T)x` a cast rather than a call, which is a
  // fact about *names* -- and one sentence about one mistake is the rule.
  EXPECT_TRUE(fixture.hasResolveError("resolve-reserved-identifier"));
}

TEST(TypeAliasResolveTest, TheFileScopeDeclarationIsVisibleBeforeItIsWritten) {
  ResolveFixture fixture;
  // The file scope has no order: `type A = B;` before `type B = i32;` is legal, and
  // this stage's part of it is that both names are defs of the file scope whatever
  // line they are on.
  fixture.source("type A = B;\ntype B = i32;\nfn i32 main() { return 0; }\n");
  ASSERT_TRUE(fixture.build());
  EXPECT_EQ(fixture.resolveErrorCodes().size(), 0u);
  const Def* const def = fixture.defNamed("B");
  ASSERT_NE(def, nullptr);
  EXPECT_EQ(def->kind, DefKind::TypeAlias);
}

TEST(TypeAliasResolveTest, ABlockNameIsDefinedInTheBlockAndHidesTheFileOne) {
  ResolveFixture fixture;
  fixture.source("type Word = u32;\n"
                 "fn i32 main() { type Word = i64; return 0; }\n");
  fixture.warnShadow();
  ASSERT_TRUE(fixture.build());

  EXPECT_EQ(fixture.defCount("Word"), 2u);
  EXPECT_TRUE(fixture.hasResolveWarning("resolve-shadowed-name"));
}

TEST(TypeAliasResolveTest, QuietWithoutTheFlag) {
  ResolveFixture fixture;
  fixture.source("type Word = u32;\n"
                 "fn i32 main() { type Word = i64; return 0; }\n");
  ASSERT_TRUE(fixture.build());
  // Shadowing is legal, and the warning is the flag's. A test that only ever asked
  // with `-Wshadow` on would not notice the day it started firing by itself.
  EXPECT_FALSE(fixture.hasResolveWarning("resolve-shadowed-name"));
}

TEST(TypeAliasResolveTest, ANameIsNotAValue) {
  ResolveFixture fixture;
  fixture.source("type Word = u32;\nfn u32 use() { return Word; }\nfn i32 main() { return 0; }\n");
  ASSERT_TRUE(fixture.build());
  // The other half of the namespace rule, spelled out: a use in a *value* position
  // finds no ordinary name -- and "unknown name" would be a lie, because the name
  // *is* declared, one namespace over. The sentence says which word would work and
  // the note points at the declaration, which is where a reader repairs it.
  ASSERT_EQ(fixture.resolved().errors.size(), 1u);
  const ResolveError& error = fixture.resolved().errors.front();
  EXPECT_EQ(toString(error.code), "resolve-unknown-name");
  EXPECT_NE(error.message.find("'Word' names a type"), std::string::npos) << error.message;
  EXPECT_NE(error.note.find("name for a type"), std::string::npos) << error.note;
  // And it is not counted as a *use* of the declaration: nothing read that name.
  EXPECT_EQ(fixture.defNamed("Word")->refCount, 0u);
}

} // namespace
} // namespace minc::resolve
