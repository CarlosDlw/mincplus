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

TEST(ScopeTest, ParametersAreDefinitionsInTheFunctionScope) {
  ResolveFixture f;
  f.source("fn i32 add(a: i32, b: i32)\n{\n  return a + b;\n}\n");
  ASSERT_TRUE(f.build());

  const resolve::Def* a = f.defNamed("a");
  const resolve::Def* b = f.defNamed("b");
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(a->kind, resolve::DefKind::Parameter);
  EXPECT_EQ(b->kind, resolve::DefKind::Parameter);
  // The parameter list is a sibling of the body, so nothing in the body walk
  // would reach it. Asserting the scope is what proves the declaration step runs
  // before the walk and lands where a reader expects: with the body's top level,
  // which is the function scope itself.
  EXPECT_EQ(f.map().scope(a->scope).kind, ScopeKind::Function);
  EXPECT_EQ(a->linkage, resolve::Linkage::None);
  // Both uses in the body answered to the parameters and not to nothing.
  EXPECT_EQ(a->refCount, 1u);
  EXPECT_EQ(b->refCount, 1u);
}

// A parameter's type is a run of identifiers when it is one of the C spellings,
// and none of those words is a name. Resolution walks the tree looking for uses,
// so the words inside a `Type` are exactly the ones it must not offer to lookup:
// `unsigned` here is a type, not a variable somebody forgot to declare.
TEST(ScopeTest, AMultiWordParameterTypeIsNotANameUse) {
  ResolveFixture f;
  f.source("fn i32 f(x: unsigned long)\n{\n  return 0;\n}\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.defNamed("unsigned"), nullptr);
  EXPECT_EQ(f.defNamed("long"), nullptr);
  EXPECT_FALSE(f.hasResolveError("resolve-unknown-name"));
}

TEST(ScopeTest, DuplicateParameterNamesAreARedeclaration) {
  ResolveFixture f;
  f.source("fn i32 f(a: i32, a: i32)\n{\n  return a;\n}\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasResolveError("resolve-redeclaration"));
}

TEST(ScopeTest, AParameterAndALocalAtTheTopOfTheBodyCollide) {
  // They are in one scope, because the body block *is* the function scope. If
  // that were not so, this would silently shadow instead of colliding.
  ResolveFixture f;
  f.source("fn i32 f(a: i32)\n{\n  let a = 1;\n  return a;\n}\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasResolveError("resolve-redeclaration"));
}

TEST(ScopeTest, AForBindingLivesInALoopScope) {
  ResolveFixture f;
  f.source("fn i32 main()\n{\n  for let i = 0; i < 3; i = i + 1\n  {\n    let j = i;\n  }\n  "
           "return 0;\n}\n");
  ASSERT_TRUE(f.build());

  const resolve::Def* i = f.defNamed("i");
  ASSERT_NE(i, nullptr);
  EXPECT_EQ(i->kind, resolve::DefKind::Variable);
  const resolve::Scope& loop = f.map().scope(i->scope);
  EXPECT_EQ(loop.kind, ScopeKind::Loop);
  // The loop scope hangs off the function scope, so the initializer's binding
  // covers the condition, the step and the body -- and does not outlive them.
  EXPECT_EQ(f.map().scope(loop.parent).kind, ScopeKind::Function);
  // The body is a block inside the loop, which is why a declaration there can
  // shadow the loop's own binding without colliding.
  EXPECT_EQ(f.map().scope(f.defNamed("j")->scope).kind, ScopeKind::Block);
}

TEST(ScopeTest, AForBindingIsNotVisibleAfterTheLoop) {
  ResolveFixture f;
  f.source("fn i32 main()\n{\n  for let i = 0; i < 3; i = i + 1 {}\n  return i;\n}\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasResolveError("resolve-unknown-name"));
  // The use after the loop still got an answer -- a `NameRef` with a reason --
  // so "every use has an answer" holds even for the misuse.
  EXPECT_EQ(f.map().refs.size(), 4u);
}

TEST(ScopeTest, NestedLoopsGetAScopeEach) {
  ResolveFixture f;
  f.source("fn i32 main()\n{\n  for let i = 0; i < 1; i = i + 1\n  {\n    for let j = 0; j < 1; j "
           "= j + 1 {}\n  }\n  return 0;\n}\n");
  ASSERT_TRUE(f.build());

  const resolve::Def* i = f.defNamed("i");
  const resolve::Def* j = f.defNamed("j");
  ASSERT_NE(i, nullptr);
  ASSERT_NE(j, nullptr);
  EXPECT_EQ(f.map().scope(i->scope).kind, ScopeKind::Loop);
  EXPECT_EQ(f.map().scope(j->scope).kind, ScopeKind::Loop);
  EXPECT_NE(i->scope, j->scope); // the inner loop is a scope of its own
  // And the inner one is nested inside the outer, not beside it: its parent is
  // the inner loop's body block, whose parent is the outer loop's scope.
  const resolve::ScopeId innerParent = f.map().scope(j->scope).parent;
  EXPECT_EQ(f.map().scope(innerParent).kind, ScopeKind::Block);
  EXPECT_EQ(f.map().scope(innerParent).parent, i->scope);
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

// A macro that expands one argument into two names gives both declarations the
// *same* written location -- the argument they both came from -- so a written
// span identifies neither of them. The unit range is what tells them apart, and
// this is the case `Def::unitSpan` exists for: without it the second name's
// lookup answers with the first declaration, and it does so silently, because
// both declarations are legal.
TEST(ScopeTest, TwoNamesFromOneMacroArgumentAreTwoDeclarations) {
  ResolveFixture f;
  f.source("#define CONCAT(a, b) a ## b\n"
           "#define PAIR(b) let b: i32 = 1; let CONCAT(b, 2): i32 = 2;\n"
           "fn i32 main()\n"
           "{\n"
           "  PAIR(a)\n"
           "  return a + a2;\n"
           "}\n");
  ASSERT_TRUE(f.build());
  ASSERT_TRUE(f.ppErrors().empty());
  ASSERT_TRUE(f.parseErrors().empty());
  ASSERT_TRUE(f.astErrorCodes().empty());

  const resolve::Def* a = f.defNamed("a");
  const resolve::Def* a2 = f.defNamed("a2");
  ASSERT_NE(a, nullptr);
  ASSERT_NE(a2, nullptr);
  // One written location, two declarations: exactly the pair a written-span key
  // cannot tell apart.
  EXPECT_EQ(a->nameSpan, a2->nameSpan);
  // And two unit ranges, one token each, which is what does tell them apart.
  EXPECT_NE(a->unitSpan, a2->unitSpan);

  // The node lookup answers with the declaration each `Name` node belongs to,
  // and not with the first declaration that happens to share its location.
  std::size_t found = 0;
  for (std::uint32_t i = 0; i < f.lowered().nodeCount(); ++i) {
    const ast::AstId id{i};
    if (f.lowered().at(id).kind != ast::NodeKind::Name) {
      continue;
    }
    const std::optional<resolve::DefId> def = resolve::defOfNameNode(f.map(), f.lowered(), id);
    ASSERT_TRUE(def.has_value());
    const resolve::Def& declaration = f.map().def(*def);
    if (declaration.name != a->name && declaration.name != a2->name) {
      continue; // `main`, the only other declaration here
    }
    EXPECT_EQ(declaration.unitSpan, f.lowered().at(id).unit);
    ++found;
  }
  EXPECT_EQ(found, 2u);
}

} // namespace
} // namespace minc::test
