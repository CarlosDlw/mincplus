// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The target: the triple that names it, and the ABI facts the front end reads
// out of it.
//
// Three properties, and each is a failure mode this file exists to prevent:
//
//   * **The table is data, and it follows the components.** `long` is 32 bits on
//     Windows and the pointer width on a Unix *by rule*, not by a name that
//     happens to be in a list -- so a new triple with a known architecture and OS
//     cannot silently inherit a wrong width.
//   * **One triple, one string.** Parsing a canonical spelling is the identity
//     function, which is what lets `--target` and a dump and an LLVM
//     `TargetMachine` all be talking about the same thing.
//   * **An unknown target is refused, never defaulted.** The old two-name enum is
//     the worked example: `systemv-amd64` no longer names a target, and it is
//     refused with a sentence rather than quietly read as the default, because a
//     target read wrong is how a cross build is silently wrong.
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sema/target.h"

namespace minc::test {
namespace {

using sema::Arch;
using sema::Env;
using sema::OsFamily;
using sema::TargetInfo;
using sema::Triple;

// Every triple this project names, so the round-trip is exhaustive over the
// spellings a caller can reach for.
constexpr std::string_view kStatedTriples[] = {
    sema::kDefaultTriple,      sema::kTripleLinuxAmd64,      sema::kTripleWindowsAmd64,
    sema::kTripleLinuxAarch64, sema::kTripleLinuxRiscv64,    sema::kTripleDarwinAmd64,
    "x86_64-w64-windows-gnu",  "x86_64-unknown-freebsd",     "aarch64-apple-darwin",
    "aarch64-pc-windows-msvc", "aarch64-unknown-linux-musl", "riscv64-unknown-linux-musl",
    "i386-unknown-linux-gnu",  "x86_64-unknown-linux",       "x86_64-unknown-darwin",
};

[[nodiscard]] TargetInfo require(std::string_view name) {
  const std::optional<TargetInfo> info = sema::targetFromName(name);
  EXPECT_TRUE(info.has_value()) << name << ": " << sema::targetRefusal(name);
  return info.value_or(sema::defaultTarget());
}

TEST(TargetTest, TheDefaultIsTheHostAndHasATableRow) {
  // The default target is the host, and **the host is a row of this table**: an
  // empty host means the build could not name a machine, and the fallback (the
  // target the project was developed against) is used instead. Either way the
  // default is a target this file states, which is what `defaultTarget`'s own
  // `value_or` is a guard against rather than an expectation.
  const TargetInfo info = sema::defaultTarget();
  EXPECT_EQ(info.name(), std::string(sema::kDefaultTriple));
  EXPECT_TRUE(sema::targetInfo(info.triple).has_value());
  EXPECT_TRUE(sema::targetRefusal(sema::kDefaultTriple).empty());

  // The host, when the build stated one, is what the default is -- and it is
  // stated in the canonical spelling, so `--target <host>` is accepted.
  if (!sema::hostTriple().empty()) {
    EXPECT_EQ(sema::kDefaultTriple, sema::hostTriple());
    EXPECT_TRUE(sema::targetFromName(sema::hostTriple()).has_value())
        << sema::targetRefusal(sema::hostTriple());
    EXPECT_EQ(sema::parseTriple(sema::hostTriple())->text, sema::hostTriple());
  } else {
    EXPECT_EQ(sema::kDefaultTriple, sema::kFallbackTriple);
  }

  // On this machine the host is Linux/amd64, so the widths are still System V's:
  // the point of the default moving to the host is not that the numbers changed
  // here, it is that they are the *host's* numbers everywhere.
  EXPECT_EQ(info.triple.os, OsFamily::linux);
  EXPECT_EQ(info.triple.arch, Arch::x86_64);
  EXPECT_EQ(info.pointerBits, 64u);
  EXPECT_EQ(info.longBits, 64u);
  EXPECT_EQ(info.longDoubleBits, 80u);
}

TEST(TargetTest, AnArchitectureAliasParsesToTheCanonicalSpelling) {
  // The names LLVM itself recognizes for the architectures here. `arm64` is the
  // one that matters: it is what Apple's toolchain and
  // `getDefaultTargetTriple()` print for every M-series machine, and a compiler
  // that refuses the name of the machine it was built on has confused tidiness
  // with correctness.
  struct Alias {
    std::string_view spelled;
    std::string_view canonical;
  };
  const Alias aliases[] = {
      {"arm64-apple-darwin", "aarch64-apple-darwin"},
      {"arm64-unknown-linux-gnu", "aarch64-unknown-linux-gnu"},
      {"amd64-unknown-linux-gnu", "x86_64-unknown-linux-gnu"},
      {"i686-unknown-linux-gnu", "i386-unknown-linux-gnu"},
      {"i586-pc-windows-msvc", "i386-pc-windows-msvc"},
  };
  for (const Alias& alias : aliases) {
    const std::optional<Triple> triple = sema::parseTriple(alias.spelled);
    ASSERT_TRUE(triple.has_value()) << alias.spelled << ": " << sema::targetRefusal(alias.spelled);
    EXPECT_EQ(triple->text, alias.canonical) << alias.spelled;
    // The ABI is the canonical row's, not a second set of numbers: an alias is
    // input syntax and nothing else.
    EXPECT_TRUE(sema::sameAbi(require(alias.spelled), require(alias.canonical))) << alias.spelled;
  }
}

TEST(TargetTest, SameAbiIgnoresTheVendorAndNothingElse) {
  // `sameAbi` is what "aimed at the machine this process runs on" means for a
  // link, so it must say yes to two spellings of one machine and no to two
  // machines that merely look alike.
  EXPECT_TRUE(sema::sameAbi(require("x86_64-pc-linux-gnu"), require("x86_64-unknown-linux-gnu")));
  EXPECT_FALSE(
      sema::sameAbi(require("x86_64-unknown-linux-gnu"), require("aarch64-unknown-linux-gnu")));
  EXPECT_FALSE(
      sema::sameAbi(require("x86_64-unknown-linux-gnu"), require("x86_64-unknown-freebsd")));
  // The environment is part of the ABI even where it changes no width here today:
  // it is what `codegen` hands to LLVM, and two different environments are two
  // different target machines.
  EXPECT_FALSE(sema::sameAbi(require("x86_64-pc-windows-msvc"), require("x86_64-w64-windows-gnu")));
}

TEST(TargetTest, EveryStatedTripleParsesToItself) {
  for (const std::string_view name : kStatedTriples) {
    const std::optional<Triple> triple = sema::parseTriple(name);
    ASSERT_TRUE(triple.has_value()) << name << ": " << sema::targetRefusal(name);
    // One target, one string: the canonical spelling is a fixed point.
    EXPECT_EQ(triple->text, name);
    EXPECT_TRUE(sema::targetInfo(*triple).has_value()) << name;
    EXPECT_TRUE(sema::targetRefusal(name).empty()) << name;
  }
}

TEST(TargetTest, LongFollowsTheRuleAndNotTheName) {
  // LP64: `long` and a pointer are the same width.
  const TargetInfo linux = require(sema::kTripleLinuxAmd64);
  EXPECT_EQ(linux.longBits, linux.pointerBits);
  EXPECT_EQ(linux.longBits, 64u);
  EXPECT_EQ(linux.longDoubleBits, 80u); // x87

  // LLP64: `long` is 32 even though a pointer is 64. This is the difference the
  // whole target table exists for.
  const TargetInfo msvc = require(sema::kTripleWindowsAmd64);
  EXPECT_EQ(msvc.longBits, 32u);
  EXPECT_EQ(msvc.pointerBits, 64u);
  EXPECT_EQ(msvc.longDoubleBits, 64u); // MSVC's `long double` is a `double`

  // ... and MinGW keeps the x87 format, which is why the environment is part of
  // the identity on Windows and not decoration.
  const TargetInfo mingw = require("x86_64-w64-windows-gnu");
  EXPECT_EQ(mingw.longBits, 32u);
  EXPECT_EQ(mingw.longDoubleBits, 80u);

  // Darwin: LP64, and `long double` is a `double` on both of its architectures.
  const TargetInfo darwin = require(sema::kTripleDarwinAmd64);
  EXPECT_EQ(darwin.longBits, 64u);
  EXPECT_EQ(darwin.longDoubleBits, 64u);

  // AArch64/RISC-V Linux: LP64 with IEEE binary128 `long double`.
  EXPECT_EQ(require(sema::kTripleLinuxAarch64).longDoubleBits, 128u);
  EXPECT_EQ(require(sema::kTripleLinuxRiscv64).longDoubleBits, 128u);

  // A 32-bit target, so the table is not a 64-bit assumption with a name on it.
  const TargetInfo i386 = require("i386-unknown-linux-gnu");
  EXPECT_EQ(i386.pointerBits, 32u);
  EXPECT_EQ(i386.longBits, 32u);
  EXPECT_EQ(i386.longDoubleBits, 80u);
}

TEST(TargetTest, TheVendorIsCarriedAndDoesNotChangeTheAbi) {
  const TargetInfo a = require("x86_64-pc-windows-msvc");
  const TargetInfo b = require("x86_64-unknown-windows-msvc");
  EXPECT_EQ(a.longBits, b.longBits);
  EXPECT_EQ(a.pointerBits, b.pointerBits);
  EXPECT_EQ(a.longDoubleBits, b.longDoubleBits);
  // ... but it stays part of the identity, because it is part of the string a
  // `TargetMachine` is built from.
  EXPECT_EQ(a.name(), "x86_64-pc-windows-msvc");
  EXPECT_EQ(b.name(), "x86_64-unknown-windows-msvc");
}

TEST(TargetTest, AnUnknownTargetIsRefusedWithASentence) {
  // The old enum's names are not targets any more. Refusing them outright is the
  // point: a name that *used to* mean System V AMD64 would otherwise be read as
  // something by a table that no longer has it.
  const std::vector<std::string_view> refused = {
      "systemv-amd64",              // the name that is gone
      "windows-x64",                // ... and its partner
      "",                           // nothing is not a target
      "x86_64",                     // not enough components
      "x86_64-pc",                  // not enough components
      "x86_64-linux-gnu",           // `arch-os-env`: the vendor was left out
      "arm7-apple-darwin",          // a similar-looking name that is not an alias
      "x86_64-pc-windows",          // Windows has no default environment
      "x86_64-apple-darwin-gnu",    // Darwin has exactly one
      "x86_64-pc-windows-nonsense", // no such environment
      "x86_64-unknown-plan9",       // no such OS
      "sparc64-unknown-linux-gnu",  // no such architecture here
      "x86_64--linux-gnu",          // an empty component
      "x86_64-unknown-linux-gnu-x", // too many components
  };
  for (const std::string_view name : refused) {
    EXPECT_FALSE(sema::targetFromName(name).has_value()) << name;
    // A refusal that says nothing is a refusal a user cannot act on.
    EXPECT_FALSE(sema::targetRefusal(name).empty()) << name;
  }
  // The sentences that carry the most information are the ones for the two easy
  // mistakes: the missing vendor, and Windows without an environment.
  EXPECT_NE(sema::targetRefusal("x86_64-linux-gnu").find("vendor"), std::string::npos);
  EXPECT_NE(sema::targetRefusal("x86_64-pc-windows").find("msvc"), std::string::npos);
}

TEST(TargetTest, AnEnvironmentTheOsDoesNotSpellIsAcceptedWhereItChangesNothing) {
  // On a Unix the C library flavour changes no width this stage states, so the
  // OS's own default (a triple with the environment left off) is a target rather
  // than a refusal. On Windows it is a refusal, because `msvc` and `gnu` disagree
  // about `long double` -- see the test above.
  const TargetInfo bare = require("x86_64-unknown-linux");
  const TargetInfo gnu = require(sema::kTripleLinuxAmd64);
  EXPECT_EQ(bare.longBits, gnu.longBits);
  EXPECT_EQ(bare.longDoubleBits, gnu.longDoubleBits);
  EXPECT_EQ(bare.name(), "x86_64-unknown-linux");
}

TEST(TargetTest, ARefusalNeverFallsBackToTheDefault) {
  // The failure this guards is the quiet one: `--target` typo'd, the compiler
  // types the program for the wrong ABI and says nothing.
  EXPECT_FALSE(sema::targetFromName("x86_64-unknown-linu-gnu").has_value());
  EXPECT_FALSE(sema::targetFromName("X86_64-unknown-linux-gnu").has_value());
}

} // namespace
} // namespace minc::test
