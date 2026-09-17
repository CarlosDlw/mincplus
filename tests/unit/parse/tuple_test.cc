// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The two shapes `tuples.md` adds to the grammar: the **product type** `(T, U)` in
// a type position (`parseTypeGroup`, shared by every position a type can be in)
// and the **pattern** `(a, b)` on the left of a binding (`parseBindingPattern`).
//
// Both are groups, and the file checks the property that makes a group safe to
// add: the parser consumes it whole. A half-read group is a group the next
// construct re-reads as something else, which is why each malformed form has one
// message and a tree that still reconstructs the source byte for byte.
#include <cstddef>
#include <string>

#include <gtest/gtest.h>

#include "parse/parse_error.h"
#include "parse/parse_fixture.h"
#include "parse/syntax_kind.h"
#include "syntax/tree.h"

namespace minc::parse {
namespace {

using test::ParseFixture;

// The first *significant* token of a node, which is how a `Name`'s spelling is
// read: the node's children are the leaf and the trivia around it, and the trivia
// is not part of the name.
[[nodiscard]] std::string firstTokenText(const syntax::SyntaxNode& node) {
  for (std::size_t i = 0; i < node.childCount(); ++i) {
    const syntax::NodeOrToken child = node.child(i);
    if (child.isToken() && !child.asToken().isTrivia()) {
      return std::string(child.asToken().text());
    }
  }
  return {};
}

// The names of a pattern, in the order the value's members arrive in.
[[nodiscard]] std::vector<std::string> patternNames(const syntax::SyntaxNode& pattern) {
  std::vector<std::string> out;
  for (std::size_t i = 0; i < pattern.childCount(); ++i) {
    const syntax::NodeOrToken child = pattern.child(i);
    if (child.isToken() || child.asNode().kind() != SyntaxKind::Name) {
      continue;
    }
    out.emplace_back(firstTokenText(child.asNode()));
  }
  return out;
}

// The significant tokens of a node, joined. A type position is *tokens* -- the
// constructors are the `*`, the `[N]` and the `(T, U)` inside one `Type` node
// (`parser.md`) -- so this is how a test asks what a type run was written as
// without reaching for the source bytes.
[[nodiscard]] std::string significantText(const syntax::SyntaxNode& node) {
  std::string out;
  for (std::size_t i = 0; i < node.childCount(); ++i) {
    const syntax::NodeOrToken child = node.child(i);
    if (!child.isToken() || child.asToken().isTrivia()) {
      continue;
    }
    if (!out.empty()) {
      out += ' ';
    }
    out += std::string(child.asToken().text());
  }
  return out;
}

// How many times a spelling appears in a dump. `find` answers "does it contain",
// and the count is what tells a product *value* from a product *type* in one
// module -- which is the whole question two of the tests below ask.
[[nodiscard]] std::size_t occurrences(const std::string& text, std::string_view needle) {
  std::size_t count = 0;
  for (std::size_t at = text.find(needle); at != std::string::npos;
       at = text.find(needle, at + needle.size())) {
    ++count;
  }
  return count;
}

// The `TuplePattern` of the *first* binding in the unit that has one, which every
// input here has exactly one of.
[[nodiscard]] syntax::SyntaxNode firstPattern(const ParseFixture& fixture) {
  const syntax::SyntaxNode root = fixture.tree().root();
  for (std::size_t i = 0; i < root.childCount(); ++i) {
    const syntax::NodeOrToken child = root.child(i);
    if (child.isToken() || child.asNode().kind() != SyntaxKind::FnDecl) {
      continue;
    }
    const syntax::SyntaxNode body = child.asNode().childOfKind(SyntaxKind::Block).value();
    for (std::size_t j = 0; j < body.childCount(); ++j) {
      const syntax::NodeOrToken stmt = body.child(j);
      if (stmt.isToken() || stmt.asNode().kind() != SyntaxKind::LetStmt) {
        continue;
      }
      return stmt.asNode().childOfKind(SyntaxKind::TuplePattern).value();
    }
  }
  return {};
}

void expectLossless(const ParseFixture& fixture) {
  EXPECT_EQ(fixture.errorCount(), 0u) << fixture.errorMessages();
  EXPECT_TRUE(fixture.tree().validate());
  EXPECT_EQ(fixture.reconstruct(), fixture.source());
}

TEST(TupleParseTest, APatternIsOneNameNodePerPosition) {
  const ParseFixture fixture("fn i32 main() {\n  let (q, r) = divmod(7, 2);\n  return q + r;\n}\n");
  ASSERT_TRUE(fixture.built());
  expectLossless(fixture);

  const syntax::SyntaxNode pattern = firstPattern(fixture);
  ASSERT_TRUE(pattern.kind() == SyntaxKind::TuplePattern);
  // The names are `Name` nodes and not bare identifiers: every stage above reads a
  // pattern's names with the code it already had for one binding (`tuples.md`,
  // decision 6), and that code asks for a `Name`.
  EXPECT_EQ(patternNames(pattern), (std::vector<std::string>{"q", "r"}));
}

TEST(TupleParseTest, ASkippedPositionIsANameLikeAnyOther) {
  // `_` is an `Identifier` to the scanner, so the pattern holds it in the same
  // shape as a name. Whether it *binds* is a name-resolution question, and it is
  // answered there -- the grammar has one form and the resolver has the rule.
  const ParseFixture fixture("fn i32 main() {\n  let (q, _) = pair();\n  return q;\n}\n");
  ASSERT_TRUE(fixture.built());
  expectLossless(fixture);
  EXPECT_EQ(patternNames(firstPattern(fixture)), (std::vector<std::string>{"q", "_"}));
}

TEST(TupleParseTest, TheAnnotationAndTheInitializerFollowThePattern) {
  // The three parts of a binding, in the order the grammar writes them: the names,
  // the annotation and the value. A pattern is the first part and nothing else --
  // which is why the two spellings share one production and one terminator.
  const ParseFixture fixture(
      "fn i32 main() {\n  let (a, b): (i32, bool) = (1, true);\n  return a;\n}\n");
  ASSERT_TRUE(fixture.built());
  expectLossless(fixture);

  const syntax::SyntaxNode root = fixture.tree().root();
  const syntax::SyntaxNode fn = root.childOfKind(SyntaxKind::FnDecl).value();
  const syntax::SyntaxNode stmt =
      fn.childOfKind(SyntaxKind::Block).value().childOfKind(SyntaxKind::LetStmt).value();
  EXPECT_TRUE(stmt.childOfKind(SyntaxKind::TuplePattern).has_value());
  EXPECT_TRUE(stmt.childOfKind(SyntaxKind::Type).has_value());
  // And the product type in the annotation is one `Type` node whose members are
  // runs inside it -- the same node kind every other type position builds.
  const syntax::SyntaxNode annotation = stmt.childOfKind(SyntaxKind::Type).value();
  EXPECT_EQ(significantText(annotation), "( i32 , bool )");
}

TEST(TupleParseTest, APatternWithNoNameIsOneMessageAndStillReconstructs) {
  // The name slot, in the out-of-order spelling: the commas are there and the
  // names are not. One message for the whole position (and not one per comma), and
  // the group is closed anyway so the rest of the binding still parses.
  const ParseFixture fixture("fn i32 main() {\n  let (, b) = pair();\n  return b;\n}\n");
  ASSERT_TRUE(fixture.built());
  // One message for the position and no cascade: the group is skipped to its own
  // `)` and the initializer is still read, so the reader repairs one character.
  EXPECT_EQ(fixture.errorCount(), 1u) << fixture.errorMessages();
  EXPECT_NE(fixture.errorMessages().find("a pattern lists the names"), std::string::npos)
      << fixture.errorMessages();
  EXPECT_EQ(fixture.reconstruct(), fixture.source());
}

TEST(TupleParseTest, ATrailingCommaIsNotAPosition) {
  // `(a, b,)` would be a pattern whose arity depends on a rule about commas, and
  // the arity is the one number this node is about. Refused by name, and the group
  // still closes so the initializer is read.
  const ParseFixture fixture("fn i32 main() {\n  let (a, b,) = pair();\n  return a;\n}\n");
  ASSERT_TRUE(fixture.built());
  EXPECT_EQ(fixture.errorCount(), 1u) << fixture.errorMessages();
  EXPECT_NE(fixture.errorMessages().find("a pattern lists the names"), std::string::npos)
      << fixture.errorMessages();
  EXPECT_EQ(fixture.reconstruct(), fixture.source());
}

TEST(TupleParseTest, AProductTypeWithNoClosingGroupNamesTheCharacter) {
  const ParseFixture fixture("fn i32 main() {\n  let p: (i32, bool = (1, true);\n  return 0;\n}\n");
  ASSERT_TRUE(fixture.built());
  EXPECT_EQ(fixture.errorCount(), 1u) << fixture.errorMessages();
  EXPECT_NE(fixture.errorMessages().find(toString(ParseErrorCode::ExpectedTypeGroupClose)),
            std::string::npos)
      << fixture.errorMessages();
  EXPECT_EQ(fixture.reconstruct(), fixture.source());
}

TEST(TupleParseTest, ADecimalLiteralBeginsWithADigit) {
  // The one rule the product *took away*: `.5` was a number while a `.` after a
  // value was nothing, and now the `.` after a value reads a member. A point may
  // still begin a **hex** float, where the characters before it are a base prefix
  // and there is no value for a member to be read from.
  {
    const ParseFixture fixture("fn i32 main() {\n  let x = .5;\n  return 0;\n}\n");
    ASSERT_TRUE(fixture.built());
    EXPECT_EQ(fixture.errorCount(), 1u) << fixture.errorMessages();
    EXPECT_NE(fixture.errorMessages().find(toString(ParseErrorCode::LeadingPointNumber)),
              std::string::npos)
        << fixture.errorMessages();
    EXPECT_EQ(fixture.reconstruct(), fixture.source());
  }
  {
    const ParseFixture fixture("fn i32 main() {\n  let x = 0x.8p3;\n  return 0;\n}\n");
    ASSERT_TRUE(fixture.built());
    expectLossless(fixture);
  }
}

TEST(TupleParseTest, AMemberIsOneFieldExprWhateverTheMemberIs) {
  // `t.0` today and `s.field` when a named product exists: one node kind, so the
  // checker asks one question and the lowering has one case to write
  // (`tuples.md`, decision 9). Two things a test has to hold: the position stays a
  // *token* in the tree (so nothing has to fold a spelling into a number twice),
  // and the group of a call is not a product.
  const ParseFixture fixture("fn i32 main() {\n  let t = (1, true);\n  return t.0;\n}\n");
  ASSERT_TRUE(fixture.built());
  expectLossless(fixture);
  const std::string dump = fixture.dump(/*showTrivia=*/false);
  EXPECT_EQ(occurrences(dump, "FieldExpr"), 1u) << dump;
  // One `TupleExpr` and it is the *value* the binding was initialized with: a
  // member read is not a product, and the two are told apart by their kinds.
  EXPECT_EQ(occurrences(dump, "TupleExpr"), 1u) << dump;
}

} // namespace
} // namespace minc::parse
