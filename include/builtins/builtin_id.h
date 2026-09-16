// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The identity of every builtin, so that no stage ever matches one by spelling.
//
// An enum and not a string, and Go is the reason: its intrinsics are keyed by
// `map[string]`, so a renamed or mistyped key is an intrinsic that silently
// stops being one, and nothing in the compiler can notice. Here the key *is* the
// identity -- `sema` reads `Def::builtin` and switches on it, `ir` reads the
// same field -- and the spellings live in exactly one place, the table.
//
// `None` is the "not a builtin" answer, so `Def::builtin` needs no second boolean:
// the same trick `resolve::Predefined::None` uses, for the same reason, spelled
// the same way.
//
// The order is the order the table lists them -- the safe, plain-named bit
// operations first, then the reserved one -- and not alphabetical: the table is
// the definition, and a reader should find them the way the record does.
//
// The base type is the **smallest that holds the table**, and `kBuiltinIdLimit`
// is the assert that says so. Deliberately not a wider type chosen for headroom:
// an enum whose base is one byte with an assert two rows from its limit is a
// compile error the day the table outgrows it -- one line to widen, right here --
// while a type chosen for the future is a fact about nothing. The `limit` is the
// count the *last* enumerator may take, so the assert reads `count + 1 <= limit`:
// `None` plus every row.
#pragma once

#include <cstddef>
#include <cstdint>

namespace minc::builtins {

enum class BuiltinId : std::uint8_t {
  None = 0,

  // The bit operations. Plain names, in the file scope, shadowable like `true`:
  // the language defines all of them completely (`clz(0)` is the width, a count
  // is taken modulo the width), so there is no input whose answer is undefined
  // and nothing for a `__builtin_` prefix to warn about.
  Clz,
  Ctz,
  Popcount,
  Bswap,
  Rotl,
  Rotr,

  // The reserved ones: the raw layer the runtime is written in, and the reason
  // the prefix exists. Not promised, not shadowable, and a program that reaches
  // for it has left the language's own surface on purpose.
  Trap,
};

// How many ids are *not* `None`. The table asserts its own size against this
// number, so adding an enumerator without a row is a compile error rather than a
// builtin that exists and cannot be reached -- which is the whole point of
// keeping the identity in one file and the data in another.
inline constexpr std::size_t kBuiltinIdCount = 7;

// The last value `BuiltinId` can take, which is what the table's `static_assert`
// checks against: `kBuiltinIdCount + 1 <= kBuiltinIdLimit`. Trivial at seven rows
// and stated now, so that the day the table approaches it is a compile error and
// not a silent truncation -- and so that the base type is a *checked* choice
// rather than a guess about how many operations the language will ever have.
inline constexpr std::size_t kBuiltinIdLimit = 0xFFu;

} // namespace minc::builtins
