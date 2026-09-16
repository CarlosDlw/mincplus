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

  // And what the default *answers* is what naming the same triple answers: it is
  // a row of the table, not a synthesis of one. The widths themselves are
  // checked by the per-target tests below, where the target is named.
  //
  // Asserting them here would assert the machine this test happens to run on --
  // 80-bit `long double` is System V's x86 answer, and `OsFamily::linux` is where
  // this file was written, not what the language promises. The first CI run on
  // macOS arm64 is what made that visible: this test failed there, reading
  // `LongDouble` on a target that has no x87.
  const std::optional<Triple> spelled = sema::parseTriple(sema::kDefaultTriple);
  ASSERT_TRUE(spelled.has_value()) << sema::targetRefusal(sema::kDefaultTriple);
  const std::optional<TargetInfo> stated = sema::targetInfo(*spelled);
  ASSERT_TRUE(stated.has_value());
  EXPECT_EQ(info.name(), stated->name());
  EXPECT_EQ(info.pointerBits, stated->pointerBits);
  EXPECT_EQ(info.longBits, stated->longBits);
  EXPECT_EQ(info.longDoubleBits, stated->longDoubleBits);
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

  // Darwin: LP64, and `long double` follows the *architecture* there rather than
  // the OS -- which is the one place in this table where the OS is not enough.
  // `x86_64-apple-darwin` runs on x87 (`sizeof(long double) == 16`, and LLVM's
  // layout for that triple carries `f80:128`); Apple silicon has no x87, so it is
  // a `double` (LLVM's aarch64 layout has no `f80` at all). Reading the first as
  // 64 refused `f80` on a target whose ABI has it, and read every 80-bit value in
  // a program compiled for it as an 8-byte `double`.
  const TargetInfo darwin = require(sema::kTripleDarwinAmd64);
  EXPECT_EQ(darwin.longBits, 64u);
  EXPECT_EQ(darwin.longDoubleBits, 80u);
  EXPECT_EQ(require(sema::kTripleDarwinAarch64).longDoubleBits, 64u);

  // AArch64/RISC-V Linux: LP64 with IEEE binary128 `long double`.
  EXPECT_EQ(require(sema::kTripleLinuxAarch64).longDoubleBits, 128u);
  EXPECT_EQ(require(sema::kTripleLinuxRiscv64).longDoubleBits, 128u);

  // A 32-bit target, so the table is not a 64-bit assumption with a name on it.
  const TargetInfo i386 = require("i386-unknown-linux-gnu");
  EXPECT_EQ(i386.pointerBits, 32u);
  EXPECT_EQ(i386.longBits, 32u);
  EXPECT_EQ(i386.longDoubleBits, 80u);
}

TEST(TargetTest, TheAlignmentOfAScalarIsTheTargetsAndNotItsWidth) {
  // The second half of "how big is a `T`", and the one place the width is not the
  // answer: i386's data layout is `...-i64:32:64-...-f64:32:64-f80:32`, so a
  // 64-bit value is aligned to four bytes and the x87 format sits in a four-byte
  // slot (twelve bytes as an object, not sixteen). `TypeStore::alignOf` reads
  // these, every `align N` in a module comes from there, and `src/ir` compares the
  // two against LLVM's own `DataLayout`.
  const TargetInfo i386 = require(sema::kTripleLinuxI386);
  EXPECT_EQ(i386.int64AlignBits, 32u);
  EXPECT_EQ(i386.float64AlignBits, 32u);
  EXPECT_EQ(i386.float80AlignBits, 32u);

  // Every other target this compiler names aligns a scalar to its own width.
  for (const std::string_view name : kStatedTriples) {
    const TargetInfo info = require(name);
    if (info.triple.arch == sema::Arch::i386) {
      continue;
    }
    EXPECT_EQ(info.int64AlignBits, 64u) << name;
    EXPECT_EQ(info.float64AlignBits, 64u) << name;
    EXPECT_EQ(info.float80AlignBits, 128u) << name;
  }
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
