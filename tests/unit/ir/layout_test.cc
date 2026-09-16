// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The layout table against the target's own data layout.
//
// `memory.md` states the cross-platform claim in one sentence -- the sizes and
// alignments come from `sema`'s table and `ir` checks that table against LLVM's
// `DataLayout` for the *target* -- and this file is the half of it that a reader
// can run. It is the check that was missing when i386 was wrong: that ABI aligns
// a 64-bit value to four bytes (`i64:32:64`) and puts the x87 format in a
// four-byte slot (`f80:32`), so this compiler stated alignment 8 for an `i64` and
// *sixteen bytes* for an `f80` on a target whose answer is 4 and 12. Nothing in
// `sema` can notice that -- it does not link LLVM, deliberately -- so the notice
// happens where the two tables meet, which is the lowering of the first type into
// a module.
//
// Two tests, and they are the two directions of one claim: every named target
// agrees, and a table that does *not* agree is refused instead of emitted. The
// second is what makes the first worth anything -- agreement that nothing checks
// is agreement that lasts until somebody edits a row.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "llvm/IR/DataLayout.h"
#include "llvm/TargetParser/Triple.h"

#include "ir/ir.h"
#include "ir/ir_fixture.h"
#include "ir/storage.h"
#include "sema/target.h"

namespace minc::test {
namespace {

// Every triple the project names, so a row that is right for x86_64 and wrong for
// i386 fails here rather than on a user's machine.
constexpr std::string_view kTriples[] = {
    sema::kTripleLinuxAmd64,    sema::kTripleWindowsAmd64, sema::kTripleLinuxI386,
    sema::kTripleLinuxAarch64,  sema::kTripleLinuxRiscv64, sema::kTripleDarwinAmd64,
    sema::kTripleDarwinAarch64,
};

// A program that interns and *uses* a broad set of the types the language has:
// every integer width, both of the float widths every target has, the two
// character-ish scalars, the null-terminated string, pointers, an array of one,
// and a slice. `f80` is asked for only where the machines have x87, because
// elsewhere the *checker* refuses the spelling with a sentence of its own -- which
// is a different claim, and one `sema`'s suite pins from the other side
// (`TypeSpecTest.Float80IsRefusedWhereTheMachineHasNoX87`).
[[nodiscard]] std::string wideProgram(const sema::TargetInfo& target) {
  std::string source =
      "fn i64 narrow(a: i8, b: i16, c: i32, d: i64, e: i128, f: u8, g: u16, h: u32, i: u128,\n"
      "             j: char, k: bool, m: str, n: *i32, o: isize, p: usize, q: f32, r: f64)\n"
      "{\n"
      "  let bytes: u8 = f;\n"
      "  let table: [4]i32 = [1, 2, 3, 4];\n"
      "  let flags: [3]bool = [true, false, true];\n"
      "  let words: [2]i64 = [d, 1];\n"
      "  let view: []i32 = table[0..4];\n"
      "  let wide: [2]f64 = [1.0, 2.0];\n"
      "  let sum: f64 = wide[0] + wide[1];\n"
      "  let deep: i128 = e;\n"
      "  let small: i8 = a;\n"
      "  let bits: u32 = h;\n"
      "  let big: u128 = i;\n"
      "  let letter: char = j;\n"
      "  let flag: bool = k;\n"
      "  let index: isize = o;\n"
      "  let single: f32 = q;\n"
      "  return d + words[1] + view[0];\n"
      "}\n";
  // The x87 format, where the machine has one -- which includes the Windows
  // triples, where `long double` is a `double` and the *format* is still there to
  // be named (`TargetInfo::hasFloat80`, and `longDoubleBits` is a different
  // question). Its *object* is the row that differs most between the two ABIs
  // (twelve bytes on i386, sixteen on System V), so a test that skipped it would
  // skip the number that was wrong.
  if (target.hasFloat80()) {
    source += "fn i64 x87(v: f80)\n"
              "{\n"
              "  let wides: [2]f80 = [1.0, 2.0];\n"
              "  let one: f80 = wides[0];\n"
              "  if v == one { return 1; }\n"
              "  return 0;\n"
              "}\n"
              "fn i64 longDouble(ld: long double)\n"
              "{\n"
              "  let copy: long double = ld;\n"
              "  let pair: [2]long double = [1.0, 2.0];\n"
              "  if copy == pair[0] { return 1; }\n"
              "  return 0;\n"
              "}\n";
  }
  return source;
}

TEST(IrLayoutTest, EveryNamedTargetAgreesWithItsDataLayout) {
  for (const std::string_view name : kTriples) {
    const std::optional<sema::TargetInfo> target = sema::targetFromName(name);
    ASSERT_TRUE(target.has_value()) << name;
    IrFixture fixture("layout.mx", *target);
    fixture.source(wideProgram(*target));
    ASSERT_TRUE(fixture.build()) << name;
    // A disagreement is an `ir-internal` -- this compiler's table against LLVM's --
    // so the assertion is that the module was built and that nothing was said.
    EXPECT_TRUE(fixture.moduleBuilt()) << name << ": " << fixture.module();
    EXPECT_TRUE(fixture.diagnostics().empty()) << name;
    // The module names the target it was built for. Not by comparing the string
    // we passed: LLVM answers with *its* spelling for the architecture, and
    // `i686-unknown-linux-gnu` prints as `i386-unknown-linux-gnu` there. That is
    // the one place two spellings of one target are legitimate -- the identity
    // here is the `Triple` LLVM built, which is why the field is set from the
    // name rather than from a second list.
    EXPECT_NE(fixture.module().find("target triple = \""), std::string::npos) << name;
  }
}

TEST(IrLayoutTest, EveryNamedTripleIsTheOneLlmParses) {
  // The other half of the same claim, and the half a *layout* test cannot make:
  // the row is selected by components this compiler parses itself -- it does not
  // link LLVM, deliberately -- so a triple LLVM reads as a different machine
  // would give a module aimed at one target with the sizes of another. Nothing in
  // `sema` can notice that; here the two parsers are asked the same question.
  for (const std::string_view name : kTriples) {
    const std::optional<sema::TargetInfo> target = sema::targetFromName(name);
    ASSERT_TRUE(target.has_value()) << name;
    const llvm::Triple parsed{std::string(name)};

    llvm::Triple::ArchType arch = llvm::Triple::UnknownArch;
    switch (target->triple.arch) {
    case sema::Arch::x86_64:
      arch = llvm::Triple::x86_64;
      break;
    case sema::Arch::aarch64:
      arch = llvm::Triple::aarch64;
      break;
    case sema::Arch::riscv64:
      arch = llvm::Triple::riscv64;
      break;
    case sema::Arch::i386:
      // `i686` is an `x86` to LLVM, and it is the same machine here: the row
      // states the 32-bit ABI, which is what both parsers agreed on.
      arch = llvm::Triple::x86;
      break;
    }
    EXPECT_EQ(parsed.getArch(), arch) << name;

    switch (target->triple.os) {
    case sema::OsFamily::linux:
      EXPECT_TRUE(parsed.isOSLinux()) << name;
      break;
    case sema::OsFamily::windows:
      EXPECT_TRUE(parsed.isOSWindows()) << name;
      break;
    case sema::OsFamily::darwin:
      EXPECT_TRUE(parsed.isMacOSX()) << name;
      break;
    case sema::OsFamily::freebsd:
      EXPECT_TRUE(parsed.isOSFreeBSD()) << name;
      break;
    }

    // **Only the environments that select an ABI are compared**, which is this
    // compiler's own rule (`target.h`: the environment is part of the identity
    // where it changes the ABI, and `msvc` is why Windows has no default). A bare
    // Unix triple's environment is LLVM's business to default, and asserting it
    // would be asserting a detail of its parser rather than a fact of the target.
    switch (target->triple.env) {
    case sema::Env::msvc:
      EXPECT_EQ(parsed.getEnvironment(), llvm::Triple::MSVC) << name;
      break;
    case sema::Env::gnu:
      EXPECT_EQ(parsed.getEnvironment(), llvm::Triple::GNU) << name;
      break;
    case sema::Env::musl:
      EXPECT_EQ(parsed.getEnvironment(), llvm::Triple::Musl) << name;
      break;
    case sema::Env::none:
      break;
    }

    // The pointer width, from the data layout LLVM builds for the triple -- the
    // number `isize`, `usize`, every pointer and every slice descriptor is made
    // of. The module is built here so the layout is LLVM's own answer for the
    // target and not a second call to the same target-machine code.
    IrFixture fixture("triple.mx", *target);
    fixture.source("fn i32 main() { return 0; }\n");
    ASSERT_TRUE(fixture.build()) << name;
    ASSERT_TRUE(fixture.moduleBuilt()) << name;
    const llvm::DataLayout& layout = ir::ModuleAccess::layout(fixture.result().module);
    EXPECT_EQ(layout.getPointerSizeInBits(0), target->pointerBits) << name;
  }
}

TEST(IrLayoutTest, ATableThatDisagreesWithTheDataLayoutIsRefusedAndNotEmitted) {
  // The direction that makes the agreement above worth checking: a wrong row is a
  // refusal naming both numbers, not a module with a wrong alignment in it. The
  // row is doctored by hand because that is exactly the edit a future change
  // makes -- and the point of the check is that the edit cannot reach an object
  // file.
  std::optional<sema::TargetInfo> target = sema::targetFromName(sema::kTripleLinuxAmd64);
  ASSERT_TRUE(target.has_value());
  target->int64AlignBits = 128;
  IrFixture fixture("layout.mx", *target);
  fixture.source("fn i64 main()\n{\n  let value: i64 = 1;\n  return value;\n}\n");
  ASSERT_TRUE(fixture.build());

  EXPECT_FALSE(fixture.moduleBuilt());
  ASSERT_EQ(fixture.diagnostics().size(), 1u);
  const ir::IRDiagnostic& diagnostic = fixture.diagnostics().front();
  EXPECT_EQ(diagnostic.code, ir::IRDiagnosticCode::Internal);
  // Both numbers, the type and the target: the sentence has to say which two
  // tables disagreed and about what, because the fix is a row in one of them.
  EXPECT_NE(diagnostic.message.find("i64"), std::string::npos) << diagnostic.message;
  EXPECT_NE(diagnostic.message.find("alignment 16"), std::string::npos) << diagnostic.message;
  EXPECT_NE(diagnostic.message.find("says 8 and 8"), std::string::npos) << diagnostic.message;
  EXPECT_NE(diagnostic.message.find(sema::kTripleLinuxAmd64), std::string::npos)
      << diagnostic.message;
}

} // namespace
} // namespace minc::test
