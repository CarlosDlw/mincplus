// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Every code the checker can produce, from an input the grammar accepts, plus
// the two properties the diagnostics are built on: no cascading, and a bound
// that is a diagnostic rather than a hang.
#include <gtest/gtest.h>

#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "sema/sema_error.h"
#include "sema/sema_fixture.h"

namespace minc::test {
namespace {

TEST(ErrorsTest, UnknownTypeSuggestsAndKeepsOneDiagnostic) {
  SemaFixture f;
  f.source("fn i32 main() { let x: i33 = 1; return x; }\n");
  ASSERT_TRUE(f.build());

  EXPECT_TRUE(f.hasError("sema-unknown-type"));
  EXPECT_EQ(f.errorCount(), 1u);
  const sema::SemaError& error = f.firstError();
  EXPECT_NE(error.message.find("`i33` is not a type"), std::string::npos);
  // The suggestion is a *note* pointing at the word, not a second error for the
  // same mistake.
  EXPECT_NE(error.note.find("did you mean"), std::string::npos);
  EXPECT_TRUE(error.noteSpan.valid());
}

TEST(ErrorsTest, AMalformedTypeNamesTheCombination) {
  SemaFixture f;
  f.source("fn i32 main() { let x: unsigned float = 1; return 0; }\n");
  ASSERT_TRUE(f.build());

  EXPECT_TRUE(f.hasError("sema-malformed-type"));
  EXPECT_EQ(f.errorCount(), 1u);
  // Both words are understood; inventing a suggestion here would answer a
  // question nobody asked.
  EXPECT_TRUE(f.firstError().note.empty());
}

TEST(ErrorsTest, VoidIsNotAnObjectType) {
  {
    SemaFixture f;
    f.source("fn i32 main() { let x: void = 1; return 0; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-type-not-value"));
    EXPECT_EQ(f.errorCount(), 1u); // and not also "cannot be assigned to `void`"
  }
  {
    SemaFixture f;
    f.source("fn void nothing() { return; }\nfn i32 main() { let x = nothing(); return 0; }\n");
    ASSERT_TRUE(f.build());
    // Inferred from a void call: the same mistake, caught one step later.
    EXPECT_TRUE(f.hasError("sema-type-not-value"));
    EXPECT_EQ(f.errorCount(), 1u);
  }
}

TEST(ErrorsTest, ALiteralTooLargeForItsType) {
  SemaFixture f;
  f.source("fn i32 main() { let x: u8 = 256; return 0; }\n");
  ASSERT_TRUE(f.build());

  EXPECT_TRUE(f.hasError("sema-literal-out-of-range"));
  EXPECT_NE(f.firstError().message.find("does not fit in `u8`"), std::string::npos);

  SemaFixture ok;
  ok.source("fn i32 main() { let x: u8 = 255; return 0; }\n");
  ASSERT_TRUE(ok.build());
  EXPECT_EQ(ok.errorCount(), 0u);
}

TEST(ErrorsTest, ALiteralTooLargeForTheCoreNeedsAWiderContext) {
  SemaFixture wide;
  wide.source(
      "fn i32 main() { let x: u128 = 340282366920938463463374607431768211455; return 0; }\n");
  ASSERT_TRUE(wide.build());
  EXPECT_EQ(wide.errorCount(), 0u);

  SemaFixture narrow;
  narrow.source(
      "fn i32 main() { let x: i32 = 340282366920938463463374607431768211455; return 0; }\n");
  ASSERT_TRUE(narrow.build());
  EXPECT_TRUE(narrow.hasError("sema-literal-out-of-range"));
}

TEST(ErrorsTest, ConditionsMustBeBool) {
  SemaFixture f;
  f.source("fn i32 main() { let x: i32 = 1 ? 2 : 3; return x; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-condition-not-bool"));

  SemaFixture logic;
  logic.source("fn i32 main() { let b: bool = 1 && true; return 0; }\n");
  ASSERT_TRUE(logic.build());
  EXPECT_TRUE(logic.hasError("sema-condition-not-bool"));
}

