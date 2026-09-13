// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// What the target says a C spelling means.
//
// The width of `long`, of `long double`, and of `isize`/`usize` is a property of
// the **target**, not of the compiler and not of the machine running it. That
// distinction is the whole point: a `#if defined(_WIN32)` here would make the
// compiler's own host decide the meaning of the user's program, so cross-
// compiling would be silently wrong -- and silently wrong is the failure mode
// this project is built to avoid. One table, selected by name, consulted by the
// type-specifier reader.
//
// The default is System V AMD64: it is the interop target `README.md` and the
// roadmap already commit to (LP64, x87 `long double`), and it is the same on
// every host, so a Linux and a Windows checkout of the compiler agree on what
// `long int` means before `--target` exists.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace minc::sema {

enum class Target : std::uint8_t {
  SystemVAmd64,
  WindowsX64,
};

[[nodiscard]] std::string_view toString(Target target);

struct TargetInfo {
  Target target = Target::SystemVAmd64;
  // Pointer width -- and therefore `isize`/`usize`.
  std::uint16_t pointerBits = 64;
  // `long`, and the length half of `long long`. 64 on LP64, 32 on LLP64.
  std::uint16_t longBits = 64;
  // `long double`: the x87 80-bit format on System V, `double` under MSVC.
  std::uint16_t longDoubleBits = 80;
  // `short`, and the width half of `short int`. 16 everywhere the ABI is used,
  // but a field rather than a constant for the same reason as the rest.
  std::uint16_t shortBits = 16;
  // `int`, and the width half of `int`. 32.
  std::uint16_t intBits = 32;
  // `char`, `signed char`, `unsigned char`. 8.
  std::uint16_t charBits = 8;
};

// The table. An unknown enumerator is a programming error, not user input, so
// the default branch is the System V row rather than a zeroed struct -- a
// zero-width `int` would be a much worse failure than a wrong-but-sane one.
[[nodiscard]] TargetInfo targetInfo(Target target);

// The name back to the enumerator, for `--target`. Here rather than in the CLI
// so that a name can only ever mean the row `toString` prints for it: two lists
// of target names would be two chances to disagree, and a target read wrong is
// how a cross build is silently wrong.
[[nodiscard]] std::optional<Target> targetFromName(std::string_view name);

inline constexpr Target kDefaultTarget = Target::SystemVAmd64;

} // namespace minc::sema
