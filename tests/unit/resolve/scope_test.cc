// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "ast/node.h"
#include "parse/syntax_kind.h"
#include "resolve/def.h"
#include "resolve/map.h"
#include "resolve/resolve.h"
#include "resolve/resolve_fixture.h"
#include "resolve/source_to_def.h"

namespace minc::test {
namespace {

using resolve::ScopeKind;

TEST(ScopeTest, ScopesNestAndTheBodyIsTheFunctionScope) {
  ResolveFixture f;
  f.source("fn i32 main()\n{\n  let a = 1;\n  {\n    let b = 2;\n    return b;\n  }\n}\n");
  ASSERT_TRUE(f.build());

  const resolve::DefMap& map = f.map();
  ASSERT_EQ(map.scopes.size(), 3u);
  EXPECT_EQ(map.scopes[0].kind, ScopeKind::File);
  EXPECT_FALSE(map.scopes[0].parent.valid());
  EXPECT_EQ(map.scopes[0].parent, resolve::kInvalidScopeId);

  // The function's body block *is* the function scope, as in C: there is no
  // second block scope wrapped around it.
  EXPECT_EQ(map.scopes[1].kind, ScopeKind::Function);
  EXPECT_EQ(map.scopes[1].parent.index, 0u);
  EXPECT_EQ(map.scopes[2].kind, ScopeKind::Block);
  EXPECT_EQ(map.scopes[2].parent.index, 1u);
}

TEST(ScopeTest, TheScopeChainAlwaysReachesTheFile) {
  ResolveFixture f;
  f.source(
      "fn i32 main()\n{\n  {\n    {\n      let deep = 1;\n      return deep;\n    }\n  }\n}\n");
  ASSERT_TRUE(f.build());

  const resolve::DefMap& map = f.map();
  std::size_t roots = 0;
  for (std::uint32_t i = 0; i < map.scopes.size(); ++i) {
    // Walking parents must terminate, and it must terminate at the file scope --
    // not at a cycle and not at a scope that does not exist.
    std::optional<resolve::ScopeId> current = resolve::ScopeId{i};
    std::size_t steps = 0;
    while (current.has_value()) {
      ASSERT_LT(steps, map.scopes.size()) << "the scope chain does not terminate";
      const resolve::Scope& scope = map.scopes[current->index];
      if (!scope.parent.valid()) {
        EXPECT_EQ(scope.kind, ScopeKind::File);
        ++roots;
        break;
      }
      current = scope.parent;
      ++steps;
    }
  }
  EXPECT_EQ(roots, map.scopes.size()); // every scope's chain reaches the one root
}

TEST(ScopeTest, DefinitionsLandInTheScopeThatDeclaresThem) {
  ResolveFixture f;
  f.source("fn i32 main()\n{\n  let a = 1;\n  {\n    let b = a;\n    return b;\n  }\n}\n");
  ASSERT_TRUE(f.build());

  const resolve::DefMap& map = f.map();
  const resolve::Def* a = f.defNamed("a");
  const resolve::Def* b = f.defNamed("b");
  const resolve::Def* main_def = f.defNamed("main");
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  ASSERT_NE(main_def, nullptr);

  EXPECT_EQ(map.scope(main_def->scope).kind, ScopeKind::File);
  EXPECT_EQ(main_def->kind, resolve::DefKind::Function);
  EXPECT_EQ(main_def->linkage, resolve::Linkage::External);
  EXPECT_EQ(map.scope(a->scope).kind, ScopeKind::Function);
  EXPECT_EQ(a->kind, resolve::DefKind::Variable);
  EXPECT_EQ(a->linkage, resolve::Linkage::None);
  EXPECT_EQ(map.scope(b->scope).kind, ScopeKind::Block);
  EXPECT_EQ(a->ns, resolve::Namespace::Ordinary);
}

TEST(ScopeTest, LookupIsInnermostOutwardAndLookupOuterSeesTheShadowed) {
  ResolveFixture f;
  f.source("fn i32 main()\n{\n  let x = 1;\n  {\n    let x = 2;\n    return x;\n  }\n}\n");
  ASSERT_TRUE(f.build());

  const resolve::DefMap& map = f.map();
  const resolve::Def* inner = nullptr;
  for (std::uint32_t i = 0; i < map.defs.size(); ++i) {
    if (f.spelling(map.defs[i].name) == "x") {
      inner = &map.defs[i]; // the last one: the inner declaration
    }
  }
  ASSERT_NE(inner, nullptr);
  const resolve::ScopeId inner_scope = inner->scope;
  EXPECT_EQ(map.scope(inner_scope).kind, ScopeKind::Block);

  const std::optional<resolve::DefId> hit =
      resolve::lookup(map, inner_scope, resolve::Namespace::Ordinary, inner->name);
  ASSERT_TRUE(hit.has_value());
  EXPECT_EQ(map.def(hit.value()).scope.index, inner_scope.index);

  // One more step down the chain is exactly what `-Wshadow` wants.
  const std::optional<resolve::DefId> shadowed =
      resolve::lookupOuter(map, inner_scope, resolve::Namespace::Ordinary, inner->name);
  ASSERT_TRUE(shadowed.has_value());
  EXPECT_NE(shadowed->index, hit->index);
  EXPECT_EQ(map.scope(map.def(shadowed.value()).scope).kind, ScopeKind::Function);

  // From the outermost scope there is nothing further out, so the shadow walk
  // ends there rather than reporting a second shadow.
  const resolve::Def& outer = map.def(shadowed.value());
  EXPECT_FALSE(
      resolve::lookupOuter(map, outer.scope, resolve::Namespace::Ordinary, outer.name).has_value());
}

TEST(ScopeTest, EveryNameUseInParsedCodeHasAnAnswer) {
  ResolveFixture f;
  f.source("fn i32 main()\n{\n  let a = 1;\n  return a + nope;\n}\n");
  ASSERT_TRUE(f.build());

  std::size_t uses = 0;
  for (const ast::Node& node : f.lowered().nodes()) {
    if (node.kind == ast::NodeKind::PathExpr && !node.inError) {
      ++uses;
    }
  }
  // Total: the count matches the uses, and the unresolved one is still there,
  // with its reason.
  EXPECT_EQ(f.map().refs.size(), uses);

  std::size_t unresolved = 0;
  for (const resolve::NameRef& ref : f.map().refs) {
    if (!ref.resolved()) {
      ++unresolved;
      EXPECT_EQ(ref.reason, resolve::UnresolvedReason::NotFound);
    }
  }
  EXPECT_EQ(unresolved, 1u);
}

TEST(ScopeTest, SourceToDefFindsTheDeclaration) {
  ResolveFixture f;
  f.source("fn i32 main()\n{\n  let total = 1;\n  return total;\n}\n");
  ASSERT_TRUE(f.build());

  const resolve::Def* total = f.defNamed("total");
  ASSERT_NE(total, nullptr);

  // A byte inside the declaration's name resolves to that declaration.
  const std::optional<resolve::DefId> at =
      resolve::defAt(f.map(), total->nameSpan.file, total->nameSpan.begin);
  ASSERT_TRUE(at.has_value());
  EXPECT_EQ(f.spelling(f.map().def(at.value()).name), "total");

  // And so does the name node, the way the item map asks it.
  bool found = false;
  for (std::uint32_t i = 0; i < f.lowered().nodeCount(); ++i) {
    const ast::AstId id{i};
    if (f.lowered().at(id).kind != ast::NodeKind::Name) {
      continue;
    }
    const std::optional<resolve::DefId> def = resolve::defOfNameNode(f.map(), f.lowered(), id);
    if (def.has_value() && f.spelling(f.map().def(*def).name) == "total") {
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

TEST(ScopeTest, ResolutionIsDeterministic) {
  const std::string source = "fn i32 main()\n{\n  let a = 1;\n  let b = a;\n  return missing;\n}\n";
  ResolveFixture first;
  first.source(source).warnUnused().warnShadow();
  ResolveFixture second;
  second.source(source).warnUnused().warnShadow();
  ASSERT_TRUE(first.build());
  ASSERT_TRUE(second.build());

  EXPECT_EQ(first.dump(), second.dump());
  EXPECT_EQ(first.resolveErrorCodes(), second.resolveErrorCodes());
  EXPECT_EQ(first.resolveWarningCodes(), second.resolveWarningCodes());
}

} // namespace
} // namespace minc::test
