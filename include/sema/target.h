// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// What the target says a C spelling means, and the triple that names it.
//
// The width of `long`, of `long double`, and of `isize`/`usize` is a property of
// the **target**, not of the compiler and not of the machine running it. That
// distinction is the whole point: a `#if defined(_WIN32)` here would make the
// compiler's own host decide the meaning of the user's program, so cross-
// compiling would be silently wrong -- and silently wrong is the failure mode
// this project is built to avoid. One table, consulted by the type-specifier
// reader.
//
// ### The identity is a triple, and it is LLVM's
//
// The target used to be a two-name enum (`SystemVAmd64`, `WindowsX64`), which is
// a table of two rows wearing the clothes of a type: it cannot name aarch64, it
// cannot name a 32-bit target, and it cannot be handed to `codegen`, which needs
// a triple to select a `TargetMachine`. It is now the **canonical LLVM triple**
// (`x86_64-unknown-linux-gnu`), because that is the string `src/ir` and
// `src/backend` have to give LLVM anyway, and a second spelling of the same
// target is a second chance for the two to disagree.
//
// This stage does not link LLVM, so it parses the triple itself -- the grammar
// is textual (`arch-vendor-os[-env]`) and four components is the whole of it.
// What it does *not* do is guess: a component this compiler does not state, a
// combination whose ABI differs with the environment, and a malformed triple are
// all **refused**, and a refusal is a diagnostic the caller prints. The
// alternative -- defaulting an unknown triple to something sane -- is exactly how
// a cross build ends up silently wrong, which is the failure this file exists to
// prevent.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace minc::sema {

// The components of a triple, as enumerators.
//
// The names **are** the triple's spelling (`x86_64`, not `X8664` or `Amd64`):
// this file's whole argument is that one target has one name, and an enumerator
// that spells an architecture differently from the string a user writes would be
// a second spelling to keep in step. The project's identifier rule asks for
// CamelCase and is waived for exactly that reason.
//
// A row per *component*, not per triple: what varies with the OS and the
// environment is a property of those components (`targetInfo`), so a new triple
// built from known parts inherits the right ABI by rule rather than by a row
// somebody remembered to add.
// NOLINTBEGIN(readability-identifier-naming)
enum class Arch : std::uint8_t {
  x86_64,
  aarch64,
  riscv64,
  i386,
};

enum class OsFamily : std::uint8_t {
  linux,
  windows,
  darwin,
  freebsd,
};

// The environment, which is not decoration: `msvc` and `gnu` are different ABIs
// on Windows (`long double` is 64 bits under one and 80 under the other), so
// Windows has no default and demands one be named.
enum class Env : std::uint8_t {
  none,
  gnu,
  musl,
  msvc,
};
// NOLINTEND(readability-identifier-naming)

[[nodiscard]] std::string_view toString(Arch arch);
[[nodiscard]] std::string_view toString(OsFamily os);
[[nodiscard]] std::string_view toString(Env env);

// A parsed triple. The vendor is carried verbatim: it is part of the identity and
// part of what `codegen` hands to LLVM, and no rule in this stage reads it.
struct Triple {
  Arch arch = Arch::x86_64;
  OsFamily os = OsFamily::linux;
  Env env = Env::none;
  std::string vendor = "unknown";
  // The canonical spelling, always the same string for the same triple, so a
  // dump and an error message are stable and a test can round-trip it.
  std::string text;
};

// `arch-vendor-os` or `arch-vendor-os-env`, and nothing else. The components are
// in that order -- a three-component triple is *not* read as `arch-os-env`,
// because which of the two a name means is a guess, and `x86_64-linux-gnu` is
// then refused with "the OS component is not ..." rather than silently read as
// an OS called `gnu`.
//
// `std::nullopt` for a malformed name, an architecture or OS this compiler does
// not state, or an environment the OS cannot have.
[[nodiscard]] std::optional<Triple> parseTriple(std::string_view name);

struct TargetInfo {
  Triple triple;
  // Pointer width -- and therefore `isize`/`usize`.
  std::uint16_t pointerBits = 64;
  // `long`, and the length half of `long long`. LP64/ILP32 on the Unices (equal
  // to the pointer width), 32 on Windows (LLP64 on 64-bit, ILP32 on 32-bit).
  std::uint16_t longBits = 64;
  // `long double`: the x87 80-bit format on System V, `double` on Darwin and
  // under MSVC, IEEE binary128 on AArch64 and RISC-V Linux.
  std::uint16_t longDoubleBits = 80;
  // `short`, and the width half of `short int`. 16 everywhere the ABI is used,
  // but a field rather than a constant for the same reason as the rest.
  std::uint16_t shortBits = 16;
  // `int`, and the width half of `int`. 32.
  std::uint16_t intBits = 32;
  // `char`, `signed char`, `unsigned char`. 8.
  std::uint16_t charBits = 8;

  // What `codegen` hands to LLVM, and what a diagnostic prints for the target.
  [[nodiscard]] const std::string& name() const {
    return triple.text;
  }
};

// The ABI facts for a parsed triple, or nothing when this compiler has no row
// for the combination. A *row*, not a fallback: `aarch64-unknown-linux-gnu` and
// `x86_64-pc-windows-msvc` differ in two of the six facts, and a target nobody
// stated is a target nobody can be right about.
[[nodiscard]] std::optional<TargetInfo> targetInfo(const Triple& triple);

// The one entry point for user input: a triple string in, the ABI out. Here
// rather than in the CLI so that a name can only ever mean the row it parses to:
// two lists of target names would be two chances to disagree, and a target read
// wrong is how a cross build is silently wrong.
[[nodiscard]] std::optional<TargetInfo> targetFromName(std::string_view name);

// Why a name was refused, as a sentence for a diagnostic; empty when it parses.
// Written by the same parser that answered `targetFromName`, so the sentence a
// user reads cannot describe a parse that did not happen.
[[nodiscard]] std::string targetRefusal(std::string_view name);

// The triples this project names. A caller that needs one by name uses these;
// user input goes through `targetFromName`, which can refuse. They are the
// spelling in one place, and the table is still what answers.
inline constexpr std::string_view kDefaultTriple = "x86_64-unknown-linux-gnu";
inline constexpr std::string_view kTripleLinuxAmd64 = "x86_64-unknown-linux-gnu";
inline constexpr std::string_view kTripleWindowsAmd64 = "x86_64-pc-windows-msvc";
inline constexpr std::string_view kTripleLinuxAarch64 = "aarch64-unknown-linux-gnu";
inline constexpr std::string_view kTripleLinuxRiscv64 = "riscv64-unknown-linux-gnu";
inline constexpr std::string_view kTripleDarwinAmd64 = "x86_64-apple-darwin";

// The default: System V AMD64. It is the interop target `README.md` and the
// roadmap already commit to (LP64, x87 `long double`), and it is the same on
// every host, so a Linux and a Windows checkout of the compiler agree on what
// `long int` means before `--target` is written.
[[nodiscard]] TargetInfo defaultTarget();

} // namespace minc::sema
