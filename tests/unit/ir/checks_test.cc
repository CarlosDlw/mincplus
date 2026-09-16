// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The checked build: the guards, the site messages, and the runtime entry they
// fail into (`docs/architectures/checks.md`).
//
// Three properties are the point of the tests, and every assertion below is one
// of them:
//
//   * **Every access through a pointer is guarded, and the guard is the one the
//     record asked for.** A null test for every obligation, an alignment test
//     when the type needs one, and a bounds test when the record has an extent.
//   * **The message names the site.** A trap that a reader cannot act on is a
//     tool, not a language, so the string in the module carries the file and the
//     position the source wrote.
//   * **Nothing is emitted when the flag is off.** The checked build has to be a
//     *build*, not a cost every program pays: the same source lowered without the
//     flag carries no guard, no message and no runtime entry.
#include <cstddef>
#include <string>

#include <gtest/gtest.h>

#include "ir/ir.h"
#include "ir/ir_fixture.h"
#include "sema/target.h"
#include "support/span/span.h"

namespace minc::ir {
namespace {

// The permission the language gives a null dereference is that it traps, and the
// guard is what makes that true rather than a segmentation fault the program's
// author cannot read.
constexpr std::string_view kNullDeref = "fn i32 head(p: *i32)\n"
                                        "{\n"
                                        "  return *p;\n"
                                        "}\n"
                                        "fn i32 main() { return 0; }\n";

TEST(IrChecksTest, ANullDerefIsGuardedAndNamesItsSite) {
  test::IrFixture fixture;
  fixture.source(std::string(kNullDeref));
  fixture.checks();
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  const std::string text = fixture.module();
  EXPECT_NE(text.find("icmp eq ptr"), std::string::npos) << text;
  EXPECT_NE(text.find("__minc_check_fail"), std::string::npos) << text;
  // The site, in the message and not only in the module: `test.mx` is the
  // fixture's file name, and `3:10` is where `*p` is written.
  EXPECT_NE(text.find("memory-null at test.mx:3:10"), std::string::npos) << text;
  // The scan agrees: a checked module with a guard in front of every access is a
  // module this compiler is allowed to have built.
  EXPECT_EQ(fixture.violations(), 0u) << text;
}

TEST(IrChecksTest, EveryAccessSpellingIsGuarded) {
  // The three spellings of "reach memory through a pointer", each read and
  // written: `*p`, `p[i]`, and `a[i]` on a named array. The count of tests is the
  // count of accesses, so a spelling that lost its guard fails here rather than
  // in a program nobody ran.
  test::IrFixture fixture;
  fixture.source("fn void store(p: *i32, a: *i32)\n"
                 "{\n"
                 "  *p = 1;\n"
                 "  p[2] = *a;\n"
                 "}\n"
                 "fn i32 read(a: *i32, n: i32)\n"
                 "{\n"
                 "  let table: [4]i32 = [1, 2, 3, 4];\n"
                 "  return *a + a[1] + table[n];\n"
                 "}\n"
                 "fn i32 main() { return 0; }\n");
  fixture.checks();
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  const std::string text = fixture.module();
  // Six accesses: `*p = 1`, `p[2] = ...`, the `*a` on the right of that store,
  // `*a` in `read`, `a[1]`, and `table[n]`. One null test each, and the bounds
  // test for the subscript of an object the record has an extent for -- the
  // array, whose count is in the type.
  std::size_t nullTests = 0;
  std::size_t boundsTests = 0;
  for (std::size_t at = text.find("icmp eq ptr"); at != std::string::npos;
       at = text.find("icmp eq ptr", at + 1)) {
    ++nullTests;
  }
  for (std::size_t at = text.find("icmp uge"); at != std::string::npos;
       at = text.find("icmp uge", at + 1)) {
    ++boundsTests;
  }
  EXPECT_EQ(nullTests, 6u) << text;
  // `table[n]` is the only subscript whose extent is a number in a type: `p[2]`
  // and `a[1]` are subscripts of a *pointer*, which is the case `memory.md`
  // assigns to a shadow memory and this build deliberately does not guess about.
  EXPECT_EQ(boundsTests, 1u) << text;
  // The alignment of an `i32` is four, so the alignment test is emitted too --
  // and for every access, because the number is the access's own type.
  EXPECT_NE(text.find("check.misaligned"), std::string::npos) << text;
  EXPECT_EQ(fixture.violations(), 0u) << text;
}

TEST(IrChecksTest, ASliceSubscriptIsBoundedByItsOwnLength) {
  // The extent of `s[i]` is a *value* -- the descriptor's `len` word -- and this
  // is the test that the record's third answer reaches the module: the bounds
  // test compares the index against the value extracted from the descriptor, and
  // not against a constant the compiler made up.
  test::IrFixture fixture;
  fixture.source("fn i32 at(s: []i32, i: i32)\n"
                 "{\n"
                 "  return s[i];\n"
                 "}\n"
                 "fn i32 main() { return 0; }\n");
  fixture.checks();
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  const std::string text = fixture.module();
  EXPECT_NE(text.find("slice.len"), std::string::npos) << text;
  EXPECT_NE(text.find("icmp uge"), std::string::npos) << text;
  // The comparison is unsigned on purpose: a negative index is a large unsigned
  // number, so one instruction catches both ends of the range.
  EXPECT_EQ(text.find("icmp sge"), std::string::npos) << text;
  EXPECT_NE(text.find("memory-out-of-bounds at test.mx:3:10"), std::string::npos) << text;
  EXPECT_EQ(fixture.violations(), 0u) << text;
}

TEST(IrChecksTest, AnArraySubscriptIsBoundedByItsCount) {
  test::IrFixture fixture;
  fixture.source("fn i32 at(a: i32)\n"
                 "{\n"
                 "  let table: [3]i32 = [1, 2, 3];\n"
                 "  return table[a];\n"
                 "}\n"
                 "fn i32 main() { return 0; }\n");
  fixture.checks();
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  // The count, as the constant the type gives -- and the guard is a comparison
  // against exactly it.
  EXPECT_NE(fixture.module().find("icmp uge i64"), std::string::npos) << fixture.module();
  EXPECT_NE(fixture.module().find(", 3"), std::string::npos) << fixture.module();
  EXPECT_EQ(fixture.violations(), 0u) << fixture.module();
}

TEST(IrChecksTest, AnUncheckedBuildEmitsNoGuardAndNoRuntime) {
  // The same source, twice: the flag has to be the only difference, and the
  // difference has to be the guards and nothing else. This is the property that
  // makes the checked build shippable -- a program pays for the checks only in
  // the build that asked for them.
  constexpr std::string_view kSource = "fn i32 head(p: *i32)\n"
                                       "{\n"
                                       "  return *p;\n"
                                       "}\n"
                                       "fn i32 main() { return 0; }\n";

  test::IrFixture checked;
  checked.source(std::string(kSource));
  checked.checks();
  ASSERT_TRUE(checked.build());
  ASSERT_TRUE(checked.moduleBuilt());

  test::IrFixture unchecked;
  unchecked.source(std::string(kSource));
  ASSERT_TRUE(unchecked.build());
  ASSERT_TRUE(unchecked.moduleBuilt());

  const std::string text = unchecked.module();
  EXPECT_EQ(text.find("__minc_check_fail"), std::string::npos) << text;
  EXPECT_EQ(text.find("check.site"), std::string::npos) << text;
  EXPECT_EQ(text.find("icmp eq ptr"), std::string::npos) << text;
  // The load is still there: what the flag removes is the guard, not the access.
  EXPECT_NE(text.find("load i32"), std::string::npos) << text;
  // And the scan of an unchecked module is clean *without* asking for the guards:
  // the rule is about a module the guards were requested for, and a scan that
  // fired on every unguarded access would fire on every ordinary build.
  EXPECT_EQ(unchecked.violations(), 0u) << text;
}

TEST(IrChecksTest, TheRuntimeEntryIsDefinedOncePerModule) {
  // One entry and one message per site, however many guards call it: the trap
  // path is a function in the module and not a library, and a second definition
  // would be a symbol the linker resolves twice.
  test::IrFixture fixture;
  fixture.source("fn i32 two(p: *i32, q: *i32)\n"
                 "{\n"
                 "  return *p + *q;\n"
                 "}\n"
                 "fn i32 main() { return 0; }\n");
  fixture.checks();
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt());

