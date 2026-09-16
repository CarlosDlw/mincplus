// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// A builtin in the module: what it becomes, and the one test that keeps the
// table from going stale.
//
// ### The differential test, and why it is worth an LLVM dependency here
//
// Every row whose lowering is an intrinsic names it, and `ir` gets the
// declaration from `Intrinsic::getOrInsertDeclaration`. That call derives the
// attributes from `Intrinsics.td`, so the two *agree by construction* today --
// and that is exactly why the test is worth writing: what it catches is the day
// somebody adds an attribute by hand beside the declaration, or maps a new row to
// an intrinsic whose contract this language does not keep. A front end's copied
// table goes stale silently; this one fails here, with the row's name in the
// message, when LLVM's own answer changes.
//
// The `Effect` cross-check is the other half, and the more valuable one: it asks
// whether what the *row claims* about the world matches what LLVM knows. A row
// that claimed `Effect::None` for an intrinsic that writes memory would be a
// checker deciding reachability and constness on a false premise.
//
// This file includes `llvm/*`, which only `src/ir` and `src/backend` may do --
// and the isolation test walks `src/` and `include/`, not the tests, because a
// test whose subject *is* LLVM's view of the module has to be able to ask it.
#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "llvm/IR/Attributes.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"

#include "builtins/builtin.h"
#include "ir/ir.h"
#include "ir/ir_fixture.h"
#include "ir/storage.h"

