// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The type after `as` -- the one type position an expression surrounds, and the
// only one where a type has to be told from the expression it sits in.
//
// Two things are pinned here, and they are the two the design turns on:
//
//   1. **the extent.** A cast's type is read up to where the expression begins, so
//      `x as i32 * 2` is a multiplication and `x as i32 < 3` is a comparison. The
//      scan answers this before the reader runs, and the reader is then bounded --
//      which is why this file checks the *tree* and not only that no error came out:
//      a bound that is one token too long still parses, it just parses something
//      else.
//   2. **the `<`.** `W < ... > ...` is two programs, and the tokens that decide
//      between them are the ones after the closer (`type_scan.h`). Every case in the
//      table below is a spelling where one of the two readings would be a type
//      position accepting something the rest of the language refuses, or refusing
//      something it accepts (`casts.md`, decisions 18 and 19).
//
// Nothing here is about *conversions*: whether `f64` may become `i32` is `sema`'s
// question, and it is asked in `sema/cast_test.cc`. What a cast may name is this
// file's, and the answer is "the same type anything else may name".
#include <cstddef>
#include <string>

#include <gtest/gtest.h>

#include "parse/parse_fixture.h"
#include "parse/syntax_kind.h"

namespace minc::parse {
namespace {

using test::ParseFixture;

[[nodiscard]] std::size_t occurrences(const std::string& haystack, const std::string& needle) {
  std::size_t count = 0;
  for (std::size_t at = haystack.find(needle); at != std::string::npos;
       at = haystack.find(needle, at + needle.size())) {
    ++count;
  }
  return count;
}

// Parse one source and hold the two invariants every case below shares: nothing
// was reported, and the tree still **is** the source. The second is the one that
// catches an extent bug -- a cast that ate one token too many or too few still
// parses, and the reconstruction is what notices.
[[nodiscard]] std::string parseClean(const std::string& source) {
  ParseFixture fixture(source);
  EXPECT_EQ(fixture.errorMessages(), "") << source;
  EXPECT_EQ(fixture.reconstruct(), source) << source;
  return fixture.dump(/*showTrivia=*/false);
}

// Was the `<` after the word read as an argument list? One question, asked of the
// tree rather than of the bytes: a list is a `TypeArgList` node, and the words of a
// use are inside it.
[[nodiscard]] bool readTheList(const std::string& dump) {
  return occurrences(dump, "TypeArgList@") != 0;
}

// Was it read as the comparison operator? Then the node is a binary expression and
// no list exists anywhere in the tree.
[[nodiscard]] bool readTheComparison(const std::string& dump) {
  return occurrences(dump, "TypeArgList@") == 0 && occurrences(dump, "BinaryExpr@") != 0;
}

// --- every shape a type can have ----------------------------------------------

TEST(CastTypeTest, EveryTypeShapeIsReadAfterAs) {
  const std::string prologue = "type Pair<T, K> = (T, K);\ntype Vec<T> = [2]T;\n";
  struct Case {
    std::string cast;
    std::string what;
  };
  const Case cases[] = {
      {"let a = x as i32;", "a primitive"},
      {"let a = x as *i32;", "a pointer"},
      {"let a = x as [4]i32;", "a counted array"},
      {"let a = x as []i32;", "a slice"},
      {"let a = x as (i32, bool);", "a product, written out"},
      {"let a = x as Pair<i32, bool>;", "a product through its name"},
      {"let a = x as Vec<f64>;", "a use"},
      {"let a = x as Vec<Vec<f64>>;", "a use of a use"},
      {"let a = x as *Vec<f64>;", "a pointer to a use"},
      {"let a = x as *[4]i32;", "a pointer to an array"},
      {"let a = x as !;", "the bottom type"},
  };
  for (const Case& one : cases) {
    // The operand is a product of every member type above, so the *parse* is the
    // only thing under test: whether the conversion is legal is another file's
    // question, and `let` with no annotation is what keeps this one out of it.
    const std::string source =
        prologue + "fn i32 main() {\n  let x = (1, 2);\n  " + one.cast + "\n  return 0;\n}\n";
    const std::string dump = parseClean(source);
    EXPECT_TRUE(occurrences(dump, "CastExpr@") != 0) << one.what;
  }
}

TEST(CastTypeTest, AUseInsideAGenericBodyReadsTowardsItsOwnBinders) {
  // The `ztests/main.mx` shape, and the one that started this: `(l, r) as Pair<T,
  // K>` inside a generic function. `T` and `K` are words like any other, so the
  // list is read by the same rule -- there is no binder-aware case in the parser,
  // and this test is what says so.
  const std::string source = "type Pair<T, K> = (T, K);\n"
                             "fn Pair<T, K> swap<T, K>(l: T, r: K) {\n"
                             "  return (l, r) as Pair<T, K>;\n"
                             "}\n"
                             "fn i32 main() { return 0; }\n";
  const std::string dump = parseClean(source);
  EXPECT_EQ(occurrences(dump, "TypeArgList@"), 2u); // the return type and the cast
}

// --- the extent: where the type stops ----------------------------------------

TEST(CastTypeTest, AStarAfterTheWordsIsAMultiplication) {
  // The regression that a cast with a reader of its own would have: a run that kept
  // reading takes `*` for a pointer constructor, and `i32 *` is a type the reader
  // refuses by name -- so a legal multiplication turned into a type error.
  const std::string dump = parseClean("fn i32 main() { let x: i32 = 1; return x as i32 * 2; }\n");
  EXPECT_TRUE(occurrences(dump, "BinaryExpr@") != 0);
  EXPECT_FALSE(occurrences(dump, "CastToProduct@") != 0);
}

TEST(CastTypeTest, AWordAfterTheTypeIsNotPartOfIt) {
  // `Foo(i32)` stops after `Foo`: a group after a word is not a type in this
  // language (a type is constructors, then *one* word, then its list), and the
  // sentence the reader gives is about the token it could not use.
  ParseFixture fixture("fn i32 main() { let x = 1 as Foo(i32); return 0; }\n");
  EXPECT_TRUE(fixture.hasErrors());
  EXPECT_NE(fixture.errorMessages().find("parse-expected-token"), std::string::npos)
      << fixture.errorMessages();
}

// --- the `<`: a list, or the comparison it also is -----------------------------

TEST(CastTypeTest, APrimitiveTakesNoArgumentsSoTheLessIsAComparison) {
  // The case Rust reads in the other direction: `i32` is a reserved type word, and
  // a word that can never take arguments has nothing to decide, so the reader does
  // not have to look ahead at all.
  const std::string less[] = {
      "let a = x as i32 < 3;",
      "let a = x as i64 < 3;",
      "let a = x as u8 < y;",
      "let a = x as i32 < y > 2;", // a chain, and the parser still reads a comparison
  };
  for (const std::string& cast : less) {
    const std::string dump = parseClean("fn i32 main() { let x: i32 = 1; let y: i32 = 2;\n  " +
                                        cast + "\n  return 0;\n}\n");
    EXPECT_TRUE(readTheComparison(dump)) << cast;
  }
}

TEST(CastTypeTest, TheContentsDecideWhenTheWordCouldTakeArguments) {
  // `Foo` may take arguments, so the tokens between the brackets are what settle
  // it. Each of these is a program a reader wrote on purpose, and in every one of
  // them the list reading would be a type nobody meant.
  const std::string comparisons[] = {
      "let a = x as Foo < 3 > 2;",       // a number is not a type
      "let a = x as Foo < y + 1 > 2;",   // an operator is not a type
      "let a = x as Foo < y as u8 > 2;", // a cast is not a type
  };
  for (const std::string& cast : comparisons) {
    const std::string dump =
        parseClean("type Foo = i32;\nfn i32 main() { let x: Foo = 1; let y: Foo = 2;\n  " + cast +
                   "\n  return 0;\n}\n");
    EXPECT_TRUE(readTheComparison(dump)) << cast;
  }
}

TEST(CastTypeTest, WhatFollowsTheCloserDecidesWhenTheContentsLookLikeAType) {
  // `Foo < y > 2` is where the vocabulary is satisfied and only the *shape of the
  // expression* is left: `y` is a word, so it could be an argument, and `2` after
  // the `>` cannot follow a complete cast -- this grammar has no juxtaposition. So
  // it is the chain it looks like, and the sentence names the chain.
  const std::string dump =
      parseClean("type Foo = i32;\nfn i32 main() { let x: Foo = 1; let y: Foo = 2;\n  let a = x "
                 "as Foo < y > 2;\n  return 0;\n}\n");
  EXPECT_TRUE(readTheComparison(dump));
}

TEST(CastTypeTest, AStrayGreaterGreaterWithAnExpressionAfterItIsAShift) {
  // `(x as Foo) < (y >> 2)`: `>>` binds tighter than `<`, so this is a legal
  // program and the `>>` is not a closer. It is the reason the stray rule asks
  // what follows instead of trusting the count.
  const std::string dump =
      parseClean("type Foo = i32;\nfn i32 main() { let x: Foo = 1; let y: i32 = 2;\n  let a = x as "
                 "Foo < y >> 2;\n  return 0;\n}\n");
  EXPECT_TRUE(readTheComparison(dump));
}

TEST(CastTypeTest, AGreaterGreaterThatClosesEveryListStaysTheCloser) {
  // The other side of the same token: `Vec<Vec<f64>>` opens two lists and the
  // compound closer closes exactly those two, so nothing is stray and the type is
  // read whole. The count is what tells the two apart, and it is depth, not
  // spelling.
  const std::string dump = parseClean(
      "type Vec<T> = [2]T;\nfn i32 main() { let x: Vec<Vec<f64>> = [[2]f64{1.0, 2.0}, [2]f64{3.0, "
      "4.0}];\n  let a = x as Vec<Vec<f64>>;\n  return 0;\n}\n");
  EXPECT_EQ(occurrences(dump, "TypeArgList@"), 4u); // two in the annotation, two in the cast
  EXPECT_EQ(occurrences(dump, "BinaryExpr@"), 0u);
}

TEST(CastTypeTest, AListIsFollowedByTheExpressionItBelongsTo) {
  // After a list the type is complete and the expression continues: a comparison
  // (`>`), a multiplication, and equality are all operators on the cast's result.
  const std::string prologue = "type Vec<T> = [2]T;\n";
  const std::string tail =
      "fn i32 main() { let v: Vec<i32> = [2]i32{1, 2};\n  %s\n  return 0;\n}\n";
  const std::string operators[] = {
      "let a = v as Vec<i32> * 2;",
      "let a = v as Vec<i32> == v;",
      "let a = v as Vec<i32> < v;",
  };
  for (const std::string& cast : operators) {
    std::string source = tail;
    source.replace(source.find("%s"), 2, cast);
    const std::string dump = parseClean(prologue + source);
    EXPECT_TRUE(readTheList(dump)) << cast;
    EXPECT_TRUE(occurrences(dump, "BinaryExpr@") != 0) << cast;
  }
}

// --- what a broken one says ----------------------------------------------------

TEST(CastTypeTest, AStrayCloserAndAStrayEqualAreNamed) {
  {
    // One `>` too many for the lists that were open.
    ParseFixture fixture("fn i32 main() { let x: i32 = 1; let y = x as A<i32>>; return 0; }\n");
    EXPECT_NE(fixture.errorMessages().find("parse-stray-type-arg-close"), std::string::npos)
        << fixture.errorMessages();
  }
  {
    // `>=` typed where the list needs one `>`: the `=` is the character that has
    // nothing to belong to, and a cast owns no declaration.
    ParseFixture fixture("type Pair<T, K> = (T, K);\n"
                         "fn i32 main() { let a: Pair<i32, bool> = (1, true);\n"
                         "  let b = a as Pair<i32, bool>= 1;\n  return 0; }\n");
    EXPECT_NE(fixture.errorMessages().find("parse-stray-type-arg-close"), std::string::npos)
        << fixture.errorMessages();
  }
}

TEST(CastTypeTest, ACastWithNoTypeIsOneSentence) {
  ParseFixture fixture("fn i32 main() { let x = 1 as; return 0; }\n");
  EXPECT_TRUE(fixture.hasErrors());
  EXPECT_NE(fixture.errorMessages().find("expected a type after `as`"), std::string::npos)
      << fixture.errorMessages();
}

TEST(CastTypeTest, AnEmptyListNamesItsOwnShape) {
  // `Vec<>` is read as the list it is, so the sentence is about the slot with
  // nothing in it -- and not about a comparison that would leave `>` with no right
  // operand, which is what refusing the empty list would produce.
  ParseFixture fixture("fn i32 main() { let x = 1 as Vec<>; return 0; }\n");
  EXPECT_NE(fixture.errorMessages().find("expected the type of an argument"), std::string::npos)
      << fixture.errorMessages();
}

TEST(CastTypeTest, AnUnterminatedListNamesTheCharacterThatIsMissing) {
  ParseFixture fixture("fn i32 main() { let x = 1 as Vec<i32; return 0; }\n");
  EXPECT_NE(fixture.errorMessages().find("parse-expected-type-arg-close"), std::string::npos)
      << fixture.errorMessages();
}

TEST(CastTypeTest, ALessWithNothingAfterItIsNotAList) {
  // An unterminated `<` in an expression: the reader asked whether the tokens after
  // it make a list, found none, and left the `<` where it is -- so the expression
  // reader reports the comparison's missing right operand instead of the type
  // reader reporting a list nobody wrote.
  ParseFixture fixture("fn i32 main() { let x: i32 = 1; let a = x as Foo <; return 0; }\n");
  EXPECT_TRUE(fixture.hasErrors());
  EXPECT_EQ(fixture.errorMessages().find("parse-expected-type-arg-close"), std::string::npos)
      << fixture.errorMessages();
}

} // namespace
} // namespace minc::parse
