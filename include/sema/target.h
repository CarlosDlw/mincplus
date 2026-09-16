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

// The triple this compiler was built for, written by CMake at configure time
// (`cmake/minc_host.cmake`). Empty when the build is for a machine this compiler
// has no ABI row for; `kDefaultTriple` below is what happens then.
#include "sema/host.h"

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
//
// **Architecture aliases are accepted and canonicalized.** The names LLVM itself
// recognizes for the architectures this file states (`amd64` for `x86_64`,
// `arm64` for `aarch64`, `i486`/`i586`/`i686` for `i386`) parse to the same row,
// and the triple they produce prints the canonical spelling. Refusing them was
// the earlier rule, on the grounds that one target should have one name -- and
// the rule is kept where it matters (the *identity* has one spelling, the one a
// dump and a `codegen` target and an error message all use) while dropping it
// where it was only hostile: `arm64-apple-darwin` is what Apple's own toolchain
// and `llvm::sys::getDefaultTargetTriple()` print for every M-series machine, and
// a compiler that refuses to be aimed at the machine it was just built on has
// confused tidiness with correctness.
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

  // --- the alignment of a scalar ----------------------------------------------
  //
  // Alignment is the other half of "how big is a `T`", and it is the half that is
  // *not* a function of the width. The widths above are the same number as their
  // own size on every target this compiler names except one, and i386 is the
  // exception: its data layout is `e-m:e-p:32:32-...-i128:128-f64:32:64-f80:32`,
  // which says a 64-bit value is aligned to **four** bytes (`i64` has no entry
  // there, which is LLVM's default `i64:32:64`) and that the x87 80-bit format
  // lives in a four-byte slot -- ten bytes of value rounded up to four is
  // twelve, not the sixteen System V gives it.
  //
  // These are numbers in the row rather than a rule in a function because they
  // are exactly the facts that differ per target, and the rule a function would
  // state ("aligned to its own width") is the one i386 breaks. `TypeStore::sizeOf`
  // and `alignOf` read them, every emitted `align N` comes from there, and
  // `src/ir` checks the two against the target's *own* `DataLayout` the first time
  // each type enters a module -- so this table and LLVM answer the same question
  // with the same number by construction, and a row that drifts is refused rather
  // than emitted.
  std::uint16_t int64AlignBits = 64;
  std::uint16_t float64AlignBits = 64;
  // The `f80` slot: 128 everywhere the x87 format has an ABI row of its own
  // (`f80:128`), 32 on i386 (`f80:32`).
  std::uint16_t float80AlignBits = 128;

  // Whether the target has the **x87 80-bit format**, which is what the `f80`
  // spelling names.
  //
  // An *architecture* fact and not an ABI one, which is why it is not a stored
  // field and not a comparison of `longDoubleBits`: the format is x86's arithmetic
  // (`st(0)` and LLVM's `x86_fp80`), so x86_64 and i386 have it and AArch64 and
  // RISC-V do not -- while `longDoubleBits` answers a different question, what the
  // OS and the environment make of the *spelling* `long double` (a plain `double`
  // under MSVC on a machine that still has x87, IEEE binary128 on AArch64 Linux).
  // Reading the two as one question refused `f80` on `x86_64-pc-windows-msvc`,
  // where LLVM's own layout carries `f80:128` and the type is perfectly
  // representable.
  //
  // Three readers ask it: the type-specifier reader, which is where a target with
  // no such format refuses the *word* (the checker's acceptance is a promise that
  // the program compiles -- `mincc check --target aarch64-unknown-linux-gnu` used
  // to accept `let x: f80` and the lowering refused it); the lowering, which maps
  // the format to `x86_fp80` and would otherwise have to guess a size and an
  // alignment; and the layout suite, which checks both against LLVM's data layout
  // for the triple. The switch is total, so a new architecture is a build error
  // here rather than a silent `false`.
  [[nodiscard]] bool hasFloat80() const {
    switch (triple.arch) {
    case Arch::x86_64:
    case Arch::i386:
      return true;
    case Arch::aarch64:
    case Arch::riscv64:
      return false;
    }
    // Not reachable while every enumerator is above; it is here because a
    // `switch` is not an expression and the alternative is a warning.
    return false;
  }

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
//
// `kFallbackTriple` exists for the case where the build could not state a host:
// it is the target the project was developed against (LP64, x87 `long double`),
// and it is deliberately *not* the first choice any more.
inline constexpr std::string_view kFallbackTriple = "x86_64-unknown-linux-gnu";
inline constexpr std::string_view kTripleLinuxAmd64 = "x86_64-unknown-linux-gnu";
inline constexpr std::string_view kTripleWindowsAmd64 = "x86_64-pc-windows-msvc";
inline constexpr std::string_view kTripleLinuxI386 = "i686-unknown-linux-gnu";
inline constexpr std::string_view kTripleLinuxAarch64 = "aarch64-unknown-linux-gnu";
inline constexpr std::string_view kTripleLinuxRiscv64 = "riscv64-unknown-linux-gnu";
inline constexpr std::string_view kTripleDarwinAmd64 = "x86_64-apple-darwin";
inline constexpr std::string_view kTripleDarwinAarch64 = "aarch64-apple-darwin";

// **The default target is the host.** A compiler whose default target was a
// constant would refuse to link on every machine that constant does not name --
// `mincc build hello.mx` on macOS or Windows would ask for `--linker` and
// `--sysroot` to build for the machine it is running on -- and would read `long`
// as 32 bits on a host where it is 64. Every production compiler defaults to the
// host for that reason, and `--target` is how a cross build is spelled.
//
// The consequence is worth stating out loud: `mincc check` is *host-dependent*
// for the same input, exactly as `gcc` and `clang` are. `--target` makes it
// host-independent, and `-vV` prints both the host and the default so a bug
// report says which one produced the output.
//
// A constant, and not only a function, because the option table names it: the
// help must show the value a user gets, and a `constexpr` table cannot be filled
// from a function. The *widths* are still the table's answer -- this is a name.
inline constexpr std::string_view kDefaultTriple =
    kHostTriple.empty() ? kFallbackTriple : kHostTriple;

// The default target: the host when the build stated one, `kFallbackTriple`
// otherwise. The ABI facts come from `targetInfo`, so nothing here states a
// width.
[[nodiscard]] TargetInfo defaultTarget();

// The host this compiler was built for, in the canonical spelling, or the empty
// string when the build could not state one. Used by `-vV`, and by the driver to
// tell a native link from a cross one.
[[nodiscard]] std::string_view hostTriple();

// True when two targets have the same ABI -- same architecture, OS and
// environment. This is what "aimed at the machine it is running on" means for a
// link, and it is *not* a comparison of the triple text: the vendor component is
// part of the target's identity (it is what `codegen` hands to LLVM) but not part
// of the ABI, so `x86_64-pc-linux-gnu` and `x86_64-unknown-linux-gnu` are the same
// machine and a native build must not be refused because a user spelled the
// vendor differently from the way this build happens to.
[[nodiscard]] bool sameAbi(const TargetInfo& left, const TargetInfo& right);

} // namespace minc::sema
