// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Debug information: the five properties that make `-g` worth having, and the
// three that keep it from being a liability.
//
// The properties are asserted on the *textual* module rather than through a
// debugger, because the text is what `mincc ir -g` prints and what a reader
// reviews; `gdb` is the next consumer down and its contract is DWARF, not this
// compiler's. A test that spawned a debugger would be a test of `gdb`.
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "ir/ir_fixture.h"

namespace minc {
namespace {

// `!dbg !17` removed from a line, and the space before it with it. The attachment
// is *last* on an instruction line but sits *before* the `{` on a `define`, so the
// line cannot simply be truncated at it.
void stripDbgAttachment(std::string& line) {
  for (;;) {
    const std::size_t at = line.find("!dbg !");
    if (at == std::string::npos) {
      break;
    }
    std::size_t end = at + 6; // past "!dbg !"
    while (end < line.size() && line[end] >= '0' && line[end] <= '9') {
      ++end;
    }
    std::size_t start = at;
    if (start > 0 && line[start - 1] == ' ') {
      --start;
    }
    line.erase(start, end - start);
    // `ret i32 %x, !dbg !8` leaves a dangling comma behind.
    if (!line.empty() && line.back() == ',') {
      line.pop_back();
    }
  }
}

// A module's text with everything debug-related removed: the `!dbg` attachments,
// the `#dbg_` records between instructions, the numbered metadata, and the blank
// lines that separate them. What is left is the *code*, which is the thing `-g`
// may not change.
[[nodiscard]] std::string stripDebug(const std::string& module) {
  std::string out;
  std::size_t at = 0;
  while (at <= module.size()) {
    const std::size_t end = module.find('\n', at);
    std::string line = module.substr(at, end == std::string::npos ? std::string::npos : end - at);
    // A record line is nothing *but* debug information, so it goes whole; an
    // attachment is a suffix or an infix and is removed in place.
    if (line.find("#dbg_") == std::string::npos && line.rfind("!", 0) != 0 && !line.empty()) {
      stripDbgAttachment(line);
      out += line;
      out += '\n';
    }
    if (end == std::string::npos) {
      break;
    }
    at = end + 1;
  }
  return out;
}

// A program with a parameter, a local and a call: every kind of debug object the
// lowering can produce, in six lines.
constexpr std::string_view kProgram = R"(fn i32 add(left: i32, right: i32)
{
  let sum: i32 = left + right;
  return sum;
}

fn i32 main()
{
  let answer: i32 = add(1, 2);
  return answer;
}
)";

TEST(IrDebugTest, DebugInfoIsOffUnlessItIsAskedFor) {
  // The default, and the reason `LoweringOptions` exists: a build that did not ask
  // for a debugger's benefit must not pay for metadata in every pass. The absence
  // is checked positively -- the scanner's own rule is that nothing but a debug
  // location may be attached, and it finds nothing here.
  test::IrFixture fixture;
  ASSERT_TRUE(fixture.source(std::string(kProgram)).build());
  ASSERT_TRUE(fixture.moduleBuilt());
  EXPECT_EQ(fixture.module().find("llvm.dbg.cu"), std::string::npos);
  EXPECT_EQ(fixture.module().find("!dbg"), std::string::npos);
  EXPECT_EQ(fixture.module().find("Dwarf Version"), std::string::npos);
  EXPECT_EQ(fixture.violations(), 0u);
}

TEST(IrDebugTest, TheModuleCarriesTheCompileUnitAndTheTwoVersionFlags) {
  // Decision 15 of `codegen.md`: a consumer rejects metadata whose version flag is
  // missing, and the message it prints is about metadata rather than about the
  // flag. Both flags and the named node are checked, because each of the three is
  // a separate way for a debugger to silently show nothing.
  test::IrFixture fixture;
  ASSERT_TRUE(fixture.source(std::string(kProgram)).debugInfo().producer("minc+ test").build());
  ASSERT_TRUE(fixture.moduleBuilt());
  const std::string module = fixture.module();

  EXPECT_NE(module.find("!llvm.dbg.cu = !{"), std::string::npos);
  EXPECT_NE(module.find("!DICompileUnit("), std::string::npos);
  EXPECT_NE(module.find("\"Debug Info Version\", i32 3"), std::string::npos);
  EXPECT_NE(module.find("\"Dwarf Version\", i32 4"), std::string::npos);
  // The producer is carried, because it is how a reader learns which compiler
  // produced a line table that looks wrong.
  EXPECT_NE(module.find("producer: \"minc+ test\""), std::string::npos);
  // `DW_LANG_C99` deliberately: a debugger picks its *expression parser* from this
  // field, and every one of them parses C expressions -- which are this
  // language's operators.
  EXPECT_NE(module.find("DW_LANG_C99"), std::string::npos);
}

TEST(IrDebugTest, EveryFunctionGetsASubprogramAndInstructionsGetLocations) {
  test::IrFixture fixture;
  ASSERT_TRUE(fixture.source(std::string(kProgram)).debugInfo().build());
  ASSERT_TRUE(fixture.moduleBuilt());
  const std::string module = fixture.module();

  // One subprogram per function, named and defined, and attached to the function
  // itself -- that attachment is what makes a backtrace name the function rather
  // than an address.
  EXPECT_NE(module.find("!DISubprogram(name: \"add\""), std::string::npos);
  EXPECT_NE(module.find("!DISubprogram(name: \"main\""), std::string::npos);
  EXPECT_NE(module.find("DISPFlagDefinition"), std::string::npos);
  EXPECT_NE(module.find("!DILocation("), std::string::npos);
  // A location that points at a real line and column, not at 0:0. Line 3 is
  // `let sum: i32 = left + right;` and line 9 is the `let` in `main`.
  EXPECT_NE(module.find("line: 3"), std::string::npos);
  EXPECT_NE(module.find("line: 9"), std::string::npos);
  EXPECT_EQ(module.find("column: 0, scope"), std::string::npos);
}

TEST(IrDebugTest, ABindingIsDeclaredAtItsFrameSlot) {
  // `#dbg_declare` and not a `dbg.value`: a binding in this language has an
  // address (`&x` is a legal expression), so the debugger must be told where the
  // object *is* rather than what value it happens to hold at one point.
  test::IrFixture fixture;
  ASSERT_TRUE(fixture.source(std::string(kProgram)).debugInfo().build());
  ASSERT_TRUE(fixture.moduleBuilt());
  const std::string module = fixture.module();

  EXPECT_NE(module.find("#dbg_declare(ptr %"), std::string::npos);
  EXPECT_NE(module.find("!DILocalVariable(name: \"sum\""), std::string::npos);
  EXPECT_NE(module.find("!DILocalVariable(name: \"answer\""), std::string::npos);
  EXPECT_NE(module.find("!DILocalVariable(name: \"left\""), std::string::npos);
  // The type of a `i32` is one basic type, named and sized as the checker's
  // `TypeStore` spells and sizes it. A debug type with its own idea of a width
  // shows the reader the wrong number, which is worse than showing them nothing.
  EXPECT_NE(module.find("!DIBasicType(name: \"i32\", size: 32, encoding: DW_ATE_signed)"),
            std::string::npos);
}

TEST(IrDebugTest, DebugInformationIsRecordsAndNotIntrinsicCalls) {
  // LLVM 19+ made records the default representation and the reference forbids the
  // two in one module: a module that mixes them verifies and produces a debugger
  // that lies. A `#dbg_` line is a record; an `llvm.dbg.` call is the legacy form.
  test::IrFixture fixture;
  ASSERT_TRUE(fixture.source(std::string(kProgram)).debugInfo().build());
  ASSERT_TRUE(fixture.moduleBuilt());
  const std::string module = fixture.module();

  EXPECT_NE(module.find("#dbg_declare"), std::string::npos);
  // `!llvm.dbg.cu` is the named metadata node and stays; what may not appear is a
  // *declaration or call* of the legacy intrinsic.
  EXPECT_EQ(module.find("@llvm.dbg."), std::string::npos);
  EXPECT_EQ(module.find("declare void @llvm.dbg"), std::string::npos);
  EXPECT_EQ(module.find("call void @llvm.dbg"), std::string::npos);
  // And the scan agrees, so the rule is enforced on every module and not only
  // here: a legacy intrinsic would be an `ir-assumption` violation.
  EXPECT_EQ(fixture.violations(), 0u);
}

TEST(IrDebugTest, TheAssumptionScanPermitsADebugLocationAndNothingElse) {
  // The permit-list, as a test: a module with `-g` is a module this compiler is
  // allowed to have built -- `!dbg` says where the code came from and licenses no
  // transformation -- while the same module scanned for *any* other attachment
  // would fail. The second half is the existing rule, still enforced.
  test::IrFixture fixture;
  ASSERT_TRUE(fixture.source(std::string(kProgram)).debugInfo().build());
  ASSERT_TRUE(fixture.moduleBuilt());
  EXPECT_EQ(fixture.violations(), 0u);
}

TEST(IrDebugTest, DebugInformationWithNoSourceFileIsRefusedRatherThanGuessed) {
  // The driver always has the file, so this is a caller bug -- and a line table
  // for the wrong file is worse than no line table, which is why it is refused
  // instead of defaulted to an empty `DIFile`.
  test::IrFixture fixture;
  ASSERT_TRUE(
      fixture.source("fn i32 main()\n{\n  return 1;\n}\n").debugInfo().noSourceFile().build());
  EXPECT_FALSE(fixture.moduleBuilt());
  EXPECT_TRUE(fixture.hasError("ir-internal"));
}

TEST(IrDebugTest, DebugInformationDoesNotChangeTheInstructionsOnlyTheirLocations) {
  // The property that makes `-g` free to leave on, and the one a later change to
  // the lowering is most likely to break. `codegen.md` calls it checkable exactly,
  // and this is the check: the same program, with and without `-g`, must produce
  // the same *instructions*. Compared after dropping the `!dbg` attachments and
  // the debug records, which are the only things allowed to differ.
  test::IrFixture plain;
  ASSERT_TRUE(plain.source(std::string(kProgram)).build());
  ASSERT_TRUE(plain.moduleBuilt());

  test::IrFixture debugged;
  ASSERT_TRUE(debugged.source(std::string(kProgram)).debugInfo().build());
  ASSERT_TRUE(debugged.moduleBuilt());

  EXPECT_EQ(stripDebug(plain.module()), stripDebug(debugged.module()));
}

} // namespace
} // namespace minc
