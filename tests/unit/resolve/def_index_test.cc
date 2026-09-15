// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The two questions every stage below `resolve` asks of a `DefMap`, and the
// table of names the language binds before any source is read.
//
// These tests exist because the answers were, until this file, spelled out three
// times -- in `sema`, in `ir`, and in `source_to_def` -- with a comment at each
// copy saying the others had to change with it. An equivalence test is what makes
// "one implementation" a fact rather than an intention: if a future edit gives
// the indexed rule and the scanned one different answers, this fails.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "resolve/def_index.h"
#include "resolve/predefined.h"
#include "resolve/resolve_fixture.h"
#include "resolve/source_to_def.h"

namespace minc::test {
namespace {

// Every `Name` node in a unit, both ways, and they have to be the same answer.
void expectTheTwoRulesAgree(ResolveFixture& fixture) {
  const resolve::DefIndex index(fixture.map());
  std::size_t names = 0;
  for (std::uint32_t i = 0; i < fixture.lowered().nodeCount(); ++i) {
    const ast::AstId id{i};
    if (fixture.lowered().at(id).kind != ast::NodeKind::Name) {
      continue;
    }
    ++names;
    EXPECT_EQ(index.defAtName(fixture.lowered(), id),
              resolve::defOfNameNode(fixture.map(), fixture.lowered(), id))
        << "at node " << i;
  }
  EXPECT_GT(names, 0u) << "the unit has no declarations to ask about";
}

TEST(DefIndexTest, TheIndexedRuleAndTheScannedRuleAgreeOnEveryNameNode) {
  ResolveFixture f;
  f.source("fn i32 twice(n: i32) { let a = n; let a2 = a; return a2; }\n"
           "fn i32 main() { let total = twice(1); return total; }\n");
  ASSERT_TRUE(f.build());
  expectTheTwoRulesAgree(f);
}

TEST(DefIndexTest, TheyAgreeWhenAMacroGivesTwoNamesOneWrittenLocation) {
  // The case a written-span key cannot tell apart: two declarations that were
  // one macro argument, so they share a written range and differ only in the
  // unit's text. This is the shape the unit-offset rule exists for.
  ResolveFixture f;
  f.source("#define CONCAT(a, b) a ## b\n"
           "#define PAIR(b) let b: i32 = 1; let CONCAT(b, 2): i32 = 2;\n"
           "fn i32 main()\n"
           "{\n"
           "  PAIR(a)\n"
           "  return a + a2;\n"
           "}\n");
  ASSERT_TRUE(f.build());
  const resolve::Def* first = f.defNamed("a");
  const resolve::Def* second = f.defNamed("a2");
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(first->nameSpan, second->nameSpan);
  EXPECT_NE(first->unitSpan, second->unitSpan);
  expectTheTwoRulesAgree(f);
}

TEST(DefIndexTest, TwoDeclarationsOfOneFunctionAnswerWithOneIdentity) {
  // `extern fn i32 f();` and `fn i32 f() { }` are one function, so both
  // declaration sites have to answer with one `DefId` -- the identity and not
  // the site. Two ids would be two types in `sema` and two symbols in the
  // backend, and LLVM renames the loser to `f.1`, leaving `f` declared and never
  // defined.
  ResolveFixture f;
  f.source("extern fn i32 f();\n"
           "fn i32 f() { return 7; }\n"
           "fn i32 main() { return f(); }\n");
  ASSERT_TRUE(f.build());

  const resolve::DefIndex index(f.map());
  std::optional<resolve::DefId> first;
  std::size_t sites = 0;
  for (std::uint32_t i = 0; i < f.lowered().nodeCount(); ++i) {
    const ast::AstId id{i};
    if (f.lowered().at(id).kind != ast::NodeKind::Name ||
        f.spelling(f.lowered().at(id).name) != "f") {
      continue;
    }
    const std::optional<resolve::DefId> def = index.defAtName(f.lowered(), id);
    ASSERT_TRUE(def.has_value()) << "at node " << i;
    if (!first.has_value()) {
      first = def;
    } else {
      EXPECT_EQ(*def, *first) << "at node " << i;
    }
    ++sites;
  }
  EXPECT_EQ(sites, 2u);
}

TEST(DefIndexTest, AReferenceResolvesToTheBindingItNames) {
  ResolveFixture f;
  f.source("fn i32 main() { let value = 3; return value; }\n");
  ASSERT_TRUE(f.build());

  const resolve::DefIndex index(f.map());
  const resolve::Def* value = f.defNamed("value");
  ASSERT_NE(value, nullptr);

  // The one reference in the unit is the use inside `return`, and it points at
  // the binding.
  ASSERT_EQ(f.map().refs.size(), 1u);
  const std::optional<resolve::DefId> target = index.targetAt(f.map().refs.front().unitSpan);
  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(f.spelling(f.map().def(*target).name), "value");

  // Both directions of the index agree: the declaration's own name node answers
  // with the same identity the reference did.
  const std::optional<resolve::DefId> declared = index.defAtUnitOffset(value->unitSpan);
  ASSERT_TRUE(declared.has_value());
  EXPECT_EQ(*declared, *target);

  // A position with nothing written at it is nullopt, not a neighbouring
  // declaration.
  EXPECT_FALSE(index.targetAt(support::Span{}).has_value());
  EXPECT_FALSE(index.defAtUnitOffset(support::Span{}).has_value());
}

TEST(PredefinedTest, TheTableIsTheListResolutionBinds) {
  ResolveFixture f;
  f.source("fn i32 main() { if true { return 0; } return 1; }\n");
  ASSERT_TRUE(f.build());

  // One definition per row, carrying the row's own kind -- not a boolean that
  // says "some name was predefined here".
  for (const resolve::PredefinedName& row : resolve::kPredefinedNames) {
    const resolve::Def* def = f.defNamed(row.spelling);
    ASSERT_NE(def, nullptr) << row.spelling;
    EXPECT_EQ(def->predefined, row.name) << row.spelling;
    EXPECT_TRUE(resolve::isPredefined(def->predefined)) << row.spelling;
    EXPECT_EQ(resolve::toString(row.name), row.spelling);
  }
  EXPECT_EQ(resolve::predefinedFromSpelling("true"), resolve::Predefined::True);
  EXPECT_EQ(resolve::predefinedFromSpelling("total"), resolve::Predefined::None);
  EXPECT_FALSE(resolve::isPredefined(resolve::Predefined::None));
}

TEST(PredefinedTest, ABindingShadowsAPredefinedNameAndIsNotOne) {
  // `true` is a value and not a keyword, so it can be shadowed like any other
  // name -- and the shadowing declaration must not come out predefined.
  ResolveFixture f;
  f.source("fn i32 main() { let true = 0; return true; }\n");
  ASSERT_TRUE(f.build());

  std::size_t predefinedCount = 0;
  std::size_t bindingCount = 0;
  for (const resolve::Def& def : f.map().defs) {
    if (f.spelling(def.name) != "true") {
      continue;
    }
    if (resolve::isPredefined(def.predefined)) {
      ++predefinedCount;
    } else {
      ++bindingCount;
    }
  }
  EXPECT_EQ(predefinedCount, 1u);
  EXPECT_EQ(bindingCount, 1u);
}

} // namespace
} // namespace minc::test
