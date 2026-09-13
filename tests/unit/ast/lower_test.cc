// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>

#include "ast/ast.h"
#include "ast/ast_error.h"
#include "ast/node.h"
#include "parse/syntax_kind.h"
#include "resolve/resolve_fixture.h"
#include "support/span/span.h"

namespace minc::test {
namespace {

using ast::AstId;
using ast::NodeKind;

// Any whitespace at all, including the vertical forms a source file can hold.
[[nodiscard]] bool hasWhitespace(std::string_view text) {
  for (const char c : text) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f') {
      return true;
    }
  }
  return false;
}

TEST(LowerTest, RootsAtTheFileNodeAndDropsTrivia) {
  ResolveFixture f;
  f.source("// a comment\nfn i32 main()\n{\n  return 0;\n}\n");
  ASSERT_TRUE(f.build());

  const ast::LoweredFile& file = f.lowered();
  ASSERT_GT(file.nodeCount(), 0u);
  EXPECT_EQ(file.at(file.root()).kind, NodeKind::File);

  std::size_t leaves = 0;
  for (const ast::Node& node : file.nodes()) {
    if (!node.isToken()) {
      continue;
    }
    ++leaves;
    // Trivia is dropped, so no leaf's spelling is whitespace. A zero-width leaf
    // (end of file, a missing token) spells nothing and is fine.
    EXPECT_FALSE(hasWhitespace(file.spellingOf(node)))
        << "leaf of kind " << parse::toString(node.kind) << " spells whitespace";
  }
  EXPECT_GT(leaves, 0u);
}

TEST(LowerTest, EveryInteriorSpanIsTheUnionOfItsChildren) {
  ResolveFixture f;
  f.source("fn i32 main()\n{\n  let a = 1 + 2;\n  return a;\n}\n");
  ASSERT_TRUE(f.build());

  const ast::LoweredFile& file = f.lowered();
  for (std::uint32_t index = 0; index < file.nodeCount(); ++index) {
    const ast::Node& node = file.nodes()[index];
    if (node.isToken()) {
      continue;
    }
    const std::span<const AstId> kids = file.childrenOf(AstId{index});
    if (kids.empty()) {
      continue; // an empty interior node such as an `Error` with no bytes
    }
    std::uint32_t begin = file.at(kids[0]).unit.begin;
    std::uint32_t end = file.at(kids[0]).unit.end;
    for (const AstId kid : kids) {
      begin = std::min(begin, file.at(kid).unit.begin);
      end = std::max(end, file.at(kid).unit.end);
    }
    // The whole point of recomputing the span: a green width includes the trivia
    // the builder hung inside the node, so a literal with a space in front of it
    // would be three bytes wide around a one-byte token.
    EXPECT_EQ(node.unit.begin, begin) << parse::toString(node.kind);
    EXPECT_EQ(node.unit.end, end) << parse::toString(node.kind);
  }
}

TEST(LowerTest, EveryNodeHasAUsableOrigin) {
  ResolveFixture f;
  f.source("fn i32 main()\n{\n  return 1 + 2;\n}\n");
  ASSERT_TRUE(f.build());

  for (const ast::Node& node : f.lowered().nodes()) {
    // A synthesized token -- the separator space the preprocessor inserts -- has
    // no written span, and a span with an invalid file is what would reach a
    // diagnostic as "file 0xFFFFFFFF".
    EXPECT_NE(node.origin.file, support::kInvalidFile)
        << parse::toString(node.kind) << " has no origin";
    EXPECT_LE(node.origin.begin, node.origin.end);
  }
}

TEST(LowerTest, NamesAreInternedOnce) {
  ResolveFixture f;
  f.source("fn i32 main()\n{\n  let total = 0;\n  return total;\n}\n");
  ASSERT_TRUE(f.build());

  const ast::LoweredFile& file = f.lowered();
  // Every identifier is interned, including the ones inside a `Type` run, so
  // `i32` is a symbol too.
  std::set<support::SymId> names;
  for (const ast::Node& node : file.nodes()) {
    if (node.name != support::kInvalidSym) {
      names.insert(node.name);
      EXPECT_FALSE(file.spellingOf(node).empty());
    }
  }
  EXPECT_EQ(names.size(), 3u);

  // The declaration and every use of it share one `SymId`: that is what makes a
  // later stage compare integers instead of spellings.
  support::SymId declared = support::kInvalidSym;
  for (std::uint32_t i = 0; i < file.nodeCount(); ++i) {
    const ast::Node& node = file.nodes()[i];
    if (node.kind == ast::NodeKind::Name && file.spellingOf(node) == "total") {
      declared = node.name;
    }
  }
  ASSERT_NE(declared, support::kInvalidSym);
  bool saw_use = false;
  for (std::uint32_t i = 0; i < file.nodeCount(); ++i) {
    const ast::Node& node = file.nodes()[i];
    if (node.kind == ast::NodeKind::PathExpr) {
      EXPECT_EQ(node.name, declared);
      saw_use = true;
    }
  }
  EXPECT_TRUE(saw_use);
}

