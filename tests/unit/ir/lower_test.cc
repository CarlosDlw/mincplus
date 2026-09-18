// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The lowering's shape: what the module contains for a construct, asserted on
// the text rather than on a golden file.
//
// Deliberately structural and not byte-exact. The text changes with LLVM, with
// the target and with a comment in a pass, so freezing it would freeze a moving
// thing; what is frozen is the *promise* -- no `nsw`, no `inbounds`, a guard
// before a division, one global per distinct string -- and that is what these
// read for.
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "ir/ir.h"
#include "ir/ir_fixture.h"
#include "sema/sema_error.h"
#include "sema/target.h"
#include "tests/examples_dir.h"

namespace minc::ir {
namespace {

TEST(IrLowerTest, AMainFunctionBecomesAModule) {
  test::IrFixture fixture;
  fixture.source("fn i32 main()\n{\n  return 0;\n}\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  const std::string text = fixture.module();
  EXPECT_NE(text.find("define i32 @main()"), std::string::npos);
  EXPECT_NE(text.find("ret i32 0"), std::string::npos);
  // The triple and the layout are LLVM's, and they are the target's identity
  // rather than the host's -- and the target here is the *default*, which is the
  // host, so the expected spelling is asked for rather than written down. A
  // literal `x86_64-unknown-linux-gnu` is what this test used to say, which made
  // it a statement about the machine running it.
  const std::string triple = "target triple = \"" + std::string(sema::kDefaultTriple) + "\"";
  EXPECT_NE(text.find(triple), std::string::npos) << text;
  EXPECT_NE(text.find("target datalayout = \""), std::string::npos);
}

TEST(IrLowerTest, TheTargetChoosesTheDataLayout) {
  const std::optional<sema::TargetInfo> windows = sema::targetFromName(sema::kTripleWindowsAmd64);
  ASSERT_TRUE(windows.has_value());
  test::IrFixture fixture("test.mx", *windows);
  fixture.source("fn i32 main() { return 0; }\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();
  EXPECT_NE(fixture.module().find("x86_64-pc-windows-msvc"), std::string::npos);
}

TEST(IrLowerTest, ABoolObjectIsAByteAndABoolValueIsABit) {
  test::IrFixture fixture;
  // A `bool` that is not a constant, because a constant folds: `true` stored
  // into an `i8` is the constant `1`, and the test is about the *instructions* a
  // non-constant one needs.
  fixture.source("fn bool flip(value: bool) { return !value; }\n"
                 "fn i32 choose(flag: bool) { return flag ? 1 : 0; }\n"
                 "fn i32 main()\n{\n  let flag: bool = flip(false);\n  return choose(flag);\n}\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  const std::string text = fixture.module();
  // The object is `i8` with a normalising store and a truncating load, because
  // `i1` is not a byte (`memory.md`, *Objects*).
  EXPECT_NE(text.find("alloca i8"), std::string::npos);
  EXPECT_NE(text.find("bool.store"), std::string::npos);
  EXPECT_NE(text.find("bool.load"), std::string::npos);
  EXPECT_EQ(text.find("alloca i1"), std::string::npos);
}

TEST(IrLowerTest, AnArrayValueIsOneConstantAndASplatIsNeverExpanded) {
  test::IrFixture fixture;
  fixture.source("fn i32 main()\n"
                 "{\n"
                 "  let zeros = [1024]u8{0; 1024};\n"
                 "  let sevens = [4]u8{7; 4};\n"
                 "  let list = [3]i32{1, 2, 3};\n"
                 "  let nested: [2][2]i32 = [[1, 2], [3, 4]];\n"
                 "  return zeros[0] + sevens[3] + list[1] + nested[1][1];\n"
                 "}\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  const std::string text = fixture.module();
  // A zero fill is `zeroinitializer` -- one constant of the array's *type*, and
  // the same one whatever the count is. This is the decision that a splat is a
  // shape: expanding 1024 elements here would make the front end's cost the
  // count's, and the module's text its size.
  EXPECT_NE(text.find("zeroinitializer"), std::string::npos);
  EXPECT_EQ(text.find("i8 0, i8 0, i8 0"), std::string::npos);
  // Every initializer is a *store of a constant*, not a chain of per-element
  // instructions.
  EXPECT_EQ(text.find("insertvalue"), std::string::npos);
  EXPECT_NE(text.find("[2 x [2 x i32]]"), std::string::npos);
  // The objects are the type's size and alignment, and the elements are read
  // through a plain `getelementptr`.
  EXPECT_NE(text.find("alloca [4 x i8]"), std::string::npos);
  EXPECT_NE(text.find("alloca [1024 x i8]"), std::string::npos);
}

TEST(IrLowerTest, ABoolArrayIsBytesAsAnObjectAndBitsAsAValue) {
  test::IrFixture fixture;
  // The scalar rule one level down: `[4]bool` is a *value* of `[4 x i1]` and an
  // object of `[4 x i8]`. A store compares the two types, so leaving this
  // difference to the caller would be a wrong-typed store -- which LLVM calls
  // undefined behaviour and reports as nothing at all.
  fixture.source("fn i32 main()\n"
                 "{\n"
                 "  let flags = [4]bool{true, false, true, false};\n"
                 "  if flags[0] { return 1; }\n"
                 "  return 0;\n"
                 "}\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  const std::string text = fixture.module();
  EXPECT_NE(text.find("alloca [4 x i8]"), std::string::npos);
  EXPECT_EQ(text.find("alloca [4 x i1]"), std::string::npos);
  EXPECT_NE(text.find("bool.load"), std::string::npos);
  // And the *initializer* is the storage form too. The object is `[4 x i8]`,
  // so the constant that reaches it has to be one -- an `[4 x i1]` here is a
  // store between two types, whose in-memory verifier is silent and whose
  // answer is whatever byte the assembler chose. This is the assertion that
  // fails if the element ever goes in as the value the expression produced.
  EXPECT_NE(text.find("store [4 x i8] c\"\\01\\00\\01\\00\""), std::string::npos) << text;
  EXPECT_EQ(text.find("store [4 x i1]"), std::string::npos) << text;
}

TEST(IrLowerTest, AByValueArrayIsACallerCopyAndAPointer) {
  test::IrFixture fixture;
  // `arrays.md` decision 13: the shape is the ABI's MEMORY class -- the caller
  // writes its own copy and passes its address -- so a `[1 << 20]i32` parameter
  // is a pointer in the signature and not a megabyte in every call site's type.
  fixture.source("fn i32 first(a: [4]i32) { return a[0]; }\n"
                 "fn i32 main() { let t = [4]i32{1, 2, 3, 4}; return first(t); }\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  const std::string text = fixture.module();
  EXPECT_NE(text.find("define i32 @first(ptr"), std::string::npos);
  EXPECT_NE(text.find("arg.copy"), std::string::npos);
  // No array is ever a parameter's LLVM type, which is the whole decision.
  EXPECT_EQ(text.find("define i32 @first([4 x i32]"), std::string::npos);
}

TEST(IrLowerTest, AnAggregateReturnIsTheCallersStorage) {
  test::IrFixture fixture;
  // The same decision, the other direction: the destination is the caller's
  // object, passed first and marked `sret`, and the function returns nothing. An
  // aggregate in the return type instead would put a megabyte of type into every
  // call site, debug record and function pointer for a large array -- the shape
  // five languages shipped wrong code from.
  fixture.source("fn [3]i32 make() { return [3]i32{1, 2, 3}; }\n"
                 "fn i32 main() { let a = make(); return a[2]; }\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  const std::string text = fixture.module();
  EXPECT_NE(text.find("define void @make(ptr sret([3 x i32])"), std::string::npos);
  EXPECT_EQ(text.find("define [3 x i32] @make"), std::string::npos);
  EXPECT_NE(text.find("call void @make"), std::string::npos);
  // The result is read back out of the caller's own slot, which is where the
  // callee wrote it.
  EXPECT_NE(text.find("load [3 x i32]"), std::string::npos);
}

TEST(IrLowerTest, ADivisionIsGuardedAndNeverPromisesNoOverflow) {
  test::IrFixture fixture;
  fixture.source("fn i32 divide(numerator: i32, denominator: i32)\n"
                 "{\n"
                 "  return numerator / denominator;\n"
                 "}\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  const std::string text = fixture.module();
  EXPECT_NE(text.find("sdiv"), std::string::npos);
  // The guard the language defines, which LLVM's `sdiv` does not.
  EXPECT_NE(text.find("div.zero"), std::string::npos);
  EXPECT_NE(text.find("llvm.trap"), std::string::npos);
  // And no promise of no-overflow anywhere: the language defines wrapping, so
  // `nsw`/`nuw` would be a promise about a program nobody wrote.
  EXPECT_EQ(text.find("nsw"), std::string::npos);
  EXPECT_EQ(text.find("nuw"), std::string::npos);
}

TEST(IrLowerTest, ANegationAndAStepAreTheOperandsOwnKind) {
  // A negation and a step are each **two instructions with one spelling**, and the
  // operand's type is what chooses: integer negation is `sub 0, x` and float
  // negation is `fneg`; an integer step adds an integer one and a float step adds a
  // float one. The checker accepts all four for every arithmetic type, so a branch
  // missing here is a module the verifier refuses -- which is what a negation and a
  // step written once for both kinds produced, and the sentence the reader got was
  // `ir-internal` about arithmetic on a floating type.
  //
  // The generic half is the same fact one stage up: `-x` under `Number` is a
  // negation at the *instance's* type, so the branch has to be read from the value
  // and not from the node.
  test::IrFixture fixture;
  fixture.source("fn f64 flip(x: f64) { return -x; }\n"
                 "fn f32 drift(x: f32) { x--; return x; }\n"
                 "fn i32 bump(x: i32) { return ++x; }\n"
                 "fn u8 negate(x: u8) { return -x; }\n"
                 "fn T mean<T: Number>(x: T) { return -x + ++x; }\n"
                 "fn i32 main() {\n"
                 "  let a: i32 = 1;\n"
                 "  let b: f64 = 1.0;\n"
                 "  let p = mean(a);\n"
                 "  let q = mean(b);\n"
                 "  return 0;\n"
                 "}\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();
  ASSERT_FALSE(fixture.hasError("ir-internal")) << fixture.module();

  const std::string text = fixture.module();
  EXPECT_NE(text.find("fneg double"), std::string::npos) << text;
  EXPECT_NE(text.find("fsub float"), std::string::npos) << text;
  EXPECT_NE(text.find("add i32"), std::string::npos) << text;
  // A `u8` negation is performed at the **promoted** type and truncated back, which
  // is the concrete path's own rule and not this test's subject -- so what is
  // asserted is the integer negation itself, at the width the operation happens at.
  EXPECT_NE(text.find("sub i32 0"), std::string::npos) << text;
  // The instance, so both halves of the branch are read at a *substituted* type and
  // not only at a written one: a `fneg` inside `mean<i32>` would be the node's type
  // leaking where the instance's belongs.
  EXPECT_NE(text.find("@__M4_meani32"), std::string::npos) << text;
  EXPECT_NE(text.find("@__M4_meanf64"), std::string::npos) << text;
  // And the two shapes that are *not* there: an integer zero built at a float type,
  // and a float one built at an integer type. They are one mistake in two places,
  // and each is a module the verifier refuses.
  EXPECT_EQ(text.find("sub double 0.000000e+00"), std::string::npos) << text;
  EXPECT_EQ(text.find("i0 "), std::string::npos) << text;
}

// An instance is a **private copy**, and the linkage is what lets two units
// instantiate the same template at the same type without the linker refusing the
// program: before this, both objects defined the same external `__M2_idi32` and
// the link ended in "multiple definition" -- a duplicate the source cannot rename,
// because the compiler chose the name.
//
// Private and not shared because there is nothing to share: resolution reads one
// unit at a time (a second file calling `id` without declaring it is
// `resolve-unknown-name`), so no unit can reach another's instance, and the two
// market answers -- COMDAT folding, a crate that owns the generic -- need a single
// definition point this language does not have until modules exist.
TEST(IrLowerTest, AnInstanceIsPrivateToItsUnit) {
  test::IrFixture fixture;
  fixture.source("fn T id<T>(x: T) { return x; }\n"
                 "fn i32 main() { return id(7); }\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  const std::string text = fixture.module();
  EXPECT_NE(text.find("define internal i32 @__M2_idi32("), std::string::npos) << text;
  // The template itself is never emitted -- a generic declaration has no body of
  // its own -- so the instance is the only definition here, and the declaration's
  // own linkage has nothing to narrow: `static fn T id<T>` and `fn T id<T>` give
  // the instance the same linkage.
  EXPECT_EQ(text.find("define i32 @__M2_idi32("), std::string::npos) << text;
}

TEST(IrLowerTest, AnIndexIsAPlainGetElementPtr) {
  test::IrFixture fixture;
  fixture.source("fn i32 read(p: *i32, i: i64)\n"
                 "{\n"
                 "  return p[i];\n"
                 "}\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  const std::string text = fixture.module();
  EXPECT_NE(text.find("getelementptr i32"), std::string::npos);
  // `inbounds` is a promise this language does not make (`memory.md`): the
  // access is inside the object only when the checked build proved it, and the
  // index is a full expression the compiler does not evaluate.
  EXPECT_EQ(text.find("getelementptr inbounds"), std::string::npos);
}

TEST(IrLowerTest, OneGlobalPerDistinctString) {
  test::IrFixture fixture;
  fixture.source("fn str first() { return \"hello\"; }\n"
                 "fn str second() { return \"hello\"; }\n"
                 "fn i32 main() { return 0; }\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  const std::string text = fixture.module();
  // Two literals with one spelling are *one object*: `&x` makes an address
  // observable, so two globals for one literal would make two equal pointers
  // compare unequal (`memory.md`, *Objects*). Counting the *declarations* and
  // not the uses, because a use is a reference to the one object.
  std::size_t globals = 0;
  for (std::size_t at = text.find("= private unnamed_addr global"); at != std::string::npos;
       at = text.find("= private unnamed_addr global", at + 1)) {
    ++globals;
  }
  EXPECT_EQ(globals, 1U) << text;
}

TEST(IrLowerTest, AFloat80OnAMachineWithoutX87IsRefusedBeforeTheMapperSeesIt) {
  // `f80` is the x87 format, so a machine with no x87 has no such *type*, and the
  // place that has to say so is the **checker** (`sema.md`, decision 26): a
  // program this stage refuses after the checker accepted it is a program the
  // pipeline promised would compile. It used to be exactly that -- this test's
  // old shape, with `check --target aarch64-unknown-linux-gnu` exiting 0 and the
  // lowering refusing afterwards.
  //
  // The lowering keeps its own guard, and this test is the seam around it: the
  // mapper's answer for an unmappable type must never become a null `alloca`, and
  // the unit must come out refused rather than built. Reaching that guard is a bug
  // now (`types.cc` says so), which is why the assertion here is the *checker's*
  // sentence and the absence of a module.
  const std::optional<sema::TargetInfo> aarch64 = sema::targetFromName(sema::kTripleLinuxAarch64);
  ASSERT_TRUE(aarch64.has_value());
  test::IrFixture fixture("test.mx", *aarch64);
  fixture.source("fn i32 main()\n{\n  let x: f80 = 0.0;\n  return 0;\n}\n");
  ASSERT_TRUE(fixture.build());

  // One refusal, one message, and it names the triple and the spelling to use.
  ASSERT_EQ(fixture.typed().errors.size(), 1U);
  EXPECT_EQ(sema::toString(fixture.typed().errors[0].code), "sema-malformed-type");
  EXPECT_NE(fixture.typed().errors[0].message.find(sema::kTripleLinuxAarch64), std::string::npos)
      << fixture.typed().errors[0].message;
  // And no module for a tree whose meaning nobody decided.
  EXPECT_TRUE(fixture.result().failed());
  EXPECT_FALSE(fixture.moduleBuilt());
}

TEST(IrLowerTest, APoisonedUnitIsRefusedAndProducesNoModule) {
  test::IrFixture fixture;
  // `i33` is not a type, so the checker reports it and the tree carries the
  // poison. The lowering must refuse rather than emit well-formed IR for a
  // program whose meaning nobody decided.
  fixture.source("fn i32 main() { let x: i33 = 1; return 0; }\n");
  ASSERT_TRUE(fixture.build());

  EXPECT_TRUE(fixture.result().failed());
  EXPECT_FALSE(fixture.moduleBuilt());
  EXPECT_TRUE(fixture.hasError("ir-internal")) << fixture.module();
}

TEST(IrLowerTest, ADeclarationAndItsDefinitionAreOneSymbol) {
  test::IrFixture fixture;
  fixture.source("extern fn i32 f();\n"
                 "fn i32 f() { return 7; }\n"
                 "fn i32 main() { return f(); }\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  const std::string text = fixture.module();
  // One symbol, and the body under the name the program calls. A second
  // `Function` for one def is what LLVM renames to `f.1`: the module then has a
  // declaration of `f` and a `define` nobody refers to, and the link fails on
  // "undefined reference to f".
  EXPECT_NE(text.find("define i32 @f()"), std::string::npos) << text;
  EXPECT_EQ(text.find("f.1"), std::string::npos) << text;
  EXPECT_NE(text.find("call i32 @f()"), std::string::npos) << text;
  EXPECT_EQ(fixture.violations(), 0U);
}

TEST(IrLowerTest, ACallToADeclarationBecomesADeclareAndACall) {
  test::IrFixture fixture;
  fixture.source("extern fn i32 puts(s: str);\n"
                 "fn i32 main() { return puts(\"x\"); }\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  const std::string text = fixture.module();
  EXPECT_NE(text.find("declare i32 @puts(ptr)"), std::string::npos) << text;
  EXPECT_EQ(text.find("define i32 @puts"), std::string::npos) << text;
}

TEST(IrLowerTest, ADeclarationNothingCallsIsStillADeclaration) {
  // A declaration is a promise about a symbol and not a definition, so it needs
  // neither a body nor a call. LLVM drops a declaration nothing references when
  // the object is written, so an `extern` declaration a program never calls
  // costs no symbol and no link-time question.
  test::IrFixture fixture;
  fixture.source("extern fn i32 unused(s: str);\nfn i32 main() { return 0; }\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  const std::string text = fixture.module();
  EXPECT_NE(text.find("declare i32 @unused(ptr)"), std::string::npos) << text;
  EXPECT_EQ(text.find("define i32 @unused"), std::string::npos) << text;
}

TEST(IrLowerTest, ANeverReturnTypeCutsTheEdgeAfterTheCall) {
  // The promise is told to LLVM in the one spelling it understands, and the fact
  // is *derived* from the return type rather than declared beside it: `!` maps to
  // `void` on the ABI side, so a function returning it is an ordinary `void`
  // function with an attribute on it. What the attribute buys is visible in the
  // same module -- the code the source wrote after the call has no way back, and
  // the terminator says so.
  test::IrFixture fixture;
  fixture.source("extern fn ! exit(code: i32);\n"
                 "fn i32 f() { exit(1); }\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  const std::string text = fixture.module();
  EXPECT_NE(text.find("attributes #0 = { noreturn }"), std::string::npos) << text;
  EXPECT_NE(text.find("declare void @exit(i32)"), std::string::npos) << text;
  EXPECT_NE(text.find("unreachable"), std::string::npos) << text;
  EXPECT_EQ(fixture.violations(), 0U);
}

TEST(IrLowerTest, ANeverArmOfAConditionalContributesPoison) {
  // The arm never produces a value, so there is nothing for the `phi` to take
  // from it -- and there is still an edge into the join from its block, because a
  // `call` is not a terminator even when it never returns. Poison of the result
  // type is the honest incoming value: it is never read, and it does not pretend
  // to be something the program could have computed.
  test::IrFixture fixture;
  fixture.source("extern fn ! exit(code: i32);\n"
                 "fn i32 pick(c: bool) { let v = c ? 1 : exit(2); return v; }\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  const std::string text = fixture.module();
  EXPECT_NE(text.find("phi i32"), std::string::npos) << text;
  EXPECT_NE(text.find("poison"), std::string::npos) << text;
  EXPECT_EQ(fixture.violations(), 0U);
}

TEST(IrLowerTest, OnlyANeverReturnTypeGetsTheNoreturnAttribute) {
  // Pins the direction: an implementation that set the attribute on every
  // function would still pass the test above, and one that set it on none would
  // pass this one. Both are needed.
  test::IrFixture fixture;
  fixture.source("fn i32 f() { return 1; }\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();
  EXPECT_EQ(fixture.module().find("noreturn"), std::string::npos) << fixture.module();
}

TEST(IrLowerTest, AVariadicDeclarationIsAVarArgsSignature) {
  // The type carries the marker all the way to LLVM's `FunctionType`, which is
  // what makes the call site below able to hand it more arguments than it
  // declares -- and, on x86-64, what makes the backend set the vector-register
  // count in `%al` from the argument types it is given.
  test::IrFixture fixture;
  fixture.source("extern fn i32 printf(fmt: str, ...);\n"
                 "fn i32 main() { return 0; }\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  const std::string text = fixture.module();
  EXPECT_NE(text.find("declare i32 @printf(ptr, ...)"), std::string::npos) << text;
  EXPECT_EQ(fixture.violations(), 0U);
}

TEST(IrLowerTest, AVariadicArgumentIsPromotedTheWayTheAbiPromises) {
  // The promotions are the part that is silently wrong when it is wrong:
  // `printf("%d", x)` with an `i8` prints the wrong integer, and nothing in the
  // pipeline notices. So the instructions are asserted, not the behaviour alone:
  // a `sext` for anything narrower than `int`, an `fpext` for `float`, and no
  // conversion at all for what is already the shape the ABI wants.
  test::IrFixture fixture;
  fixture.source("extern fn i32 printf(fmt: str, ...);\n"
                 "fn i32 main() {\n"
                 "  let small: i8 = 65;\n"
                 "  let wide: i64 = 9000000000;\n"
                 "  let ratio: f32 = 0.5;\n"
                 "  printf(\"%d %lld %f\\n\", small, wide, ratio);\n"
                 "  return 0;\n"
                 "}\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  const std::string text = fixture.module();
  EXPECT_NE(text.find("sext i8 %"), std::string::npos) << text;
  EXPECT_NE(text.find("to i32"), std::string::npos) << text;
  EXPECT_NE(text.find("fpext float %"), std::string::npos) << text;
  EXPECT_NE(text.find("to double"), std::string::npos) << text;
  // The call names the variadic signature, which is what tells LLVM the trailing
  // arguments are un-specified.
  EXPECT_NE(text.find("call i32 (ptr, ...) @printf("), std::string::npos) << text;
  // `i64` is passed as itself: `long long` is not promoted.
  EXPECT_NE(text.find(", i64 %"), std::string::npos) << text;
  EXPECT_EQ(fixture.violations(), 0U);
}

TEST(IrLowerTest, ADebugBuildOfAVariadicFunctionDoesNotBreakTheDebugTypes) {
  // A variadic *definition* is refused by the parser (`va_start` does not
  // exist), and this fixture lowers through errors on purpose. What is checked
  // here is that the DWARF path survives it: the subroutine type of a variadic
  // function ends with DWARF's null marker, and a debugger reading a fixed-arity
  // signature for it would show the wrong arguments for every frame inside.
  test::IrFixture fixture;
  fixture.debugInfo(true);
  fixture.source("fn i32 f(a: i32, ...) { return a; }\nfn i32 main() { return 0; }\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();
  EXPECT_EQ(fixture.violations(), 0U);
}

TEST(IrLowerTest, EveryExampleLowers) {
  namespace fs = std::filesystem;
  std::vector<std::string> files;
  std::error_code ec;
  for (const fs::directory_entry& entry : fs::directory_iterator(test::kExamplesDir, ec)) {
    if (entry.is_regular_file(ec) && entry.path().extension() == ".mx") {
      files.push_back(entry.path().string());
    }
  }
  ASSERT_FALSE(files.empty()) << "examples/ is missing files";

  // A *stated* target, not the host. The corpus is a statement about the
  // language, and part of what the language has depends on the machine it is for
  // (`f80` is x87's format, and `long double` resolves per target). Running the
  // sweep against whatever machine executes it would make "every example lowers" a
  // claim about the CI runner rather than about the corpus.
  const std::optional<sema::TargetInfo> reference = sema::targetFromName(sema::kTripleLinuxAmd64);
  ASSERT_TRUE(reference.has_value());

  for (const std::string& path : files) {
    test::IrFixture fixture(path, *reference);
    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in.good()) << path;
    const std::string source((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
    fixture.source(source);
    ASSERT_TRUE(fixture.build()) << path;
    EXPECT_TRUE(fixture.moduleBuilt()) << path << "\n" << fixture.module();
    EXPECT_TRUE(fixture.result().failed() == false) << path;
    EXPECT_EQ(fixture.violations(), 0U) << path;
  }
}

} // namespace
} // namespace minc::ir
