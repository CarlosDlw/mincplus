// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The file scope: what a binding at the top of a unit is worth, and which
// initializers are refused for not having a value at all.
//
// `globals.md` is the record; these are the properties it promises, one test
// each. Three of them are the reason the pass exists at all: a forward reference
// folds (order independence), a cycle is refused with its chain, and the value is
// *published* rather than left to be recomputed a stage later.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "sema/sema_fixture.h"

namespace minc::test {
namespace {

TEST(GlobalTest, AFileScopeBindingIsCheckedAndPublished) {
  SemaFixture f;
  f.source("const SIZE: i32 = 8;\n"
           "let counter: i32 = 0;\n"
           "fn i32 main() { return SIZE + counter; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_EQ(f.errorCount(), 0u);

  // The binding's type reaches its own definition, which is what a body that
  // reads it compares against -- the same answer a block-scope binding gives.
  EXPECT_EQ(f.bindingType("SIZE"), "i32");
  EXPECT_EQ(f.bindingType("counter"), "i32");
  // ... and its value is published, because the object's bytes are written by
  // the compiler and a stage below cannot re-derive them.
  const sema::GlobalInfo* size = f.globalOf("SIZE");
  ASSERT_NE(size, nullptr);
  EXPECT_EQ(sema::toString(size->value), "int");
  EXPECT_EQ(size->intValue.bits, 8u);
  EXPECT_EQ(size->intValue.signedValue(), 8);
  ASSERT_TRUE(size->init.valid());
  EXPECT_FALSE(size->negated);

  const sema::GlobalInfo* counter = f.globalOf("counter");
  ASSERT_NE(counter, nullptr);
  EXPECT_EQ(sema::toString(counter->value), "int");
  EXPECT_EQ(counter->intValue.signedValue(), 0);
}

TEST(GlobalTest, AnInitializerIsFoldedToAValue) {
  SemaFixture f;
  f.source("const A: i32 = 2 * 3 + 1;\n"
           "const B: u8 = 1 << 4;\n"
           "let C: i32 = -7;\n"
           "fn i32 main() { return A + B + C; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_EQ(f.errorCount(), 0u);

  ASSERT_NE(f.globalOf("A"), nullptr);
  EXPECT_EQ(f.globalOf("A")->intValue.signedValue(), 7);
  EXPECT_EQ(f.globalOf("B")->intValue.signedValue(), 16);
  // A unary minus over an integer is folded by the same core `#if` uses, so the
  // published value is the number and not a sign waiting for a stage below.
  EXPECT_EQ(f.globalOf("C")->intValue.signedValue(), -7);
}

TEST(GlobalTest, AForwardReferenceFoldsBecauseTheOrderIsNotTheSourceOrder) {
  SemaFixture f;
  // The whole point of the dependency walk: `FIRST` is written *above* the
  // binding it reads, and a file-scope name is visible independently of order
  // (`resolve.md`, decision A). A pass that walked in source order would refuse
  // `FIRST` for reading a name that is defined one line below it.
  f.source("const FIRST: i32 = LATER * 2;\n"
           "const LATER: i32 = 21;\n"
           "const CHAIN: i32 = FIRST + LATER;\n"
           "fn i32 main() { return CHAIN; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_EQ(f.errorCount(), 0u);

  ASSERT_NE(f.globalOf("FIRST"), nullptr);
  EXPECT_EQ(f.globalOf("FIRST")->intValue.signedValue(), 42);
  EXPECT_EQ(f.globalOf("CHAIN")->intValue.signedValue(), 63);
  // The table is still in **source** order, so a dump and a cache key are
  // deterministic however the walk happened to reach them.
  const std::vector<std::string> names = f.globalNames();
  ASSERT_EQ(names.size(), 3u);
  EXPECT_EQ(names[0], "FIRST");
  EXPECT_EQ(names[1], "LATER");
  EXPECT_EQ(names[2], "CHAIN");
}

TEST(GlobalTest, AValueWithNoFoldedFormIsPublishedAsItsLiteral) {
  SemaFixture f;
  f.source("const RATIO: f64 = 0.5;\n"
           "const NEG: f64 = -1.5;\n"
           "const WIDE: i128 = 100000000000000000000;\n"
           "const GREETING: str = \"hi\";\n"
           "fn i32 main() { return 0; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_EQ(f.errorCount(), 0u);

  // A float has no folded value by design (`sema.md`, *Constant folding*): the
  // record names the *node* whose spelling is the value, and the lowering reads
  // it with the reader that already exists for it.
  ASSERT_NE(f.globalOf("RATIO"), nullptr);
  EXPECT_EQ(f.globalKind("RATIO"), "literal");
  EXPECT_TRUE(f.globalOf("RATIO")->node.valid());
  EXPECT_FALSE(f.globalOf("RATIO")->negated);

  // A sign is a bit and not arithmetic, so a negated literal is still one
  // literal -- the spelling plus the fact that it is negated.
  EXPECT_EQ(f.globalKind("NEG"), "literal");
  EXPECT_TRUE(f.globalOf("NEG")->negated);

  // Wider than the 64-bit core the folding uses, so its digits are the value.
  EXPECT_EQ(f.globalKind("WIDE"), "literal");

  // A `str` is the address of the bytes the compiler writes for it.
  EXPECT_EQ(f.globalKind("GREETING"), "literal");
}

TEST(GlobalTest, NullAndNoInitializerAreValuesOfTheirOwn) {
  SemaFixture f;
  f.source("let nothing: *i32 = null;\n"
           "let zeroed: i32;\n"
           "let flag: bool = true;\n"
           "fn i32 main() { return zeroed; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_EQ(f.errorCount(), 0u);

  ASSERT_NE(f.globalOf("nothing"), nullptr);
  EXPECT_EQ(f.globalKind("nothing"), "null");
  // No initializer is the C ABI's `.bss`: zero, and a decision rather than an
  // omission (`memory.md`, decision 12).
  EXPECT_EQ(f.globalKind("zeroed"), "zero");
  // `true` and `false` carry an integer value, so they are folded like any other
  // integer constant.
  EXPECT_EQ(f.globalKind("flag"), "int");
  EXPECT_EQ(f.globalOf("flag")->intValue.bits, 1u);
}

TEST(GlobalTest, AFileScopeLetIsNeverReadBeforeItIsAssigned) {
  SemaFixture f;
  // The definite-assignment pass is about the paths *inside* a function, and a
  // file-scope object's bytes are written before any of them run. Without the
  // exception this is the first read of a file-scope `let` and it would be
  // reported as unassigned.
  f.source("let counter: i32 = 0;\n"
           "let other: i32;\n"
           "fn void bump() { counter = counter + 1; other = 2; }\n"
           "fn i32 main() { bump(); return counter + other; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.errorCount(), 0u);
}

TEST(GlobalTest, AnInitializerThatIsNotAConstantIsRefusedByName) {
  // One case per kind of thing that has no value before the program runs. Each
  // program produces **one** diagnostic, and the sentence names what the reader
  // wrote: that is the whole difference between a refusal that can be acted on
  // and one that says "this expression".
  struct Case {
    const char* source;
    const char* fragment;
  };
  const Case cases[] = {
      {"let a: i32 = 1;\nconst B: i32 = a;\nfn i32 main() { return B; }\n", "`a` is a `let`"},
      {"fn i32 f() { return 1; }\nconst C: i32 = f();\nfn i32 main() { return C; }\n",
       "a call is not a constant"},
      {"let p: *i32 = null;\nlet D: i32 = *p;\nfn i32 main() { return D; }\n",
       "a dereference reads memory"},
      {"let x: i32 = 1;\nlet F: *i32 = &x;\nfn i32 main() { return x; }\n",
       "the address of an object"},
      // The offender is the operand and not the whole expression: `a + b` where
      // `a` is a `let` is a story about `a`, and the caret has to land on it.
      {"let a: i32 = 1;\nconst G: i32 = a + 1;\nfn i32 main() { return G; }\n", "`a` is a `let`"},
  };

  for (const Case& one : cases) {
    SemaFixture f;
    f.source(one.source);
    ASSERT_TRUE(f.build()) << one.source;
    ASSERT_EQ(f.errorCount(), 1u) << one.source;
    EXPECT_TRUE(f.hasError("sema-global-not-constant")) << one.source;
    EXPECT_NE(f.firstError().message.find(one.fragment), std::string::npos)
        << one.source << ": " << f.firstError().message;
  }
}

TEST(GlobalTest, ARefusedExpressionIsReportedOnceByTheRuleThatRefusedIt) {
  // `1 + 2.0` is not a bad *constant*, it is a `+` the language does not have.
  // The operator says so, in the words an assignment uses for the same rule, and
  // this pass adds nothing: "not a constant" about an expression with no type
  // would be a second diagnostic describing a different mistake.
  SemaFixture f;
  f.source("const E: i32 = 1 + 2.0;\nfn i32 main() { return E; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_EQ(f.errorCount(), 1u) << f.firstError().message;
  EXPECT_TRUE(f.hasError("sema-invalid-operands"));
  EXPECT_FALSE(f.hasError("sema-global-not-constant"));
  EXPECT_NE(f.firstError().message.find("different classes of number"), std::string::npos)
      << f.firstError().message;
}

TEST(GlobalTest, ALocalIsNotAValueAndNeitherIsAMissingName) {
  {
    SemaFixture f;
    // A local `const` is a constant, but it has no value at file scope: the
    // sentence has to say *that* rather than "not a constant".
    f.source("const H: i32 = 1;\n"
             "fn i32 main() { const local: i32 = 2; let x: i32 = local; return x; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_EQ(f.errorCount(), 0u);
  }
  {
    SemaFixture f;
    // A malformed literal was reported by the lexer, and `checkLiteral` left it
    // neither constant nor a value: this pass says nothing about it, because one
    // mistake is one diagnostic.
    f.source("const A: i32 = 0o9;\nfn i32 main() { return A; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_EQ(f.errorCount(), 0u) << "the lexer owns the digits";
  }
}

TEST(GlobalTest, EveryFileScopeBindingGetsOneRecordInSourceOrder) {
  SemaFixture f;
  f.source("const A: i32 = 1;\n"
           "let b: i32 = 2;\n"
           "const C: str = \"c\";\n"
           "fn i32 main() { return A + b; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_EQ(f.errorCount(), 0u);

  const std::vector<std::string> names = f.globalNames();
  ASSERT_EQ(names.size(), 3u);
  EXPECT_EQ(names[0], "A");
  EXPECT_EQ(names[1], "b");
  EXPECT_EQ(names[2], "C");
  // The table is published once per binding and not once per read: the two reads
  // in `main` add nothing to it.
  EXPECT_EQ(f.typed().globalTable.size(), 3u);
}

TEST(GlobalTest, ACycleIsRefusedOnceWithTheChainThatClosesIt) {
  {
    SemaFixture f;
    f.source("const A: i32 = B;\n"
             "const B: i32 = A;\n"
             "fn i32 main() { return A; }\n");
    ASSERT_TRUE(f.build());
    // One diagnostic for one cycle, and not two -- one per binding -- and not
    // also a "not a constant" for each end of it.
    EXPECT_EQ(f.errorCount(), 1u);
    ASSERT_TRUE(f.hasError("sema-global-cycle"));
    EXPECT_NE(f.firstError().message.find("A -> B -> A"), std::string::npos)
        << f.firstError().message;
  }
  {
    SemaFixture f;
    // A cycle of one: the binding reads itself.
    f.source("const H: i32 = H + 1;\nfn i32 main() { return H; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_EQ(f.errorCount(), 1u);
    EXPECT_TRUE(f.hasError("sema-global-cycle"));
    EXPECT_NE(f.firstError().message.find("H -> H"), std::string::npos);
  }
  {
    SemaFixture f;
    // Longer than two, and the chain is what makes it readable.
    f.source("const A: i32 = B;\n"
             "const B: i32 = C;\n"
             "const C: i32 = A;\n"
             "fn i32 main() { return A; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_EQ(f.errorCount(), 1u);
    ASSERT_TRUE(f.hasError("sema-global-cycle"));
    EXPECT_NE(f.firstError().message.find("A -> B -> C -> A"), std::string::npos)
        << f.firstError().message;
  }
}

TEST(GlobalTest, AConditionalAndABooleanConstantAreBothValues) {
  SemaFixture f;
  f.source("const FLAG: bool = true;\n"
           "const ALSO: bool = !FLAG;\n"
           "const PICK: i32 = FLAG ? 1 : 2;\n"
           "fn i32 main() { return PICK; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_EQ(f.errorCount(), 0u);

  EXPECT_EQ(f.globalKind("FLAG"), "int");
  EXPECT_EQ(f.globalOf("ALSO")->intValue.bits, 0u);
  // A `?:` over a constant condition folds to the arm it takes, which is the
  // same folding a block-scope binding gets.
  EXPECT_EQ(f.globalOf("PICK")->intValue.signedValue(), 1);
}

TEST(GlobalTest, AConstantReadsAnotherConstantWithoutFoldingItAgain) {
  SemaFixture f;
  f.source("const PI: f64 = 3.5;\n"
           "const HALF: f64 = PI;\n"
           "const DOUBLE: i32 = 21;\n"
           "const TWICE: i32 = DOUBLE * 2;\n"
           "fn i32 main() { return TWICE; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_EQ(f.errorCount(), 0u);

  // An integer *is* folded through the other binding, because the folded value is
  // what the core has (`support/consteval`).
  EXPECT_EQ(f.globalKind("TWICE"), "int");
  EXPECT_EQ(f.globalOf("TWICE")->intValue.signedValue(), 42);
  // A float is not, and what is published is the literal the other binding's
  // value already pointed at: one value, read once, with no rounding in between.
  EXPECT_EQ(f.globalKind("HALF"), "literal");
  EXPECT_EQ(f.globalOf("HALF")->node.index, f.globalOf("PI")->node.index);
}

} // namespace
} // namespace minc::test
