// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// File-scope objects in the module: the shape, the linkage, and the bytes.
//
// The property these tests exist for is the one `globals.md` decision 2 states
// and `ir.md` repeats: a file-scope object's initializer is a **value**, written
// by the compiler, and never an instruction. Every module below is scanned for
// that first, because a `llvm::GlobalVariable` whose initializer came from
// anywhere but the record is exactly the failure the record exists to prevent.
#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Alignment.h"

#include "ir/ir_fixture.h"
#include "ir/storage.h"

namespace minc::test {
namespace {

TEST(IrGlobalTest, AFileScopeBindingBecomesAModuleObject) {
  IrFixture f;
  f.source("const SIZE: i32 = 8;\n"
           "let counter: i32 = 0;\n"
           "let zeroed: i32;\n"
           "fn i32 main() { return SIZE + counter + zeroed; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_TRUE(f.moduleBuilt());
  EXPECT_EQ(f.violations(), 0u);

  const std::string text = f.module();
  // `const` and `let` produce the *same object* -- `memory.md`, decision 15, and
  // the dedicated test below. What differs is which name may be assigned.
  EXPECT_NE(text.find("@SIZE = global i32 8"), std::string::npos) << text;
  // `let` is mutable storage, external by default, and zero when the source gave
  // no initializer -- the `.bss` of the ABI.
  EXPECT_NE(text.find("@counter = global i32 0"), std::string::npos) << text;
  EXPECT_NE(text.find("@zeroed = global i32 0"), std::string::npos) << text;
  // A body reads them through the object and never through a copy.
  EXPECT_NE(text.find("load i32, ptr @SIZE"), std::string::npos) << text;
}

TEST(IrGlobalTest, AStaticBindingIsInternalToTheUnit) {
  IrFixture f;
  f.source("static let hidden: i32 = 3;\n"
           "static fn i32 helper() { return hidden; }\n"
           "fn i32 main() { return helper(); }\n");
  ASSERT_TRUE(f.build());
  ASSERT_TRUE(f.moduleBuilt());
  EXPECT_EQ(f.violations(), 0u);

  const std::string text = f.module();
  // `static` is the one word that narrows linkage, and it is the same word on
  // both kinds of declaration: a function and a binding.
  EXPECT_NE(text.find("@hidden = internal global i32 3"), std::string::npos) << text;
  EXPECT_NE(text.find("define internal i32 @helper()"), std::string::npos) << text;
}

TEST(IrGlobalTest, AFoldedConstantIsMaterialisedAndNotRecomputed) {
  IrFixture f;
  f.source("const FIRST: i32 = LATER * 2;\n"
           "const LATER: i32 = 21;\n"
           "const NEG: i32 = -7;\n"
           "const WIDE: u128 = 170141183460469231731687303715884105727;\n"
           "fn i32 main() { return FIRST + LATER + NEG; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_TRUE(f.moduleBuilt());
  EXPECT_EQ(f.violations(), 0u);

  const std::string text = f.module();
  // A forward reference reached the module *folded*: the order the checker walked
  // is the module's, not the source's, and the object carries the value.
  EXPECT_NE(text.find("@FIRST = global i32 42"), std::string::npos) << text;
  // The sign is the *value* and not a flag: `-7` is two's complement, in the
  // object, so a reader of the module reads one number.
  EXPECT_NE(text.find("@NEG = global i32 -7"), std::string::npos) << text;
  // Wider than the 64-bit core: its digits are the value, and the width is the
  // object's.
  EXPECT_NE(text.find("@WIDE = global i128 170141183460469231731687303715884105727"),
            std::string::npos)
      << text;
}

TEST(IrGlobalTest, AFloatLiteralIsReadFromItsSpellingAndASignIsOneBit) {
  IrFixture f;
  f.source("const RATIO: f64 = 0.5;\n"
           "const NEG: f64 = -1.5;\n"
           "const SMALL: f32 = 2.5;\n"
           "fn i32 main() { return 0; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_TRUE(f.moduleBuilt());
  EXPECT_EQ(f.violations(), 0u);

  const std::string text = f.module();
  EXPECT_NE(text.find("@RATIO = global double 5.000000e-01"), std::string::npos) << text;
  // The negation is a sign bit, so the digits are still the spelling's and no
  // rounding happens anywhere.
  EXPECT_NE(text.find("@NEG = global double -1.500000e+00"), std::string::npos) << text;
  EXPECT_NE(text.find("@SMALL = global float 2.500000e+00"), std::string::npos) << text;
}

TEST(IrGlobalTest, AStringLiteralIsOneObjectAndTheBindingHoldsItsAddress) {
  IrFixture f;
  f.source("const GREETING: str = \"hi\";\n"
           "const OTHER: str = \"hi\";\n"
           "fn i32 main() { return 0; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_TRUE(f.moduleBuilt());
  EXPECT_EQ(f.violations(), 0u);

  const std::string text = f.module();
  // One object for one spelling, because `&x` makes the address observable: two
  // globals for one literal would make two equal pointers compare unequal.
  std::size_t occurrences = 0;
  for (std::size_t at = text.find("@str = private unnamed_addr global"); at != std::string::npos;
       at = text.find("@str = private unnamed_addr global", at + 1)) {
    ++occurrences;
  }
  EXPECT_EQ(occurrences, 1u) << text;
  // The binding's bytes are that object's address.
  EXPECT_NE(text.find("@GREETING = global ptr @str"), std::string::npos) << text;
  EXPECT_NE(text.find("@OTHER = global ptr @str"), std::string::npos) << text;
  // ... and the terminator is part of the object the compiler writes.
  EXPECT_NE(text.find("c\"hi\\00\""), std::string::npos) << text;
}

TEST(IrGlobalTest, NullAndABoolArePublishedAtTheirStorageShape) {
  IrFixture f;
  f.source("let nothing: *i32 = null;\n"
           "const FLAG: bool = true;\n"
           "let off: bool = false;\n"
           "fn i32 main() { return 0; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_TRUE(f.moduleBuilt());
  EXPECT_EQ(f.violations(), 0u);

  const std::string text = f.module();
  EXPECT_NE(text.find("@nothing = global ptr null"), std::string::npos) << text;
  // A `bool` object is a **byte**, and only a load and a store care about the
  // `i1`: the object's shape is the storage shape (`memory.md`, *Objects*).
  EXPECT_NE(text.find("@FLAG = global i8 1"), std::string::npos) << text;
  EXPECT_NE(text.find("@off = global i8 0"), std::string::npos) << text;
}

TEST(IrGlobalTest, AWriteToAFileScopeLetIsAStoreToTheObject) {
  IrFixture f;
  f.source("let runs: i32 = 0;\n"
           "fn void bump() { runs = runs + 1; }\n"
           "fn i32 main() { bump(); bump(); return runs; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_TRUE(f.moduleBuilt());
  EXPECT_EQ(f.violations(), 0u);

  const std::string text = f.module();
  EXPECT_NE(text.find("store i32"), std::string::npos) << text;
  EXPECT_NE(text.find(", ptr @runs"), std::string::npos) << text;
}

TEST(IrGlobalTest, EveryInitializerIsAConstantAndNeverAnInstruction) {
  IrFixture f;
  // Every kind of record in one unit, so the scan is over all four arms at once.
  f.source("const SIZE: i32 = 8;\n"
           "const HALF: f64 = 0.5;\n"
           "const NEG: i32 = -3;\n"
           "const WIDE: i128 = 12345678901234567890123;\n"
           "const GREETING: str = \"hi\";\n"
           "const FLAG: bool = true;\n"
           "let nothing: *i32 = null;\n"
           "let zeroed: i32;\n"
           "static const INTERNAL: i32 = 1;\n"
           "fn i32 main() { return SIZE + NEG; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_TRUE(f.moduleBuilt());
  EXPECT_EQ(f.violations(), 0u);

  // The structural half of the rule is LLVM's: `getInitializer` returns a
  // `Constant`, so an instruction cannot be in one. What this asserts is the
  // other half -- that the module has the object for every binding, in source
  // order, and that nothing was dropped on the way.
  const std::string text = f.module();
  for (const char* name :
       {"SIZE", "HALF", "NEG", "WIDE", "GREETING", "FLAG", "nothing", "zeroed", "INTERNAL"}) {
    EXPECT_NE(text.find("@" + std::string(name) + " = "), std::string::npos)
        << name << " is missing from the module:\n"
        << text;
  }
  // `static const` is both words at once, and each keeps its own meaning: hidden
  // from the linker, and not assignable *by that name*.
  EXPECT_NE(text.find("@INTERNAL = internal global i32 1"), std::string::npos) << text;
}

TEST(IrGlobalTest, AConstBindingIsNotReadOnlyMemory) {
  IrFixture f;
  // `memory.md`, decision 15: **`const` protects a name, not memory.** LLVM's
  // `constant` is the opposite claim -- it makes a write through a pointer to the
  // object undefined behaviour -- so nothing this stage emits may carry it. The
  // source cannot reach such a write today (`&SIZE` is `sema-address-of-const`,
  // because a pointer to it would be a way to write it), and that is exactly why
  // the flag has to stay off: a compiler may not lean on a refusal it may later
  // relax, and the day an explicit `readonly` arrives this is the one place it
  // turns on.
  //
  // This is the regression test for the reflex. It fails the moment someone
  // "optimizes" a `const` global into `.rodata`, which would be a module that is
  // wrong for a program the checker passed.
  f.source("const SIZE: i32 = 8;\n"
           "let size: i32 = 8;\n"
           "const RATIO: f64 = 0.5;\n"
           "const WIDE: i128 = 123;\n"
           "static const HIDDEN: i32 = 1;\n"
           "fn i32 main() { return SIZE + size; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_TRUE(f.moduleBuilt());
  EXPECT_EQ(f.violations(), 0u);

  // The object and its mutability, read off the module's own definitions rather
  // than off the text: an object is "constant" in LLVM's sense exactly when the
  // module says so, and `isConstant` is that.
  llvm::Module& module = ir::ModuleAccess::llvmModule(f.result().module);
  for (const llvm::GlobalVariable& object : module.globals()) {
    EXPECT_FALSE(object.isConstant())
        << "`" << object.getName().str() << "` was emitted as a `constant` object";
  }

  // And the flag is not merely *absent* but **scanned**, which is what makes the
  // rule structural: the two ways a later change could reintroduce it are set
  // here, on a module this compiler would never build, and the scan has to say so.
  // A check nothing can trip is a check that stopped running.
  llvm::GlobalVariable* size = module.getNamedGlobal("SIZE");
  ASSERT_NE(size, nullptr) << f.module();
  size->setConstant(true);
  const std::vector<ir::IRDiagnostic> afterConstant = f.scan();
  ASSERT_EQ(afterConstant.size(), 1u) << f.module();
  EXPECT_EQ(ir::toString(afterConstant.front().code), "ir-assumption");

  // The alignment half, on a fresh module: an object that states an alignment its
  // type does not give is the same class of miscompile as an over-aligned access,
  // and LLVM's default for an understated one is slow code either way.
  IrFixture g;
  g.source("const RATIO: f64 = 0.5;\n"
           "fn i32 main() { return 0; }\n");
  ASSERT_TRUE(g.build());
  ASSERT_TRUE(g.moduleBuilt());
  EXPECT_EQ(g.violations(), 0u);
  llvm::Module& other = ir::ModuleAccess::llvmModule(g.result().module);
  llvm::GlobalVariable* ratio = other.getNamedGlobal("RATIO");
  ASSERT_NE(ratio, nullptr) << g.module();
  ratio->setAlignment(llvm::Align(1));
  const std::vector<ir::IRDiagnostic> afterAlignment = g.scan();
  ASSERT_EQ(afterAlignment.size(), 1u) << g.module();
  EXPECT_EQ(ir::toString(afterAlignment.front().code), "ir-alignment");
}

TEST(IrGlobalTest, DebugInformationNamesAFileScopeObject) {
  IrFixture f;
  f.source("static const SIZE: i32 = 8;\n"
           "fn i32 main() { return SIZE; }\n");
  f.debugInfo();
  ASSERT_TRUE(f.build());
  ASSERT_TRUE(f.moduleBuilt());
  EXPECT_EQ(f.violations(), 0u);

  const std::string text = f.module();
  // A `GlobalVariable` with no debug expression is a symbol `gdb` cannot name, so
  // `-g` has to attach one -- and it says whether the object is local to the
  // unit, which is what `static` means to a debugger.
  EXPECT_NE(text.find("!DIGlobalVariableExpression"), std::string::npos) << text;
  EXPECT_NE(text.find("name: \"SIZE\""), std::string::npos) << text;
  EXPECT_NE(text.find("isLocal: true"), std::string::npos) << text;
}

} // namespace
} // namespace minc::test