TEST(ErrorsTest, BoolAndStrAreNotArithmetic) {
  {
    SemaFixture f;
    f.source("fn i32 main() { let b: bool = true; let x: i32 = b + 1; return x; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-invalid-operands"));
  }
  {
    // `%`, `&`, `|`, `^` are integer-only, and a float operand is refused with a
    // sentence that names the operator and both types.
    SemaFixture f;
    f.source("fn i32 main() { let x: f64 = 1.0 % 2.0; return 0; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-invalid-operands"));
    EXPECT_NE(f.firstError().message.find("integer operands"), std::string::npos);
  }
}

TEST(ErrorsTest, EqualityIsDefinedForArithmeticBoolAndStr) {
  {
    SemaFixture f;
    f.source("fn i32 main() { let a: str = \"x\"; let b: bool = a == a; return 0; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_FALSE(f.hasError("sema-invalid-operands"));
  }
  {
    SemaFixture f;
    f.source("fn i32 main() { let a: str = \"x\"; let b: bool = a < a; return 0; }\n");
    ASSERT_TRUE(f.build());
    // Ordering a `str` is refused: `a < b` on strings would compare addresses,
    // and that is the operator this language does not have.
    EXPECT_TRUE(f.hasError("sema-invalid-operands"));
  }
}

TEST(ErrorsTest, TheLeftSideOfAnAssignmentMustBeAPlace) {
  SemaFixture f;
  f.source("fn i32 main() { let x = 1; x + 1 = 2; return x; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-invalid-assignment"));
}

TEST(ErrorsTest, AConstCannotBeWritten) {
  {
    SemaFixture f;
    f.source("fn i32 main() { const c = 1; c = 2; return c; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-assign-to-const"));
    EXPECT_EQ(f.errorCount(), 1u); // the specific reason, not "not an lvalue"
  }
  {
    SemaFixture f;
    f.source("fn i32 main() { const c = 1; c += 1; return c; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-assign-to-const"));
  }
  {
    SemaFixture f;
    // Parentheses do not launder a `const`.
    f.source("fn i32 main() { const c = 1; (c) = 2; return c; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-assign-to-const"));
  }
  {
    SemaFixture f;
    f.source("fn i32 main() { const c = 1; ++c; return c; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-assign-to-const"));
  }
}

TEST(ErrorsTest, IncrementingIsForPlacesAndForNumbers) {
  {
    SemaFixture f;
    f.source("fn i32 main() { let x = 1; (x + 1)++; return x; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-incdec-not-lvalue"));
  }
  {
    SemaFixture f;
    f.source("fn i32 main() { let s: str = \"x\"; s++; return 0; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-invalid-operands"));
  }
}

TEST(ErrorsTest, Calls) {
  {
    SemaFixture f;
    f.source("fn i32 main() { let x = 1; return x(2); }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-not-a-function"));
  }
  {
    SemaFixture f;
    f.source("fn i32 zero() { return 0; }\nfn i32 main() { return zero(1, 2); }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-argument-count"));
    EXPECT_EQ(f.errorCount(), 1u);
    EXPECT_NE(f.firstError().message.find("takes no arguments"), std::string::npos);
  }
}

TEST(ErrorsTest, ReturnStatements) {
  {
    SemaFixture f;
    f.source("fn i32 f() { return \"x\"; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-return-mismatch"));
  }
  {
    SemaFixture f;
    f.source("fn i32 f() { return; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-return-missing-value"));
  }
  {
    SemaFixture f;
    f.source("fn void f() { return 1; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-return-void-value"));
  }
  {
    SemaFixture f;
    f.source("fn i32 f() { let x = 1; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-missing-return"));
  }
}

TEST(ErrorsTest, MainMustHaveTheReservedShape) {
  SemaFixture f;
  f.source("fn f64 main() { return 0.0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-main-signature"));
  EXPECT_NE(f.firstError().message.find("`main` must be declared"), std::string::npos);
}