  const std::string text = fixture.module();
  std::size_t definitions = 0;
  for (std::size_t at = text.find("define internal void @__minc_check_fail");
       at != std::string::npos; at = text.find("define internal void @__minc_check_fail", at + 1)) {
    ++definitions;
  }
  EXPECT_EQ(definitions, 1u)
      << text; // Two sites on one line, so the messages are two objects and not one: the
  // text carries the position, which is what makes the pair distinguishable.
  EXPECT_NE(text.find("memory-null at test.mx:3:10"), std::string::npos) << text;
  EXPECT_NE(text.find("memory-null at test.mx:3:15"), std::string::npos) << text;
}

TEST(IrChecksTest, AnIndexTheCompilerCanDecideEmitsNoTest) {
  // A subscript whose index and extent are both constants is not a missing check:
  // the comparison is emitted and LLVM's own folder answers it, so what reaches
  // the module is the answer and not the test. This is the one place the guards
  // are not in the text, and it is why the scan's escapes are about the *address*
  // rather than about the branch (`invariants.cc`, `guardBranch`).
  test::IrFixture fixture;
  fixture.source("fn i32 at()\n"
                 "{\n"
                 "  let table: [3]i32 = [1, 2, 3];\n"
                 "  return table[2];\n"
                 "}\n"
                 "fn i32 main() { return 0; }\n");
  fixture.checks();
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();
  // No bounds test survives, and the null and alignment guards -- whose operand is
  // an address and not a number -- still do.
  EXPECT_EQ(fixture.module().find("icmp uge"), std::string::npos) << fixture.module();
  EXPECT_NE(fixture.module().find("icmp eq ptr"), std::string::npos) << fixture.module();
  EXPECT_TRUE(fixture.scan().empty()) << fixture.module();
}

TEST(IrChecksTest, TheGuardsDoNotDisturbTheAssumptionScan) {
  // The checked build adds a function, a global and a branch per guard, and every
  // one of them is something the assumption list has an opinion about: the runtime
  // entry must carry no attribute, the message objects must not be `constant`, and
  // no instruction may carry metadata. A checked example is the cheapest way to
  // say all three at once.
  test::IrFixture fixture;
  fixture.source("fn i32 sum(s: []i32, i: i32)\n"
                 "{\n"
                 "  let table: [4]i32 = [1, 2, 3, 4];\n"
                 "  return s[i] + table[i];\n"
                 "}\n"
                 "fn i32 main() { return 0; }\n");
  fixture.checks();
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();
  EXPECT_TRUE(fixture.scan().empty()) << fixture.module();
}

} // namespace
} // namespace minc::ir
