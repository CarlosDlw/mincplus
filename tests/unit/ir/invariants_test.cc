// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The scan, and the vocabulary it reports in.
//
// The scan is the mechanism behind `ir.md`'s claim that the list of assumptions
// this compiler hands to the optimizer is closed. These tests pin the two halves
// of that claim: a module this compiler built passes clean, and the enumeration
// of codes has no entry without a name (so a code added to the enum without a
// table row fails here rather than in a user's terminal).
#include <cstddef>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "ir/invariants.h"
#include "ir/ir.h"
#include "ir/ir_fixture.h"

namespace minc::ir {
namespace {

TEST(IrInvariantsTest, EveryBuiltModulePasses) {
  test::IrFixture fixture;
  fixture.source("fn i32 sum(n: i32)\n"
                 "{\n"
                 "  let total: i32 = 0;\n"
                 "  let i: i32 = 0;\n"
                 "  while i < n\n"
                 "  {\n"
                 "    total += i;\n"
                 "    i += 1;\n"
                 "  }\n"
                 "  return total;\n"
                 "}\n"
                 "fn i32 main() { return sum(4) / 2; }\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  const std::vector<IRDiagnostic> violations = fixture.scan();
  for (const IRDiagnostic& violation : violations) {
    ADD_FAILURE() << toString(violation.code) << ": " << violation.message;
  }
  EXPECT_TRUE(violations.empty());
}

TEST(IrInvariantsTest, AnUnbuiltModuleScansClean) {
  // A default-constructed handle owns no module, so there is nothing to scan
  // and nothing to report -- which is what keeps a caller from having to check
  // `built()` before asking.
  const Module module;
  EXPECT_FALSE(module.built());
  EXPECT_TRUE(scanModule(module).empty());
}

TEST(IrInvariantsTest, EveryDiagnosticCodeHasAName) {
  const std::vector<IRDiagnosticCode>& codes = allDiagnosticCodes();
  EXPECT_FALSE(codes.empty());

  std::set<IRDiagnosticCode> unique;
  for (const IRDiagnosticCode code : codes) {
    const std::string name(toString(code));
    EXPECT_FALSE(name.empty()) << static_cast<int>(code);
    EXPECT_EQ(name.rfind("ir-", 0), 0U) << name;
    unique.insert(code);
  }
  // One row per code: a duplicate row would make the table describe a code
  // twice and the enumeration would be longer than the enum.
  EXPECT_EQ(unique.size(), codes.size());
}

TEST(IrInvariantsTest, TheAssumptionScanIsExposedForAConsumer) {
  // The scan is a function over an artifact and nothing else, so a later stage
  // (the `link` command, a fuzz harness) can ask it the same question without
  // going through the driver.
  test::IrFixture fixture;
  fixture.source("fn i32 main() { return 1; }\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt());
  EXPECT_TRUE(scanModule(fixture.result().module).empty());
}

} // namespace
} // namespace minc::ir