namespace minc::ir {
namespace {

// --- a program per row --------------------------------------------------------
//
// Generated from the row and not written per row, so a row added to the table is
// covered by every test below without anyone remembering to add a snippet. That
// is the same property `all()` gives the drivers: the data is the list.

[[nodiscard]] std::string sourceSpelling(builtins::BuiltinType type) {
  switch (type) {
  case builtins::BuiltinType::Void:
    return "void";
  case builtins::BuiltinType::Bool:
    return "bool";
  case builtins::BuiltinType::Char:
    return "char";
  case builtins::BuiltinType::Str:
    return "str";
  case builtins::BuiltinType::I8:
    return "i8";
  case builtins::BuiltinType::I16:
    return "i16";
  case builtins::BuiltinType::I32:
    return "i32";
  case builtins::BuiltinType::I64:
    return "i64";
  case builtins::BuiltinType::I128:
    return "i128";
  case builtins::BuiltinType::Isize:
    return "isize";
  case builtins::BuiltinType::U8:
    return "u8";
  case builtins::BuiltinType::U16:
    return "u16";
  case builtins::BuiltinType::U32:
    return "u32";
  case builtins::BuiltinType::U64:
    return "u64";
  case builtins::BuiltinType::U128:
    return "u128";
  case builtins::BuiltinType::Usize:
    return "usize";
  case builtins::BuiltinType::F32:
    return "f32";
  case builtins::BuiltinType::F64:
    return "f64";
  case builtins::BuiltinType::VoidPtr:
    return "*void";
  case builtins::BuiltinType::Never:
    return "!";
  case builtins::BuiltinType::AnyInteger:
  case builtins::BuiltinType::MatchArg:
  case builtins::BuiltinType::MatchArgPtr:
    // The shapes the sweep fixes for the row it is generating: `any-int` and the
    // holes are the *i32* of the argument the test declares, which is what makes
    // one program per row possible at all.
    return "i32";
  }
  return "i32";
}

// The argument list of a row, as declarations and as the call's operands.
[[nodiscard]] std::string boundArgument(std::size_t index, builtins::BuiltinType type) {
  return "  let a" + std::to_string(index) + ": " + sourceSpelling(type) + " = 1;\n";
}

[[nodiscard]] std::string callOperands(const builtins::BuiltinInfo& row) {
  std::string operands;
  for (std::size_t i = 0; i < row.signature.params.size(); ++i) {
    if (i != 0) {
      operands += ", ";
    }
    operands += "a" + std::to_string(i);
  }
  return operands;
}

// One program that *calls* the row, in the shape its result type allows: a
// `Never` result cannot be bound (`let x: ! = ...` is an object of the bottom
// type, which `never.md` refuses on purpose), so such a row is called as the last
// statement of a function whose type says it does not return.
[[nodiscard]] std::string callProgram(const builtins::BuiltinInfo& row) {
  std::string declarations;
  for (std::size_t i = 0; i < row.signature.params.size(); ++i) {
    declarations += boundArgument(i, row.signature.params[i]);
  }
  const std::string call = std::string(row.spelling) + "(" + callOperands(row) + ")";
  if (row.signature.result == builtins::BuiltinType::Never) {
    return "fn i32 diverge() {\n" + declarations + "  " + call + ";\n}\n" +
           "fn i32 main() { return 0; }\n";
  }
  return "fn i32 main() {\n" + declarations + "  let r = " + call + ";\n  return 0;\n}\n";
}

// --- the module's own view ----------------------------------------------------

// The attribute text of a declaration *and* of each of its parameters, which is
// what "the attributes are LLVM's and not ours" means in a comparison: the two
// lists have to be equal as text, so the test fails on an attribute added,
// removed or changed anywhere.
[[nodiscard]] std::vector<std::string> attributeFingerprint(const llvm::Function& function) {
  std::vector<std::string> out;
  out.push_back(function.getAttributes().getFnAttrs().getAsString());
  for (unsigned i = 0; i < function.arg_size(); ++i) {
    out.push_back(function.getAttributes().getParamAttrs(i).getAsString());
  }
  return out;
}

// The name the declaration actually has, which is the intrinsic's *mangled* one
// (`llvm.ctlz.i32`). Asked for rather than written out, and looked up with
// `getFunction` rather than `getDeclaration`: the latter creates the declaration
// it cannot find, which would make this test pass on a module that never emitted
// one -- a test that can be satisfied by the bug it exists to catch is not a
// test.
[[nodiscard]] llvm::Function* findDeclaration(llvm::Module& module,
                                              const builtins::BuiltinInfo& row) {
  const llvm::Intrinsic::ID id = llvm::Intrinsic::lookupIntrinsicID(row.lowering.name);
  llvm::SmallVector<llvm::Type*, 1> overloads;
  if (row.lowering.overload == builtins::Lowering::Overload::FirstArgument) {
    overloads.push_back(llvm::Type::getInt32Ty(module.getContext()));
  }
  return module.getFunction(llvm::Intrinsic::getName(id, overloads, &module));
}

// What LLVM says the attributes of this declaration should be. Asked with the
// declaration's own type, so the answer is for the exact overload the module
// holds rather than for the row's abstract shape.
[[nodiscard]] std::vector<std::string> expectedAttributes(llvm::Function& declaration) {
  const llvm::AttributeList list = llvm::Intrinsic::getAttributes(
      declaration.getContext(), llvm::Intrinsic::lookupIntrinsicID(declaration.getName()),
      declaration.getFunctionType());
  std::vector<std::string> out;
  out.push_back(list.getFnAttrs().getAsString());
  for (unsigned i = 0; i < declaration.arg_size(); ++i) {
    out.push_back(list.getParamAttrs(i).getAsString());
  }
  return out;
}

// --- tests --------------------------------------------------------------------

TEST(IrBuiltinsTest, EveryRowIsLowerable) {
  // The sweep the other stages use, for builtins: one input per row, generated
  // from the row, built into a module. It is also what gives the differential
  // test below its subjects, so a row that cannot be lowered cannot be silently
  // skipped by it either.
  for (const builtins::BuiltinInfo& row : builtins::all()) {
    test::IrFixture fixture;
    fixture.source(callProgram(row));
    ASSERT_TRUE(fixture.build()) << row.spelling;
    EXPECT_TRUE(fixture.moduleBuilt()) << row.spelling << ": " << fixture.module();
    EXPECT_TRUE(fixture.codes().empty()) << row.spelling;
  }
}

TEST(IrBuiltinsTest, TheDeclarationCarriesLLVMsOwnAttributesAndNothingElse) {
  for (const builtins::BuiltinInfo& row : builtins::all()) {
    if (row.lowering.kind != builtins::Lowering::Kind::Intrinsic) {
      continue;
    }
    const llvm::Intrinsic::ID id = llvm::Intrinsic::lookupIntrinsicID(row.lowering.name);
    ASSERT_NE(id, llvm::Intrinsic::not_intrinsic) << row.spelling;

    test::IrFixture fixture;
    fixture.source(callProgram(row));
    ASSERT_TRUE(fixture.build()) << row.spelling;
    ASSERT_TRUE(fixture.moduleBuilt()) << row.spelling << ": " << fixture.module();

    llvm::Module& module = ModuleAccess::llvmModule(fixture.result().module);
    llvm::Function* declaration = findDeclaration(module, row);
    ASSERT_NE(declaration, nullptr) << row.spelling << ": " << fixture.module();

    // One comparison, over the function and every parameter: an attribute added
    // by hand in the lowering would show up here, and so would one LLVM stops
    // deriving.
    EXPECT_EQ(attributeFingerprint(*declaration), expectedAttributes(*declaration)) << row.spelling;
  }
}

TEST(IrBuiltinsTest, WhatTheRowClaimsMatchesWhatLLVMKnows) {
  // The differential property, and the one that goes stale in a front end that
  // copies a table: a row's `effect` has to agree with the intrinsic's own
  // properties. `memory(none)` is "reads and writes nothing"; `NoReturn` is
  // "control does not come back". A row that claimed either without the intrinsic
  // saying so would be the checker reasoning from a false premise.
  for (const builtins::BuiltinInfo& row : builtins::all()) {
    if (row.lowering.kind != builtins::Lowering::Kind::Intrinsic) {
      continue;
    }
    const llvm::Intrinsic::ID id = llvm::Intrinsic::lookupIntrinsicID(row.lowering.name);
    ASSERT_NE(id, llvm::Intrinsic::not_intrinsic) << row.spelling;

    test::IrFixture fixture;
    fixture.source(callProgram(row));
    ASSERT_TRUE(fixture.build()) << row.spelling;
    ASSERT_TRUE(fixture.moduleBuilt()) << row.spelling;
    llvm::Module& module = ModuleAccess::llvmModule(fixture.result().module);
    llvm::Function* declaration = findDeclaration(module, row);
    ASSERT_NE(declaration, nullptr) << row.spelling;
    const llvm::AttributeSet attributes =
        llvm::Intrinsic::getAttributes(module.getContext(), id, declaration->getFunctionType())
            .getFnAttrs();

    const bool diverges = attributes.hasAttribute(llvm::Attribute::NoReturn);
    EXPECT_EQ(diverges, row.effect == builtins::Effect::Diverges)
        << row.spelling << " claims " << builtins::toString(row.effect);

    const bool noMemory = attributes.hasAttribute(llvm::Attribute::Memory) &&
                          attributes.getMemoryEffects().doesNotAccessMemory();
    if (row.effect == builtins::Effect::None) {
      EXPECT_TRUE(noMemory) << row.spelling << " claims to touch nothing";
    }
  }
}

TEST(IrBuiltinsTest, TheZeroAnswerIsTheLanguagesAndNotLLVMsPoison) {
  // `clz(0)` is the width in this language, and the module says so with the one
  // operand LLVM needs: `is_zero_poison` is `false`. Reading it out of the text is
  // how this test says "the row asked for the defined answer" -- the constant
  // operand is the whole of the promise, and a row that lost it would be UB the
  // checker could not see.
  test::IrFixture fixture;
  fixture.source("fn i32 main() { let x: u32 = 0; let r = clz(x); let s = ctz(x); return 0; }\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  const std::string text = fixture.module();
  EXPECT_NE(text.find("llvm.ctlz.i32(i32 "), std::string::npos) << text;
  EXPECT_NE(text.find(", i1 false)"), std::string::npos) << text;
  EXPECT_NE(text.find("llvm.cttz.i32(i32 "), std::string::npos) << text;
  // And never the poisoned form: one `true` in this module would be a zero
  // argument the language allows and LLVM may answer anything for.
  EXPECT_EQ(text.find(", i1 true)"), std::string::npos) << text;
}

TEST(IrBuiltinsTest, ARotateReducesItsCountBeforeTheIntrinsic) {
  // `llvm.fshl` is poison for a count at or past the width; this language defines
  // a rotate as the count modulo the width. So the `urem` is the language's
  // answer and not an optimization, and the test reads it in the module.
  test::IrFixture fixture;
  fixture.source("fn i32 main() { let x: i32 = 1; let n: i32 = 40; let r = rotl(x, n);\n"
                 "  let s = rotr(x, n); return 0; }\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  const std::string text = fixture.module();
  EXPECT_NE(text.find("urem i32"), std::string::npos) << text;
  EXPECT_NE(text.find("llvm.fshl.i32"), std::string::npos) << text;
  EXPECT_NE(text.find("llvm.fshr.i32"), std::string::npos) << text;
}

TEST(IrBuiltinsTest, ACountWiderThanTheValueStillReduces) {
  // The count is its own integer type, so a `u64` count on an `i32` value is
  // legal -- and the wrapping has to happen *before* the modulo for the answer to
  // stay `n mod width`. Both widths are powers of two, which is what makes the
  // two orders agree; this test is what would catch a future change to a width
  // that is not.
  test::IrFixture fixture;
  fixture.source(
      "fn i32 main() { let x: i32 = 1; let n: u64 = 3; let r = rotl(x, n); return 0; }\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  const std::string text = fixture.module();
  EXPECT_NE(text.find("trunc i64"), std::string::npos) << text;
  EXPECT_NE(text.find("urem i32"), std::string::npos) << text;
}

TEST(IrBuiltinsTest, EveryIntegerWidthGetsItsOwnDeclaration) {
  // One row, twelve concrete declarations -- and the widths that matter for a
  // module are the *LLVM* ones, so this is where `isize` being the target's width
  // stops being a claim about the table and becomes `llvm.ctlz.i64`.
  const std::vector<std::pair<std::string, std::string>> widths = {
      {"i8", "i8"},     {"i16", "i16"},   {"i32", "i32"},   {"i64", "i64"},
      {"i128", "i128"}, {"isize", "i64"}, {"u8", "i8"},     {"u16", "i16"},
      {"u32", "i32"},   {"u64", "i64"},   {"u128", "i128"}, {"usize", "i64"}};
  for (const auto& [written, llvm] : widths) {
    test::IrFixture fixture;
    fixture.source("fn i32 main() { let x: " + written + " = 1; let r = clz(x); return 0; }\n");
    ASSERT_TRUE(fixture.build()) << written;
    ASSERT_TRUE(fixture.moduleBuilt()) << written << ": " << fixture.module();
    EXPECT_NE(fixture.module().find("@" + std::string("llvm.ctlz.") + llvm + "(i"),
              std::string::npos)
        << written << ": " << fixture.module();
  }
}

TEST(IrBuiltinsTest, ACallThatNeverComesBackEndsTheBlock) {
  // `llvm.trap` plus the `unreachable` after it: the module has to say control
  // stops, because everything downstream of the call -- the optimizer, the
  // backend, the reader of the IR -- reasons from that.
  test::IrFixture fixture;
  fixture.source("fn i32 fail() { __builtin_trap(); }\nfn i32 main() { return fail(); }\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  const std::string text = fixture.module();
  EXPECT_NE(text.find("call void @llvm.trap()"), std::string::npos) << text;
  EXPECT_NE(text.find("unreachable"), std::string::npos) << text;
}

TEST(IrBuiltinsTest, NothingForbiddenEntersTheModule) {
  // The invariant scan, over a corpus that mentions every row: the permit-list
  // `ir.md` states (no `noalias`, `nonnull`, `noundef`, `dereferenceable`,
  // `align`, `signext`, `zeroext`, `inreg`) applies to a builtin's declaration
  // exactly as it applies to a function this compiler wrote.
  std::string source;
  for (const builtins::BuiltinInfo& row : builtins::all()) {
    source += callProgram(row);
  }
  test::IrFixture fixture;
  fixture.source(source);
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  for (const IRDiagnostic& violation : fixture.scan()) {
    ADD_FAILURE() << violation.message;
  }
}

} // namespace
} // namespace minc::ir
