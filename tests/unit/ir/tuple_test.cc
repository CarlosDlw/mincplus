// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The product in the module: its shape, the two ways it is built and taken apart,
// how it crosses a call, the guard in front of a member access, and the debug
// record (`tuples.md`).
//
// Module assertions rather than execution tests, for the reason `slice_test.cc`
// gives: what is under test is the *shape* this stage emits, and the answers are
// `driver/build_command_test.cc`'s, where the program is linked and run.
#include <gtest/gtest.h>

#include <string>

#include "ir/ir_fixture.h"

namespace minc::test {
namespace {

TEST(TupleIrTest, TheShapeIsAnUnnamedStructOfStorageMembers) {
  // `{ i32, i8 }` for `(i32, bool)`: the members are their **storage** types, so a
  // `bool` in an aggregate is a byte and not a bit -- `[4]bool` and `(bool, bool)`
  // have one representation each, and it is the same rule (`arrays.md` decision
  // 12). And the struct is *literal* and not a named type: a product has no name
  // in the language, so a name in the module would be a claim about identity
  // (`tuples.md`, decision 11).
  IrFixture f;
  f.source("fn i32 main() {\n  let p = (1, true);\n  return 0;\n}\n");
  ASSERT_TRUE(f.build());
  ASSERT_TRUE(f.moduleBuilt());
  const std::string module = f.module();
  EXPECT_NE(module.find("%p = alloca { i32, i8 }"), std::string::npos) << module;
  EXPECT_EQ(module.find("%Tuple"), std::string::npos) << module;
  EXPECT_EQ(f.violations(), 0u);
}

TEST(TupleIrTest, AProductOfKnownMembersIsAConstant) {
  // Every member known at compile time is a `ConstantStruct`: the object is a
  // value, and a value leaves no instructions behind.
  IrFixture f;
  f.source("fn i32 main() {\n  let p = (1, 2);\n  return 0;\n}\n");
  ASSERT_TRUE(f.build());
  ASSERT_TRUE(f.moduleBuilt());
  const std::string module = f.module();
  EXPECT_NE(module.find("store { i32, i32 } { i32 1, i32 2 }"), std::string::npos) << module;
  EXPECT_EQ(module.find("insertvalue"), std::string::npos) << module;
  EXPECT_EQ(f.violations(), 0u);
}

TEST(TupleIrTest, AProductWithAComputedMemberIsBuiltFromPoison) {
  // The other half: a member that is not known is an `insertvalue` chain from
  // poison, and **no `alloca`**. Materialising storage for a product the consumer
  // is about to read would give a value two shapes, and a value has one
  // (`tuples.md`, decision 7).
  IrFixture f;
  f.source("fn i32 twice(a: i32) { return a + a; }\n"
           "fn i32 main() {\n  let p = (twice(1), 2);\n  return 0;\n}\n");
  ASSERT_TRUE(f.build());
  ASSERT_TRUE(f.moduleBuilt());
  const std::string module = f.module();
  EXPECT_NE(module.find("%product = insertvalue { i32, i32 } poison, i32 %call, 0"),
            std::string::npos)
      << module;
  EXPECT_NE(module.find("%product1 = insertvalue { i32, i32 } %product, i32 2, 1"),
            std::string::npos)
      << module;
  EXPECT_EQ(f.violations(), 0u);
}

TEST(TupleIrTest, AMemberOfAPlaceIsAGepWithAnI32Index) {
  // The member's *address*: a `getelementptr` whose second index selects a struct
  // member, which LLVM requires to be a constant `i32` -- the first index walks the
  // pointer at the address width and the second numbers a member, and the two
  // widths are not the same thing (`tuples.md`, decision 4). An `i64` second index
  // is rejected by the verifier, which is how this test caught it.
  IrFixture f;
  f.source("fn i32 main() {\n  let p = (1, 2);\n  p.1 = 7;\n  return p.0;\n}\n");
  ASSERT_TRUE(f.build());
  ASSERT_TRUE(f.moduleBuilt());
  const std::string module = f.module();
  EXPECT_NE(module.find("getelementptr { i32, i32 }, ptr %p, i64 0, i32 1"), std::string::npos)
      << module;
  EXPECT_EQ(module.find("getelementptr { i32, i32 }, ptr %p, i64 0, i64"), std::string::npos)
      << module;
  EXPECT_EQ(f.violations(), 0u);
}

TEST(TupleIrTest, AMemberOfAValueIsAnExtractValue) {
  // The member of a **value** -- a call's result, which lives in registers -- is
  // an `extractvalue`. `p.1` on a *binding* is not this case and must not be: a
  // binding is a place, so its member is reached through an address
  // (`tuples.md`, decision 9). Which of the two this is comes from the checker's
  // record and never from a second look at the base.
  IrFixture f;
  f.source("fn (i32, i32) pair() { return (1, 2); }\n"
           "fn i32 main() {\n  let t = pair().1;\n  return t;\n}\n");
  ASSERT_TRUE(f.build());
  ASSERT_TRUE(f.moduleBuilt());
  const std::string module = f.module();
  EXPECT_NE(module.find("%member = extractvalue { i32, i32 }"), std::string::npos) << module;
  // A value path is an extract and never a `getelementptr`: nothing in registers
  // has an address.
  EXPECT_EQ(module.find("member = getelementptr"), std::string::npos) << module;
  EXPECT_EQ(f.violations(), 0u);
}

TEST(TupleIrTest, AMemberAccessIsGuardedInTheCheckedBuild) {
  // A product is an object, and an access through a pointer into it is an access
  // like any other: the checked build puts the null and alignment guards in front
  // of it, and the *record* is what says so rather than a rule here
  // (`checks.md`). A bounds test is absent by construction -- the position is a
  // constant the checker already bounded against the arity (`tuples.md`,
  // decision 11).
  IrFixture f;
  f.checks(true);
  f.source("fn i32 main() {\n  let p = (1, 2);\n  return p.0;\n}\n");
  ASSERT_TRUE(f.build());
  ASSERT_TRUE(f.moduleBuilt());
  const std::string module = f.module();
  EXPECT_NE(module.find("check.null"), std::string::npos) << module;
  EXPECT_NE(module.find("check.misaligned"), std::string::npos) << module;
  EXPECT_EQ(module.find("check.outside"), std::string::npos) << module;
  EXPECT_EQ(f.violations(), 0u);
}

TEST(TupleIrTest, AnAggregateReturnIsWrittenIntoTheCallersObject) {
  // The same convention `[N]T` already has: a product is returned through an
  // `sret` destination, so a big product moves through the caller's storage and
  // not through a copy in the callee's frame (`arrays.md` decision 13,
  // `tuples.md` decision 12).
  IrFixture f;
  f.source("fn (i32, bool) divmod(a: i32, b: i32) { return (a / b, a > b); }\n"
           "fn i32 main() {\n  let p = divmod(7, 2);\n  return p.0;\n}\n");
  ASSERT_TRUE(f.build());
  ASSERT_TRUE(f.moduleBuilt());
  const std::string module = f.module();
  EXPECT_NE(module.find("define void @divmod(ptr sret({ i32, i8 })"), std::string::npos) << module;
  EXPECT_NE(module.find("call void @divmod(ptr %sret, i32 7, i32 2)"), std::string::npos) << module;
  EXPECT_EQ(f.violations(), 0u);
}

TEST(TupleIrTest, ADestructuringLoadsOnceAndStoresEachMember) {
  // One evaluation of the value and one slot per name: the product is loaded
  // **once** and each member is an `extractvalue` out of that one value
  // (`tuples.md`, decision 6). Two loads would be two reads of a value that could
  // have changed between them, which is a program the source did not write.
  IrFixture f;
  f.source("fn i32 main() {\n  let p = (1, 2);\n  let (a, b) = p;\n  return a + b;\n}\n");
  ASSERT_TRUE(f.build());
  ASSERT_TRUE(f.moduleBuilt());
  const std::string module = f.module();
  EXPECT_NE(module.find("%a = alloca i32"), std::string::npos) << module;
  EXPECT_NE(module.find("%b = alloca i32"), std::string::npos) << module;
  EXPECT_NE(module.find("%load = load { i32, i32 }, ptr %p"), std::string::npos) << module;
  EXPECT_NE(module.find("%member = extractvalue { i32, i32 } %load, 0"), std::string::npos)
      << module;
  EXPECT_NE(module.find("%member1 = extractvalue { i32, i32 } %load, 1"), std::string::npos)
      << module;
  EXPECT_NE(module.find("store i32 %member, ptr %a"), std::string::npos) << module;
  EXPECT_NE(module.find("store i32 %member1, ptr %b"), std::string::npos) << module;
  EXPECT_EQ(f.violations(), 0u);
}

TEST(TupleIrTest, ASkippedPositionTakesNoSlotAndNoMember) {
  IrFixture f;
  f.source("fn i32 main() {\n  let p = (1, 2);\n  let (a, _) = p;\n  return a;\n}\n");
  ASSERT_TRUE(f.build());
  ASSERT_TRUE(f.moduleBuilt());
  const std::string module = f.module();
  EXPECT_NE(module.find("%a = alloca i32"), std::string::npos) << module;
  EXPECT_EQ(module.find("%_ = alloca"), std::string::npos) << module;
  // Only the member that is bound is taken out of the product.
  EXPECT_EQ(module.find("extractvalue { i32, i32 } %load, 1"), std::string::npos) << module;
  EXPECT_EQ(f.violations(), 0u);
}

TEST(TupleIrTest, TheDebugRecordIsAnUnnamedStructWithPositionNamedMembers) {
  // One `DW_TAG_structure_type` with **no name** and a `DW_TAG_member` per
  // position called `__0`, `__1`: a nameless struct is what an anonymous C
  // `struct` is, and `__0` rather than `0` is the spelling rustc's tuples reach
  // DWARF with -- a name in DWARF is an identifier, and a position is a number
  // (`tuples.md`, decision 14). GDB's `ptype` on such a type prints
  // `struct { i32 __0; bool __1; }`, which is the whole requirement.
  IrFixture f;
  f.debugInfo(true);
  f.source("fn i32 main() {\n  let p = (1, true);\n  return p.0;\n}\n");
  ASSERT_TRUE(f.build());
  ASSERT_TRUE(f.moduleBuilt());
  const std::string module = f.module();
  EXPECT_NE(module.find("DW_TAG_structure_type"), std::string::npos) << module;
  EXPECT_NE(module.find("DW_TAG_member"), std::string::npos) << module;
  EXPECT_NE(module.find("__0"), std::string::npos) << module;
  EXPECT_NE(module.find("__1"), std::string::npos) << module;
  EXPECT_EQ(f.violations(), 0u);
}

} // namespace
} // namespace minc::test
