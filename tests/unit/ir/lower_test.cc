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
  // rather than the host's.
  EXPECT_NE(text.find("target triple = \"x86_64-unknown-linux-gnu\""), std::string::npos);
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
  for (std::size_t at = text.find("= private unnamed_addr constant"); at != std::string::npos;
       at = text.find("= private unnamed_addr constant", at + 1)) {
    ++globals;
  }
  EXPECT_EQ(globals, 1U) << text;
}

TEST(IrLowerTest, ATypeTheTargetHasNoShapeForIsRefusedAndNotCrashed) {
  // `f80` is the x87 format. A target whose ABI has no x87 `long double` has no
  // LLVM type for it, so the mapper refuses -- and the refusal has to reach the
  // caller as a diagnostic, not as a null `alloca`. That was the crash: the
  // binding's slot was built from the mapper's null and the process died where
  // the reader was owed a message. Now the unit is refused, the module is not
  // built, and the message names the type and the target.
  const std::optional<sema::TargetInfo> aarch64 = sema::targetFromName(sema::kTripleLinuxAarch64);
  ASSERT_TRUE(aarch64.has_value());
  test::IrFixture fixture("test.mx", *aarch64);
  fixture.source("fn i32 main()\n{\n  let x: f80 = 0.0;\n  return 0;\n}\n");
  ASSERT_TRUE(fixture.build());

  EXPECT_TRUE(fixture.result().failed());
  EXPECT_FALSE(fixture.moduleBuilt());
  EXPECT_TRUE(fixture.hasError("ir-unsupported-type"));
  // One refusal, one message: a mapper asked twice for the same unmappable type
  // used to record the diagnostic twice.
  EXPECT_EQ(fixture.diagnostics().size(), 1U);
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

  for (const std::string& path : files) {
    test::IrFixture fixture(path);
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