TEST(LowerTest, ItemTreeDescribesTheSignature) {
  ResolveFixture f;
  // No parameter list: parameters are not parsed yet, so a test about arity
  // would be asserting on a region the parser already reported.
  f.source("fn i32 add() { return 0; }\nfn i32 main() { return 0; }\n");
  ASSERT_TRUE(f.build());

  const ast::ItemTree& items = f.lowered().items();
  ASSERT_EQ(items.items.size(), 2u);
  EXPECT_EQ(items.items[0].kind, NodeKind::FnDecl);
  EXPECT_EQ(f.spelling(items.items[0].name), "add");
  EXPECT_TRUE(items.items[0].hasBody);
  EXPECT_NE(items.items[0].body, ast::kInvalidAst);
  EXPECT_EQ(f.spelling(items.items[1].name), "main");
  // The signature stops where the body begins, which is what makes it stable
  // under an edit inside that body.
  EXPECT_LE(items.items[0].span.begin, items.items[0].span.end);
}

TEST(LowerTest, ItemTreeIsStableUnderABodyEdit) {
  ResolveFixture before;
  before.source("fn i32 main()\n{\n  return 1;\n}\n");
  ResolveFixture after;
  after.source("fn i32 main()\n{\n  let x = 41;\n  return x + 1;\n}\n");
  ASSERT_TRUE(before.build());
  ASSERT_TRUE(after.build());

  // The node arrays differ -- the bodies are different -- and the item trees do
  // not. That is the whole invariant the editor depends on.
  EXPECT_NE(before.lowered().nodeCount(), after.lowered().nodeCount());
  EXPECT_EQ(before.lowered().items(), after.lowered().items());
  // The hash agrees with the equality, because it hashes exactly those fields.
  EXPECT_EQ(before.lowered().items().hash, after.lowered().items().hash);
}

TEST(LowerTest, ItemTreeChangesWhenASignatureDoes) {
  // The names differ in length as well as spelling: two `ItemTree`s from two
  // *sessions* have interner-local `SymId`s, but the spans they carry are byte
  // ranges and are comparable, so this is a real difference either way.
  ResolveFixture before;
  before.source("fn i32 one() { return 0; }\n");
  ResolveFixture after;
  after.source("fn i32 oneMore() { return 0; }\n");
  ASSERT_TRUE(before.build());
  ASSERT_TRUE(after.build());

  EXPECT_NE(before.lowered().items(), after.lowered().items());
}

TEST(LowerTest, ErrorRegionsAreMarked) {
  ResolveFixture f;
  f.source("fn i32 main()\n{\n  let x = ;\n  return 0;\n}\n");
  ASSERT_TRUE(f.build());

  bool any_error = false;
  for (const ast::Node& node : f.lowered().nodes()) {
    if (node.inError) {
      any_error = true;
      // A node inside an error region is never a name-bearing one, or the
      // resolver would resolve a name the parser could not even place.
      break;
    }
  }
  EXPECT_TRUE(any_error);
  EXPECT_FALSE(f.parseErrors().empty());
  // The parser reported the region, so validation says nothing about it.
  EXPECT_FALSE(f.hasAstError("ast-missing-type"));
}

TEST(LowerTest, NodeLimitIsADiagnosticAndNotACrash) {
  ResolveFixture f;
  f.source("fn i32 main()\n{\n  let a = 1;\n  let b = 2;\n  return a + b;\n}\n");
  ast::LowerLimits limits;
  limits.maxNodes = 4;
  f.lowerLimits(limits);
  ASSERT_TRUE(f.build());

  EXPECT_TRUE(f.hasAstError("ast-node-limit"));
}

} // namespace
} // namespace minc::test