TEST(ErrorsTest, ConstantDivisionByZeroIsDiagnosedHere) {
  {
    SemaFixture f;
    f.source("fn i32 main() { let x = 1 / 0; return x; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-division-by-zero"));
  }
  {
    SemaFixture f;
    f.source("fn i32 main() { let x = 1 % 0; return x; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_NE(f.firstError().message.find("remainder by zero"), std::string::npos);
  }
  {
    // Through a `const`, which is why the collected value exists.
    SemaFixture f;
    f.source("fn i32 main() { const z = 0; let x = 1 / z; return x; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-division-by-zero"));
  }
}

// The divisor is checked on its own, and not only when the whole expression
// folds: `x / 0` has a constant divisor and a non-constant other side, and the
// fold never runs for it. The mistake is the same mistake, so the diagnostic is
// the same diagnostic -- through one function the two spellings share.
TEST(ErrorsTest, AConstantZeroDivisorIsRefusedWithANonConstantDividend) {
  {
    SemaFixture f;
    f.source("fn i32 main() { let a = 1; let x = a / 0; return x; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-division-by-zero"));
  }
  {
    SemaFixture f;
    f.source("fn i32 main() { let a = 1; let x = a % 0; return x; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_NE(f.firstError().message.find("remainder by zero"), std::string::npos);
  }
  {
    // The compound form asks the same question.
    SemaFixture f;
    f.source("fn i32 main() { let a = 1; a /= 0; return a; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-division-by-zero"));
  }
  {
    SemaFixture f;
    f.source("fn i32 main() { let a = 1; a %= 0; return a; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-division-by-zero"));
  }
  {
    // And `*` is not a division. The divisor check is asked of every arithmetic
    // operator, so this is the case where it has to *decline* to answer: `x * 0`
    // is zero, not an error.
    SemaFixture f;
    f.source("fn i32 main() { let a = 1; let x = a * 0; return x; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_FALSE(f.hasError("sema-division-by-zero"));
  }
  {
    // A float division by zero is the IEEE answer and stays one: `hasIntValue`
    // is only set on an integer, so `0.0` never reaches the check.
    SemaFixture f;
    f.source("fn f64 main() { let a = 1.0; let x = a / 0.0; return x; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_FALSE(f.hasError("sema-division-by-zero"));
  }
}

// The advice in a refusal has to be advice a reader can act on. The message
// cannot name the expression -- the checker holds a type and a node, not the text
// the reader wrote -- so it names the shape of the fix, and it says *where* the
// conversion is before it gives the advice. A message with a hole in it ("write
// ` != 0`") is worse than no advice at all, and it is the kind of thing only a
// test can hold in place.
TEST(ErrorsTest, ARefusedBooleanConversionAdvisesAComparableShape) {
  SemaFixture f;
  f.source("fn i32 main() { let a = 1; let b: bool = a; return b ? 1 : 0; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_TRUE(f.hasError("sema-invalid-assignment"));
  const std::string& message = f.firstError().message;
  EXPECT_NE(message.find("does not convert to `bool`"), std::string::npos);
  EXPECT_NE(message.find("in this initializer"), std::string::npos);
  EXPECT_NE(message.find("value != 0"), std::string::npos);
  // No placeholder: the two characters that used to be all that was left of the
  // expression are not a message any more.
  EXPECT_EQ(message.find("write ` "), std::string::npos);
  EXPECT_EQ(message.find("write ` in"), std::string::npos);
}

// The shift count has a range and it is the width of the value moved, not the
// range of the value: C leaves both of these undefined and the backend inherits
// a poison value, which is a `>>` that does not shift.
TEST(ErrorsTest, AShiftCountOutsideTheWidthIsRefused) {
  {
    SemaFixture f;
    f.source("fn i32 main() { return 1 << 32; }\n");
    ASSERT_TRUE(f.build());
    // Asserted before `firstError()` is read: the sentence is only safe to look
    // at once there is one.
    ASSERT_TRUE(f.hasError("sema-shift-count-out-of-range"));
    EXPECT_EQ(f.errorCount(), 1u) << f.firstError().message;
    EXPECT_NE(f.firstError().message.find("(32)"), std::string::npos);
    EXPECT_NE(f.firstError().message.find("`i32`"), std::string::npos);
  }
  {
    SemaFixture f;
    f.source("fn i32 main() { return 1 >> 40; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-shift-count-out-of-range"));
  }
  {
    SemaFixture f;
    f.source("fn i32 main() { return 1 << (0 - 1); }\n");
    ASSERT_TRUE(f.build());
    ASSERT_TRUE(f.hasError("sema-shift-count-out-of-range"));
    EXPECT_NE(f.firstError().message.find("negative"), std::string::npos);
  }
  {
    // The width is the *left* operand's, so the same count is fine one type up.
    SemaFixture f;
    f.source("fn i64 f(a: i64) { return a << 40; }\nfn i32 main() { return 0; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_EQ(f.errorCount(), 0u);
  }
  {
    SemaFixture f;
    f.source("fn i32 main() { return 1 << 8; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_EQ(f.errorCount(), 0u);
  }
}

// A constant that does not fit the type it is computed in is a mistake the
// compiler can see. The runtime operation is defined to wrap, but a constant is
// not a runtime operation -- and a value the stage hands on but cannot represent
// is exactly the gap the IR would inherit.
TEST(ErrorsTest, AConstantThatDoesNotFitItsTypeIsRefused) {
  {
    // Through a `const`, so the operands already have a real type and nothing
    // downstream would re-check the result.
    SemaFixture f;
    f.source("fn i32 main() { const m = 2147483647; return m + 1; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-constant-out-of-range"));
    EXPECT_NE(f.firstError().message.find("2147483648"), std::string::npos);
  }
  {
    // `INT_MIN / -1`: the one quotient the type cannot hold.
    SemaFixture f;
    f.source("fn i32 main() { const m = (0 - 2147483647) - 1; return m / (0 - 1); }\n");
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasError("sema-constant-out-of-range"));
  }
  {
    // `INT_MIN % -1` is not that case: the remainder is representable, and it is
    // what the language defines.
    SemaFixture f;
    f.source("fn i32 main() { const m = (0 - 2147483647) - 1; return m % (0 - 1); }\n");
    ASSERT_TRUE(f.build());
    EXPECT_EQ(f.errorCount(), 0u);
  }
  {
    // A value that fits is not a finding, however it was computed.
    SemaFixture f;
    f.source("fn i32 main() { const m = 2147483647; return m - 1; }\n");
    ASSERT_TRUE(f.build());
    EXPECT_EQ(f.errorCount(), 0u);
  }
}

TEST(ErrorsTest, NoCascadeAroundAFailedExpression) {
  SemaFixture f;
  f.source("fn i32 main() { return unknown + unknown2 + 1; }\n");
  ASSERT_TRUE(f.build());
  // The names are resolution's finding; the checker must not add a second
  // sentence per enclosing operator.
  EXPECT_EQ(f.errorCount(), 0u);
}

TEST(ErrorsTest, NoCascadeAroundAFailedType) {
  SemaFixture f;
  f.source("fn i33 f() { return 0; }\nfn i32 main() { return f() + f(); }\n");
  ASSERT_TRUE(f.build());
  // One unknown type is one diagnostic, however many expressions are built on
  // the poison it produced: the poison spreads silently.
  EXPECT_EQ(f.errorCount(), 1u);
  EXPECT_TRUE(f.hasError("sema-unknown-type"));
}

TEST(ErrorsTest, NoCascadeAroundAParseError) {
  SemaFixture f;
  f.source("fn i32 main() { let x = ; return 0; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_TRUE(f.hasParseError());
  // The region the parser reported is not re-reported, and nothing downstream
  // of it invents a second problem.
  EXPECT_EQ(f.errorCount(), 0u);
}

TEST(ErrorsTest, TheTypeBudgetIsDiagnosedAndNotAHang) {
  SemaFixture f;
  f.source("fn i32 main() { let x: i32 = 1; return x; }\n");
  sema::SemaOptions options;
  options.maxTypes = 0; // lowered, never disabled
  f.semaOptions(options);
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-limit-types"));
}

TEST(ErrorsTest, EveryCodeIsReachableFromAnInputTheGrammarAccepts) {
  std::set<std::string> reached;
  const auto collect = [&reached](const SemaFixture& f) {
    for (const std::string& code : f.errorCodes()) {
      reached.insert(code);
    }
    for (const std::string& code : f.warningCodes()) {
      reached.insert(code);
    }
  };
  struct Case {
    std::string source;
    bool warnConversion = false;
  };
  const std::vector<Case> cases = {
      {"fn i33 f() { return 0; }\n", false},
      {"fn i32 main() { let x: unsigned float = 1; return 0; }\n", false},
      {"fn i32 main() { let x: void = 1; return 0; }\n", false},
      {"fn i32 main() { let x: u8 = 256; return 0; }\n", false},
      {"fn i32 main() { let x: i32 = 1 ? 2 : 3; return x; }\n", false},
      {"fn i32 main() { let x: str = \"a\" < \"b\"; return 0; }\n", false},
      {"fn i32 main() { let x = 1; x + 1 = 2; return x; }\n", false},
      {"fn i32 main() { const c = 1; c = 2; return c; }\n", false},
      {"fn i32 main() { let x = 1; (x + 1)++; return x; }\n", false},
      {"fn i32 main() { let x = 1; return x(2); }\n", false},
      {"fn i32 z() { return 0; }\nfn i32 main() { return z(1); }\n", false},
      {"fn i32 f() { return \"x\"; }\n", false},
      {"fn i32 f() { return; }\n", false},
      {"fn void f() { return 1; }\n", false},
      {"fn i32 f() { let x = 1; }\n", false},
      {"fn f64 main() { return 0.0; }\n", false},
      // The two ways a function's declarations can disagree. Both need more than
      // one declaration, which is why neither is a parse rule: what has to match
      // is a *type*, and this is the stage that has types.
      {"fn i32 f() { return 1; }\nfn i32 f() { return 2; }\n", false},
      {"extern fn i32 f(a: i32);\nfn i32 f() { return 0; }\n", false},
      {"fn i32 main() { return 1 / 0; }\n", false},
      {"fn i32 main() { return 0; }\n", false},                               // maxTypes = 0
      {"fn i32 f() { return 1; let x = 2; return x; }\n", false},             // unreachable
      {"fn i32 main() { let a: i32 = 1; let b: i8 = a; return 0; }\n", true}, // -Wconversion
      {"fn i32 f() { break; }\n", false},                                     // break with no loop
      {"fn i32 f() { continue; }\n", false},              // continue with no loop
      {"fn i32 f() { if 1 { } return 0; }\n", false},     // if condition not bool
      {"fn i32 f() { while 1 { } return 0; }\n", false},  // while condition not bool
      {"fn i32 f() { for ; 1; { } return 0; }\n", false}, // for condition not bool
      {"fn i32 f(void a) { return 0; }\n", false},        // void parameter
      {"fn i32 f(a: i32) { return a; }\nfn i32 main() { return f(); }\n", false},
      {"fn i32 f(a: i32) { return a; }\nfn i32 main() { return f(\"x\"); }\n", false},
      // A constant expression whose value does not fit the type it is computed
      // in, through a `const` so nothing downstream re-checks it.
      {"fn i32 main() { const m = 2147483647; return m + 1; }\n", false},
      {"fn i32 main() { return 1 << 32; }\n", false},
      {"fn i32 main() { let x: i32; return x; }\n", false},
      // The pointer codes. Each input is the shortest program that reaches one,
      // and each is here rather than in `pointer_test.cc` for the same reason as
      // every other row: this is the list that proves the *table* has no entry
      // nobody can reach.
      {"fn i32 main() { let x: i32 = 1; return *x; }\n", false},             // deref-not-pointer
      {"fn i32 main() { return *null; }\n", false},                          // void access
      {"fn i32 main() { let p: *void = null; p += 1; return 0; }\n", false}, // void step
      {"fn i32 main() { let x: i32 = 1; let p = &(x + 1); return 0; }\n", false},
      {"fn i32 main() { const c: i32 = 1; let p: *i32 = &c; return 0; }\n", false},
      {"fn i32 main() { const c: i32 = 1; let p: *i32 = &(c); return 0; }\n", false},
      {"fn i32 main() { let p: *i32 = null; let i: f64 = 1.0; return p[i]; }\n", false},
      {"fn i32 main() { let x: i32 = 1; let p: *i32 = &x; let b: bool = p == 1; return 0; }\n",
       false},
      {"fn i32 main() { let x: i32 = 1; let p: *i32 = x; return 0; }\n", false},
      // The two ways a `!` body can break its promise. They are here rather than
      // only in `never_test.cc` for the same reason as every row above: this is
      // the list that proves the *table* has no entry nobody can reach.
      {"fn ! f() { return; }\n", false},
      {"fn ! f() { }\n", false},
      // The file scope. A binding whose initializer cannot be a value, and the
      // one shape that has no order to evaluate in.
      {"let a: i32 = 1;\nlet b: i32 = a;\nfn i32 main() { return b; }\n", false},
      {"const a: i32 = b;\nconst b: i32 = a;\nfn i32 main() { return a; }\n", false},
      // An array subscript whose index is a constant the type can rule out. A
      // *runtime* index is not this code and not an error at all: it is the
      // access's extent, which the checked build guards (`arrays.md` decision 7).
      {"fn i32 f(a: [4]i32) { return a[7]; }\nfn i32 main() { return 0; }\n", false},
      // The two initializer codes, one input each. The list form has no type of
      // its own, and the typed form's shape can disagree with its own type -- in
      // three ways, of which the length is the one that also *is* the common
      // mistake: a list that is one element short of the type.
      {"fn i32 main() { let a = [1, 2, 3]; return 0; }\n", false},
      {"fn i32 main() { let b = [3]i32{1, 2}; return 0; }\n", false},
  };

  for (const Case& one : cases) {
    SemaFixture f;
    f.source(one.source);
    if (one.warnConversion) {
      f.warnConversion();
    }
    // The budget case is the one that needs a lowered limit.
    if (one.source == "fn i32 main() { return 0; }\n") {
      sema::SemaOptions options;
      options.maxTypes = 0;
      f.semaOptions(options);
    }
    ASSERT_TRUE(f.build()) << one.source;
    collect(f);
  }

  for (const sema::SemaErrorCode code : sema::allSemaErrorCodes()) {
    EXPECT_TRUE(reached.count(std::string(sema::toString(code))) != 0)
        << "no input produces " << sema::toString(code);
  }
  EXPECT_EQ(reached.size(), sema::allSemaErrorCodes().size());
}

TEST(ErrorsTest, TheCodeTableHasOneRowPerCode) {
  // The table is the contract: a code added to the enum without a row would be a
  // name nobody can grep for and a severity nobody decided.
  EXPECT_EQ(sema::semaErrorCodeInfos().size(), sema::allSemaErrorCodes().size());
  std::set<std::string> names;
  for (const sema::SemaErrorCodeInfo& info : sema::semaErrorCodeInfos()) {
    EXPECT_EQ(std::string(sema::toString(info.code)), std::string(info.name));
    EXPECT_TRUE(names.insert(info.name).second) << info.name << " appears twice";
    EXPECT_TRUE(std::string_view(info.name).rfind("sema-", 0) == 0) << info.name;
  }
  EXPECT_EQ(sema::isWarning(sema::SemaErrorCode::UnreachableCode), true);
  EXPECT_EQ(sema::isWarning(sema::SemaErrorCode::InvalidOperands), false);
}

} // namespace
} // namespace minc::test
