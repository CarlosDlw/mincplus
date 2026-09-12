// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include <string>

#include <gtest/gtest.h>

#include "support/expected/expected.h"
#include "support/expected/fallible.h"

namespace minc::support {
namespace {

TEST(ExpectedTest, ValuePath) {
  Expected<int, std::string> ok(42);
  EXPECT_TRUE(ok.hasValue());
  EXPECT_FALSE(ok.hasError());
  EXPECT_TRUE(static_cast<bool>(ok));
  EXPECT_EQ(ok.value(), 42);
  EXPECT_EQ(*ok, 42);
  EXPECT_EQ(ok.valueOr(7), 42);
}

TEST(ExpectedTest, ErrorPath) {
  Expected<int, std::string> bad(makeUnexpected<std::string>("nope"));
  EXPECT_FALSE(bad.hasValue());
  EXPECT_TRUE(bad.hasError());
  EXPECT_FALSE(static_cast<bool>(bad));
  EXPECT_EQ(bad.error(), "nope");
  EXPECT_EQ(bad.valueOr(7), 7);
}

TEST(ExpectedTest, MoveOutValueAndError) {
  Expected<std::string, std::string> ok(std::string("payload"));
  std::string taken = std::move(ok).value();
  EXPECT_EQ(taken, "payload");

  Expected<std::string, std::string> bad(makeUnexpected<std::string>("boom"));
  std::string reason = std::move(bad).error();
  EXPECT_EQ(reason, "boom");
}

TEST(ExpectedTest, VoidSpecialization) {
  Expected<void, std::string> ok;
  EXPECT_TRUE(ok.hasValue());
  EXPECT_FALSE(ok.hasError());

  Expected<void, std::string> bad(makeUnexpected<std::string>("fail"));
  EXPECT_FALSE(bad.hasValue());
  EXPECT_TRUE(bad.hasError());
  EXPECT_EQ(bad.error(), "fail");
}

Fallible<int> parseSmall(int x) {
  if (x < 0) {
    return makeUnexpected<std::string>("negative");
  }
  return x * 2;
}

TEST(ExpectedTest, FallibleAlias) {
  auto ok = parseSmall(21);
  ASSERT_TRUE(ok);
  EXPECT_EQ(ok.value(), 42);
  auto bad = parseSmall(-1);
  EXPECT_FALSE(bad);
  EXPECT_TRUE(bad.hasError());
}

} // namespace
} // namespace minc::support
