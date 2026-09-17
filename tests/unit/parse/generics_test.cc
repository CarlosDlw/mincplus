// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The two list shapes `generics.md` adds to the grammar: the **binder list** of a
// declaration (`fn T identity<T>(v: T)`, `type Pair<T, K> = (T, K);`) and the
// **argument list** of a use (`Pair<i32, bool>`, `makePair::<i32, bool>(...)`).
//
// Two lists, one shape, one closer -- and a closer that is not always one token.
// `>>` is one token to the lexer (maximal munch) and two closures to the grammar,
// and that is the whole reason the file exists: the split has to happen in the
// *reader*, because a lexical rule cannot know how many lists are open, and it
// has to travel back up the type grammar's recursion rather than through a flag.
// Every spelling of the closer is pinned here, including the two that C++ still
// asks a reader to put a space into.
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "parse/parse_error.h"
#include "parse/parse_fixture.h"
#include "parse/syntax_kind.h"
#include "syntax/tree.h"

namespace minc::parse {
namespace {

using test::ParseFixture;

// The first *significant* token of a node: how a `Name`'s spelling is read, since
// a node's children are the leaf and the trivia around it.
[[nodiscard]] std::string firstTokenText(const syntax::SyntaxNode& node) {
  for (std::size_t i = 0; i < node.childCount(); ++i) {
    const syntax::NodeOrToken child = node.child(i);
    if (child.isToken() && !child.asToken().isTrivia()) {
      return std::string(child.asToken().text());
    }
  }
  return {};
}

// A node's own significant token children, joined. A type position is *tokens* --
// the constructors and the lists live inside one `Type` node (`parser.md`) -- so
// this is how a test asks what a run was written as without reaching for bytes.
[[nodiscard]] std::string tokenText(const syntax::SyntaxNode& node) {
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

// The first node of a kind anywhere under `node`, which is how a test reaches the
// binding inside a function body without spelling out the path to it.
[[nodiscard]] std::optional<syntax::SyntaxNode> findFirst(const syntax::SyntaxNode& node,
                                                          SyntaxKind kind) {
  if (node.kind() == kind) {
    return node;
  }
  for (std::size_t i = 0; i < node.childCount(); ++i) {
    const syntax::NodeOrToken child = node.child(i);
    if (child.isToken()) {
      continue;
    }
    if (std::optional<syntax::SyntaxNode> found = findFirst(child.asNode(), kind)) {
      return found;
    }
  }
  return std::nullopt;
}

// The names of a binder list, in the order they were written.
[[nodiscard]] std::vector<std::string> binderNames(const syntax::SyntaxNode& params) {
  std::vector<std::string> out;
  for (std::size_t i = 0; i < params.childCount(); ++i) {
    const syntax::NodeOrToken child = params.child(i);
    if (child.isToken() || child.asNode().kind() != SyntaxKind::Name) {
      continue;
    }
    out.push_back(firstTokenText(child.asNode()));
  }
  return out;
}

[[nodiscard]] std::size_t occurrences(const std::string& text, std::string_view needle) {
  std::size_t count = 0;
  for (std::size_t at = text.find(needle); at != std::string::npos;
       at = text.find(needle, at + needle.size())) {
    ++count;
  }
  return count;
}

// The one function declaration of a unit. Every input here has exactly one.
[[nodiscard]] syntax::SyntaxNode fnDecl(const ParseFixture& fixture) {
  return fixture.tree().root().childOfKind(SyntaxKind::FnDecl).value();
}

void expectLossless(const ParseFixture& fixture) {
  EXPECT_EQ(fixture.errorCount(), 0u) << fixture.errorMessages();
  EXPECT_TRUE(fixture.tree().validate());
  EXPECT_EQ(fixture.reconstruct(), fixture.source());
}

// One code, once, and every byte still in the tree: the property every refusal in
// this file has, checked in one place.
void expectOneError(const ParseFixture& fixture, ParseErrorCode code) {
  ASSERT_EQ(fixture.errorCount(), 1u) << fixture.errorMessages();
  EXPECT_EQ(fixture.tree().errors().front().code, code);
  EXPECT_EQ(fixture.reconstruct(), fixture.source());
}

// --- the binder list ----------------------------------------------------------

TEST(GenericsParseTest, AnAliasBindsItsParameters) {
  ParseFixture fixture("type Pair<T, K> = (T, K);\n");
  ASSERT_TRUE(fixture.built());
  expectLossless(fixture);

  const syntax::SyntaxNode decl =
      fixture.tree().root().childOfKind(SyntaxKind::TypeAliasDecl).value();
  const std::optional<syntax::SyntaxNode> params = decl.childOfKind(SyntaxKind::GenericParams);
  ASSERT_TRUE(params.has_value());
  EXPECT_EQ(binderNames(*params), (std::vector<std::string>{"T", "K"}));

  // The list is closed, and its own tokens are the two brackets and the commas:
  // the binders are `Name` *nodes*, which is what makes every later stage read a
  // binder with the code it already had for a name.
  EXPECT_EQ(tokenText(*params), "< , >");
}

TEST(GenericsParseTest, AFunctionsBindersComeAfterItsNameAndBeforeItsParameters) {
  ParseFixture fixture("fn T identity<T>(value: T) { return value; }\n");
  ASSERT_TRUE(fixture.built());
  expectLossless(fixture);

  const syntax::SyntaxNode decl = fnDecl(fixture);
  const std::optional<syntax::SyntaxNode> type = decl.childOfKind(SyntaxKind::Type);
  const std::optional<syntax::SyntaxNode> name = decl.childOfKind(SyntaxKind::Name);
  const std::optional<syntax::SyntaxNode> params = decl.childOfKind(SyntaxKind::GenericParams);
  ASSERT_TRUE(type.has_value());
  ASSERT_TRUE(name.has_value());
  ASSERT_TRUE(params.has_value());

  // The return type is a run of *words*, so anything between `fn` and the name
  // would be read as part of the type -- which is the reason the binders are
  // written after the name and not before it (`generics.md`, decision 1).
  EXPECT_EQ(tokenText(*type), "T");
  EXPECT_EQ(firstTokenText(*name), "identity");
  EXPECT_EQ(binderNames(*params), (std::vector<std::string>{"T"}));
}

TEST(GenericsParseTest, TheNameIsNotTheLastWordOfTheRunWhenTheReturnTypeHasArguments) {
  // `Vec<i32>` is one word of the run and `f` is the name: a scan that counted the
  // identifiers inside the arguments would read `i32` as the function's name.
  ParseFixture fixture("fn Vec<i32> f() { return 0; }\n");
  ASSERT_TRUE(fixture.built());
  expectLossless(fixture);

  const syntax::SyntaxNode decl = fnDecl(fixture);
  const std::optional<syntax::SyntaxNode> type = decl.childOfKind(SyntaxKind::Type);
  const std::optional<syntax::SyntaxNode> name = decl.childOfKind(SyntaxKind::Name);
  ASSERT_TRUE(type.has_value());
  ASSERT_TRUE(name.has_value());
  EXPECT_EQ(tokenText(*type), "Vec < i32 >");
  EXPECT_EQ(firstTokenText(*name), "f");
  EXPECT_FALSE(decl.childOfKind(SyntaxKind::GenericParams).has_value());
}

TEST(GenericsParseTest, ANestedListClosesWithOneGreaterGreater) {
  // `>>` is **one** token and two lists. A reader that split it in the lexer would
  // make one list per `<`-token and know nothing about how many are open; a reader
  // that refused it would ask the writer for a space (`generics.md`, decision 4).
  ParseFixture fixture("type M = Vec<Vec<i32>>;\n");
  ASSERT_TRUE(fixture.built());
  expectLossless(fixture);

  const std::string dump = fixture.dump(/*showTrivia=*/false);
  EXPECT_EQ(occurrences(dump, "TypeArgList"), 2u);
  // The `>>` stays **one** token in the tree: what the lexer says is what the
  // source says, and the two closures are what the grammar reads out of it. An
  // argument list that is a node of its own is what keeps the second closure from
  // being lost between the two lists.
  EXPECT_NE(dump.find("GreaterGreater@"), std::string::npos) << dump;
}

TEST(GenericsParseTest, ThreeNestedListsCloseWithGreaterGreaterAndGreater) {
  ParseFixture fixture("type N = Vec<Vec<Vec<i32>>>;\n");
  ASSERT_TRUE(fixture.built());
  expectLossless(fixture);

  const std::string dump = fixture.dump(/*showTrivia=*/false);
  EXPECT_EQ(occurrences(dump, "TypeArgList"), 3u);
  // Two closures from the `>>` and one from the `>` that follows it. The leading
  // space is what keeps ` Greater@` from also matching inside ` GreaterGreater@`.
  EXPECT_EQ(occurrences(dump, " GreaterGreater@"), 1u) << dump;
  EXPECT_EQ(occurrences(dump, " Greater@"), 1u) << dump;
}

TEST(GenericsParseTest, TheArgumentsAreTypesAndNotExpressions) {
  ParseFixture fixture("fn i32 main() { let p: Pair<i32, bool> = (1, true); return 0; }\n");
  ASSERT_TRUE(fixture.built());
  expectLossless(fixture);

  const syntax::SyntaxNode binding = findFirst(fnDecl(fixture), SyntaxKind::LetStmt).value();
  const std::optional<syntax::SyntaxNode> type = binding.childOfKind(SyntaxKind::Type);
  ASSERT_TRUE(type.has_value());
  const std::optional<syntax::SyntaxNode> args = type->childOfKind(SyntaxKind::TypeArgList);
  ASSERT_TRUE(args.has_value());

  std::size_t types = 0;
  for (std::size_t i = 0; i < args->childCount(); ++i) {
    const syntax::NodeOrToken child = args->child(i);
    if (!child.isToken() && child.asNode().kind() == SyntaxKind::Type) {
      ++types;
    }
  }
  EXPECT_EQ(types, 2u);
}

// --- the closer, in all four spellings ---------------------------------------

TEST(GenericsParseTest, AGreaterEqualClosesTheListAndTheBinding) {
  // `let p: Pair<i32, bool>= t;` -- the `>` and the `=` written together, which is
  // the spelling C++ has never accepted. The type reader reports the `=` by value
  // and the binding consumes it, so neither stage asks the writer for a space.
  ParseFixture fixture("fn i32 main() { let p: Pair<i32, bool>= t; return p.0; }\n");
  ASSERT_TRUE(fixture.built());
  expectLossless(fixture);

  const syntax::SyntaxNode binding = findFirst(fnDecl(fixture), SyntaxKind::LetStmt).value();
  EXPECT_TRUE(binding.childOfKind(SyntaxKind::Type).has_value());
  // The initializer is there, which is the whole point: the `=` was consumed as
  // the binding's own and not reported as a stray character.
  EXPECT_TRUE(findFirst(binding, SyntaxKind::PathExpr).has_value());
}

TEST(GenericsParseTest, AGreaterGreaterEqualClosesTwoListsAndTheBinding) {
  ParseFixture fixture("fn i32 main() { let v: Vec<Vec<i32>>= xs; return 0; }\n");
  ASSERT_TRUE(fixture.built());
  expectLossless(fixture);

  EXPECT_EQ(occurrences(fixture.dump(/*showTrivia=*/false), "TypeArgList"), 2u);
  const syntax::SyntaxNode binding = findFirst(fnDecl(fixture), SyntaxKind::LetStmt).value();
  EXPECT_TRUE(findFirst(binding, SyntaxKind::PathExpr).has_value());
}

TEST(GenericsParseTest, AnAliasClosesItsBinderListAndItsEqualTogether) {
  ParseFixture fixture("type Pair<T>= (T, T);\n");
  ASSERT_TRUE(fixture.built());
  expectLossless(fixture);

  const syntax::SyntaxNode decl =
      fixture.tree().root().childOfKind(SyntaxKind::TypeAliasDecl).value();
  EXPECT_EQ(binderNames(decl.childOfKind(SyntaxKind::GenericParams).value()),
            (std::vector<std::string>{"T"}));
  EXPECT_TRUE(decl.childOfKind(SyntaxKind::Type).has_value());
}

TEST(GenericsParseTest, AnExtraCloserIsReportedAtTheCharacter) {
  ParseFixture fixture("fn i32 main() { let x: A<B>> = 1; return 0; }\n");
  ASSERT_TRUE(fixture.built());
  expectOneError(fixture, ParseErrorCode::StrayTypeArgClose);

  // One sentence for one extra character -- and the `=` that follows it is still
  // read as the binding's, so the mistake does not become a second one.
  EXPECT_TRUE(findFirst(fnDecl(fixture), SyntaxKind::LiteralExpr).has_value());
}

TEST(GenericsParseTest, AMissingCloserNamesTheCharacterToType) {
  ParseFixture fixture("fn i32 main() { let x: A<i32 = 1; return 0; }\n");
  ASSERT_TRUE(fixture.built());
  expectOneError(fixture, ParseErrorCode::ExpectedTypeArgClose);
}

TEST(GenericsParseTest, AnEmptyListIsNamedRatherThanLeftToTheCloser) {
  ParseFixture binder("fn T f<>(v: T) { return v; }\n");
  ASSERT_TRUE(binder.built());
  expectOneError(binder, ParseErrorCode::ExpectedName);
  EXPECT_NE(binder.errorMessages().find("at least one name"), std::string::npos)
      << binder.errorMessages();

  ParseFixture argument("fn i32 main() { let x: A<> = 1; return 0; }\n");
  ASSERT_TRUE(argument.built());
  expectOneError(argument, ParseErrorCode::ExpectedType);
  EXPECT_NE(argument.errorMessages().find("<i32, bool>"), std::string::npos)
      << argument.errorMessages();
}

TEST(GenericsParseTest, AConstraintIsRefusedOnceAndNotIgnored) {
  // The slot is reserved and read *with* its refusal: a constraint that parsed and
  // was then ignored would be a declaration promising a guarantee no stage checks.
  ParseFixture fixture("type P<T: Ordered> = (T, T);\n");
  ASSERT_TRUE(fixture.built());
  expectOneError(fixture, ParseErrorCode::ConstraintNotRead);
  EXPECT_NE(fixture.errorMessages().find("<T>"), std::string::npos) << fixture.errorMessages();
}

// --- the argument list of a call ----------------------------------------------

TEST(GenericsParseTest, TheExplicitCallFormPutsItsArgumentsInTheCall) {
  ParseFixture fixture("fn i32 main() { return identity::<i32>(5); }\n");
  ASSERT_TRUE(fixture.built());
  expectLossless(fixture);

  const syntax::SyntaxNode call = findFirst(fnDecl(fixture), SyntaxKind::CallExpr).value();
  EXPECT_TRUE(call.childOfKind(SyntaxKind::TypeArgList).has_value());
  // The arguments of the *call* are still an `ArgList`: the two list children of
  // one call are told apart by kind and not by position.
  EXPECT_TRUE(call.childOfKind(SyntaxKind::ArgList).has_value());
}

TEST(GenericsParseTest, TheExplicitCallFormRequiresItsCall) {
  // `f::<i32>` on its own would name an instantiated function, and a function
  // value that carries arguments is a shape this language does not have.
  ParseFixture fixture("fn i32 main() { let x = identity::<i32>; return x; }\n");
  ASSERT_TRUE(fixture.built());
  expectOneError(fixture, ParseErrorCode::ExpectedToken);
  EXPECT_NE(fixture.errorMessages().find("f::<i32>(x)"), std::string::npos)
      << fixture.errorMessages();
}

} // namespace
} // namespace minc::parse
