// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// `type Name = T;`: one name, no type.
//
// The property this file is built on is the one the feature exists for
// (`type_alias.md`, decision 2): **a name for a type is transparent, so it is not
// in the type store.** Every test that could pass by accident somewhere else is
// phrased as an *identity* here -- the same `TypeId`, the same count in the store,
// the same module without debug information -- because that is the difference
// between an alias and a type that happens to look like one.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sema/sema_fixture.h"
#include "sema/typed_ast.h"

namespace minc::sema {
namespace {

using test::SemaFixture;

// Every node of the lowered tree, by index -- the whole tree, because the tests
// below ask about positions rather than about declarations, and a helper that
// visited only items could not see a block's `type` at all.
[[nodiscard]] std::vector<ast::AstId> nodesOf(const SemaFixture& f) {
  const std::span<const ast::Node> nodes(f.lowered().nodes());
  std::vector<ast::AstId> out;
  out.reserve(nodes.size());
  for (std::uint32_t index = 0; index < nodes.size(); ++index) {
    out.push_back(ast::AstId{index});
  }
  return out;
}

// The `Name` node of the `which`-th `type` declaration of the unit, in source
// order -- the file scope's first, then each block's as it is checked.
[[nodiscard]] ast::AstId aliasNameNode(const SemaFixture& f, std::size_t which = 0) {
  std::size_t seen = 0;
  for (const ast::AstId id : nodesOf(f)) {
    if (f.lowered().at(id).kind != ast::NodeKind::TypeAliasDecl) {
      continue;
    }
    if (seen++ != which) {
      continue;
    }
    return f.lowered().childOfKind(id, ast::NodeKind::Name);
  }
  return {};
}

// The first word a type position is written with, which is the spelling a reader
// sees at its left edge (`*A`, `[4]A`, `A`).
[[nodiscard]] std::string firstWordOf(const SemaFixture& f, ast::AstId typeNode) {
  for (const ast::AstId child : f.lowered().childrenOf(typeNode)) {
    const std::string_view written = f.lowered().spellingOf(child);
    if (!written.empty()) {
      return std::string(written);
    }
  }
  return {};
}

// --- the identity ------------------------------------------------------------

TEST(TypeAliasTest, TheNameAndTheExpansionAreTheSameType) {
  SemaFixture f;
  f.source("type Word = u32;\nfn i32 main() { let a: Word = 1; let b: u32 = 2; return a + b; }\n");
  ASSERT_TRUE(f.build());

  EXPECT_FALSE(f.hasError("sema-unknown-type"));
  EXPECT_EQ(f.errorCount(), 0u);
  // One type and not two: an alias that interned itself would make `a` and `b`
  // different types, and every rule that compares types -- assignment, a call, a
  // `switch` on one -- would refuse a program that names the same type twice.
  EXPECT_EQ(f.bindingType("a"), f.bindingType("b"));
  EXPECT_EQ(f.bindingType("a"), "u32");
}

TEST(TypeAliasTest, DeclaringANameDoesNotGrowTheTypeStore) {
  SemaFixture plain;
  plain.source("fn i32 main() { let a: u32 = 1; return a; }\n");
  ASSERT_TRUE(plain.build());

  SemaFixture named;
  named.source("type Word = u32;\ntype Pair = [2]u32;\ntype Ref = *Word;\n"
               "fn i32 main() { let a: Word = 1; let b: Pair = [1, 2];"
               " let p: Ref = &a; return a; }\n");
  ASSERT_TRUE(named.build());

  // The two units name the same types, so they hold the same number of them --
  // the aliases cost the store nothing. `Pair` and `Ref` add `[2]u32` and `*u32`,
  // and those are the same entries this unit would have if the annotations had
  // been written out.
  EXPECT_EQ(named.types().count(), plain.types().count() + 2);
}

TEST(TypeAliasTest, AnAliasOfAnAliasIsTheSameType) {
  SemaFixture f;
  f.source("type A = u64;\ntype B = A;\ntype C = *B;\n"
           "fn i32 main() { let x: C = null; let y: *u64 = null; x = y; return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
}

// --- order -------------------------------------------------------------------

TEST(TypeAliasTest, FileScopeOrderDoesNotMatter) {
  SemaFixture f;
  // The file scope is decided in dependency order, so a name may be used by a
  // declaration above the one that declares it -- which is what module-shaped code
  // needs, and what `globals.md` already does for values.
  f.source("type A = *B;\ntype B = i32;\nfn i32 main() { let p: A = null; let v: B = 1;"
           " return v; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
}

TEST(TypeAliasTest, BlockScopeIsReadInOrderAndTheNameLeavesWithTheBlock) {
  SemaFixture f;
  // Three spellings of one name: the file's is `u64`, the function's is `i32`, and
  // the inner block's is `f64`. Each binding below the declaration has to get the
  // nearest one, and the use after the inner block has to be back to `i32` -- so
  // the test proves both directions of the shadow rule, in one program.
  f.source("type S = u64;\nfn i32 main() {\n  type S = i32;\n  let a: S = 2;\n"
           "  {\n    type S = f64;\n    let b: S = 1.5;\n  }\n  let c: S = 3;\n"
           "  return a + c;\n}\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
  EXPECT_EQ(f.bindingType("a"), "i32");
  EXPECT_EQ(f.bindingType("b"), "f64");
  EXPECT_EQ(f.bindingType("c"), "i32");
}

TEST(TypeAliasTest, ABlockNameIsGoneAfterTheBlock) {
  SemaFixture f;
  f.source("fn i32 main() {\n  { type Local = i32; let inside: Local = 1; }\n"
           "  let outside: Local = 2;\n  return outside;\n}\n");
  ASSERT_TRUE(f.build());
  // One error and one error only: the use is outside the block that declared the
  // name, so it is unknown -- and the checker does not then cascade into the
  // `return` because the binding's type is already an error.
  EXPECT_EQ(f.errorCount(), 1u);
  EXPECT_TRUE(f.hasError("sema-unknown-type"));
}

TEST(TypeAliasTest, AUseAboveTheDeclarationInABlockIsNotAType) {
  SemaFixture f;
  f.source("fn i32 main() {\n  let x: Later = 1;\n  type Later = i32;\n  return x;\n}\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 1u);
  EXPECT_TRUE(f.hasError("sema-unknown-type"));
}

TEST(TypeAliasTest, ABlockNameMayHideAFileOneWithADifferentType) {
  SemaFixture f;
  // The *type* is what proves the shadow took effect: if the file's `f64` had
  // answered, this initializer would be refused.
  f.source("type N = f64;\nfn i32 main() {\n  type N = i32;\n  let v: N = 3;\n  return v;\n}\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
  EXPECT_EQ(f.bindingType("v"), "i32");
}

// --- circles -----------------------------------------------------------------

TEST(TypeAliasTest, AFileScopeCircleNamesThePathAndPrintsOneDiagnostic) {
  SemaFixture f;
  f.source("type A = B;\ntype B = C;\ntype C = A;\nfn i32 main() { return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 1u);
  EXPECT_TRUE(f.hasError("sema-type-alias-cycle"));
  // The *path* is the message: which of the three went in a circle is the question
  // a reader cannot answer from \"defined in terms of itself\" alone.
  const std::string& message = f.firstError().message;
  EXPECT_NE(message.find("A` -> `B` -> `C` -> `A"), std::string::npos);
}

TEST(TypeAliasTest, ACircleThroughAPointerIsStillACircle) {
  SemaFixture f;
  // `type P = *P;` is refused even though a pointer is a size that recursion could
  // have: a name for a type is an abbreviation, and an abbreviation that contains
  // itself has no expansion. Recursion needs a type that names *itself*, and that
  // is `struct`, which the language does not have yet (`type_alias.md`, 6).
  f.source("type P = *P;\nfn i32 main() { return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 1u);
  EXPECT_TRUE(f.hasError("sema-type-alias-cycle"));
}

TEST(TypeAliasTest, ACircleThroughAnArrayIsStillACircle) {
  SemaFixture f;
  f.source("type Row = [4]Row;\nfn i32 main() { return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 1u);
  EXPECT_TRUE(f.hasError("sema-type-alias-cycle"));
}

TEST(TypeAliasTest, ACircleOfOneNameIsTheSameSentence) {
  SemaFixture f;
  f.source("type A = A;\nfn i32 main() { return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 1u);
  EXPECT_TRUE(f.hasError("sema-type-alias-cycle"));
}

TEST(TypeAliasTest, ABlockCircleIsRefusedAndTheNameIsNotInScopeYet) {
  SemaFixture f;
  f.source("fn i32 main() { type A = *[4]A; return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 1u);
  EXPECT_TRUE(f.hasError("sema-type-alias-cycle"));
}

TEST(TypeAliasTest, ABlockNameMayPointAtAnOuterNameOfTheSameSpelling) {
  SemaFixture f;
  // With an outer `F` in scope the name means *that* one until the declaration
  // completes -- which is exactly what C's `typedef T T;` does, and the reason a
  // block is read in order rather than treated as a circle.
  f.source("type F = f64;\nfn i32 main() {\n  type F = *F;\n  let p: F = null;\n  return 0;\n}\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
  EXPECT_EQ(f.bindingType("p"), "*f64");
}

// --- one fault, one diagnostic ------------------------------------------------

TEST(TypeAliasTest, AReservedNameIsReportedOnceByTheResolver) {
  SemaFixture f;
  f.source("type i32 = i64;\nfn i32 main() { let v: i32 = 1; return v; }\n");
  ASSERT_TRUE(f.build());
  // The resolver owns the sentence (a type name is what makes `(T)x` a cast), the
  // checker only makes sure the name does not become usable -- and `i32` goes on
  // meaning `i32` for the rest of the unit, which is the property that keeps a
  // refused declaration from silently redefining the language.
  EXPECT_TRUE(f.hasResolveError("resolve-reserved-identifier"));
  EXPECT_EQ(f.errorCount(), 0u);
  EXPECT_EQ(f.bindingType("v"), "i32");
}

TEST(TypeAliasTest, ABrokenNameIsSilentWhereItIsUsed) {
  SemaFixture f;
  // The declaration is the fault, and it is reported there. Every use of the name
  // then answers \"understood, no type\" instead of \"unknown word\", so a unit
  // with one broken alias does not produce one error per use.
  f.source("type Bad = [0]i32;\nfn i32 main() { let a: Bad = 1; let b: Bad = 2; return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 1u);
  EXPECT_TRUE(f.hasError("sema-malformed-type"));
}

TEST(TypeAliasTest, AMalformedTargetIsReportedOnce) {
  SemaFixture f;
  f.source("type Bad = [n]i32;\nfn i32 main() { return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 1u);
}

// --- the name survives ---------------------------------------------------------

TEST(TypeAliasTest, ADiagnosticNamesTheWordTheSourceWrote) {
  SemaFixture f;
  f.source("type Arr = [8]u8;\nfn i32 main() { let w: u64 = 1; let a: Arr = w; return 0; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_EQ(f.errorCount(), 1u);
  // `Arr (aka `[8]u8`)`, which is clang's shape and the reason to copy it: an
  // expansion is right for *identity* and wrong for *reading* -- the reader repairs
  // a message about a word they wrote, not about a type they may never have
  // spelled out.
  const std::string& message = f.firstError().message;
  EXPECT_NE(message.find("`Arr (aka `[8]u8`)`"), std::string::npos);
}

TEST(TypeAliasTest, EveryUseOfANamePointsAtTheDeclarationItMeant) {
  SemaFixture f;
  // One use of the file's name (the global), one of the block's, and one position
  // written as a built-in: three positions, two declarations.
  f.source("type S = i32;\nlet g: S = 1;\nfn i32 main() {\n  type S = f64;\n  let a: S = 1.5;\n"
           "  let b: f64 = 2.5;\n  return 0;\n}\n");
  ASSERT_TRUE(f.build());
  ASSERT_EQ(f.errorCount(), 0u);

  // Two declarations of one spelling, and each position points at the one in
  // scope where it was written -- which is the fact the debug info names a
  // variable's type with and an editor's hover reads. A single map keyed on the
  // *spelling* would answer the block's name at the file's positions.
  std::size_t fileAlias = 0;
  std::size_t blockAlias = 0;
  const std::span<const TypeAliasInfo> aliases = f.typed().aliases();
  ASSERT_EQ(aliases.size(), 2u);
  const std::string firstName(f.lowered().spellingOf(aliasNameNode(f, 0)));
  const std::string secondName(f.lowered().spellingOf(aliasNameNode(f, 1)));
  EXPECT_EQ(firstName, "S");
  EXPECT_EQ(secondName, "S");

  for (const ast::AstId id : nodesOf(f)) {
    if (f.lowered().at(id).kind != ast::NodeKind::Type) {
      continue;
    }
    if (firstWordOf(f, id) != "S") {
      continue;
    }
    const std::uint32_t index = f.typed().aliasAt(id);
    ASSERT_NE(index, TypedFile::kNoAlias);
    const std::string spelling = f.types().spelling(aliases[index].type);
    if (index == 0) {
      ++fileAlias;
      EXPECT_EQ(spelling, "i32");
    } else {
      ++blockAlias;
      EXPECT_EQ(spelling, "f64");
    }
  }
  EXPECT_EQ(fileAlias, 1u);
  EXPECT_EQ(blockAlias, 1u);
}

TEST(TypeAliasTest, ADeclarationThatIsNeverUsedIsStillPublished) {
  SemaFixture f;
  f.source("type Unused = i32;\nfn i32 main() { return 0; }\n");
  ASSERT_TRUE(f.build());
  // The table is one entry per declaration and not one per *use*: a dump, an
  // editor, and a name-based rename all need the declaration, and a declaration a
  // program does not happen to use is still a declaration.
  ASSERT_EQ(f.typed().aliases().size(), 1u);
  EXPECT_EQ(f.types().spelling(f.typed().aliases().front().type), "i32");
}

} // namespace
} // namespace minc::sema
