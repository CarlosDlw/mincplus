// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <set>
#include <string>
#include <string_view>

#include "ast/ast_error.h"
#include "resolve/resolve_fixture.h"

namespace minc::test {
namespace {

TEST(ValidateTest, BindingWithNoTypeAndNoInitializer) {
  ResolveFixture f;
  f.source("fn i32 main()\n{\n  let x;\n  return 0;\n}\n");
  ASSERT_TRUE(f.build());

  EXPECT_TRUE(f.hasAstError("ast-missing-type"));
  EXPECT_FALSE(f.structural().empty());
  EXPECT_EQ(ast::toString(f.structural().front().code), "ast-missing-type");
}

TEST(ValidateTest, ConstantWithNoInitializer) {
  ResolveFixture f;
  f.source("fn i32 main()\n{\n  const c: i32;\n  return 0;\n}\n");
  ASSERT_TRUE(f.build());

  // A `const` that can never be assigned and was never given a value: legal
  // syntax, impossible meaning. The parser sees two independent optional
  // clauses and cannot tell.
  EXPECT_TRUE(f.hasAstError("ast-const-without-value"));
  EXPECT_FALSE(f.hasAstError("ast-missing-type"));
}

TEST(ValidateTest, AWellFormedBindingIsSilent) {
  ResolveFixture f;
  f.source("fn i32 main()\n{\n  let a: i32;\n  let b = 1;\n  const c = 2;\n  const d: i32 = 3;\n"
           "  return b;\n}\n");
  ASSERT_TRUE(f.build());

  EXPECT_TRUE(f.structural().empty());
  EXPECT_FALSE(f.hasAstError("ast-missing-type"));
  EXPECT_FALSE(f.hasAstError("ast-const-without-value"));
}

TEST(ValidateTest, ARegionTheParserReportedIsNotReportedAgain) {
  ResolveFixture f;
  // The initializer is missing, so the parser reports it and wraps the gap in an
  // `Error` node. Validation must not add a second diagnostic for it.
  f.source("fn i32 main()\n{\n  let x = ;\n  return 0;\n}\n");
  ASSERT_TRUE(f.build());

  EXPECT_FALSE(f.parseErrors().empty());
  EXPECT_FALSE(f.hasAstError("ast-missing-type"));
}

TEST(ValidateTest, EveryCodeIsReachable) {
  std::set<std::string> reached;

  {
    ResolveFixture f;
    f.source("fn i32 main()\n{\n  let x;\n  return 0;\n}\n");
    ASSERT_TRUE(f.build());
    for (const std::string& code : f.astErrorCodes()) {
      reached.insert(code);
    }
  }
  {
    ResolveFixture f;
    f.source("fn i32 main()\n{\n  const c: i32;\n  return 0;\n}\n");
    ASSERT_TRUE(f.build());
    for (const std::string& code : f.astErrorCodes()) {
      reached.insert(code);
    }
  }
  {
    ResolveFixture f;
    f.source("fn i32 main()\n{\n  let a = 1;\n  return a;\n}\n");
    ast::LowerLimits limits;
    limits.maxNodes = 3;
    f.lowerLimits(limits);
    ASSERT_TRUE(f.build());
    for (const std::string& code : f.astErrorCodes()) {
      reached.insert(code);
    }
  }

  // The table is the definition of the closed set, so a code added without an
  // input that produces it fails here rather than being discovered later.
  for (const ast::AstErrorCode code : ast::allAstErrorCodes()) {
    EXPECT_TRUE(reached.count(std::string(ast::toString(code))) != 0)
        << "no input produces " << ast::toString(code);
  }
  EXPECT_EQ(reached.size(), ast::allAstErrorCodes().size());
}

} // namespace
} // namespace minc::test
