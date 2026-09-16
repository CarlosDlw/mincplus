// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Reading a literal's *spelling* into a value, once.
//
// The lexer produces literal tokens and deliberately stops there: it knows a
// token is an `IntegerLiteral`, never what number it is. Two stages then need
// the number -- the preprocessor, to evaluate `#if`, and the type checker, to
// range-check a literal against the type its context gave it -- and a spelling
// that means two different things to two stages is a bug waiting for a header.
//
// The one place the two genuinely differ is the base, so it is an option rather
// than a divergence:
//
//   * inside `.mx` source there is no implicit octal -- `0755` is decimal, and
//     octal is spelled `0o755`, because C's leading-zero rule is a footgun the
//     language rejects elsewhere (README, *Literals*);
//   * inside a `#if` the input is C, including the headers it came from, and
//     `0755` is octal.
//
// Everything else -- which prefixes exist, which suffixes are accepted, when a
// decimal constant becomes unsigned, and the overflow rule -- is shared, so the
// two readers cannot drift apart.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "support/consteval/const_int.h"
#include "support/consteval/suffix.h"

namespace minc::support {

// How a leading zero is read. See the file comment: this is the one rule the two
// callers disagree about, so it is explicit at every call.
enum class IntegerBaseRule : std::uint8_t {
  DecimalLeadingZero, // `.mx` source: `0755` is 755
  ImplicitOctal,      // C: `0755` is 493
};

struct IntegerLiteral {
  ConstInt value;
  // The **number** of the spelling, prefix included and suffix left out, as a
  // view into `text`. The value above is what the 64-bit core could hold; this is
  // what a reader with a wider type needs (`i128`/`u128`, which deliberately keep
  // no `ConstInt`), and it is produced here because this is the reader that knows
  // where the digits end.
  std::string_view number;
  // The spelling is a well-formed integer literal.
  bool ok = false;
  // The spelling is well formed but the value does not fit the core's 64 bits.
  // Kept apart from `ok == false` so a caller with a wider target type (`i128`,
  // `u128`) can accept it and one without can report `message`.
  bool tooWide = false;
  // Set when `ok` is false, and when `tooWide` is set. Always a complete
  // sentence, because it is what a diagnostic prints.
  std::string message;
  // What the literal's spelling says its type is, when it says one: `10u8` is a
  // `u8` and `10` is deferred. The split between the digits and the suffix is
  // this reader's -- it is the only stage that reads the digits -- so the
  // descriptor comes back here instead of being cut off the spelling again by
  // each caller (`casts.md`).
  LiteralSuffix suffix;
};

// A *float* literal's spelling, split the same way: the number, and the suffix
// that decides its type. The number is a view into `text`.
struct FloatLiteral {
  std::string_view number;
  LiteralSuffix suffix;
};

// The split, for a float. The numeric part is read with the scanner's own grammar
// (`numericPartOfFloat`) and what follows is classified: `1.5f32` is `1.5` and an
// `f32`, `1.5` is `1.5` with no suffix, and a spelling the scanner would not have
// produced comes back whole with no suffix -- the reader's own failure is then
// what reports it.
[[nodiscard]] FloatLiteral readFloatLiteral(std::string_view text);

// The value of an integer literal's spelling. An empty spelling is refused
// rather than read as zero.
[[nodiscard]] IntegerLiteral parseIntegerLiteral(std::string_view text, IntegerBaseRule baseRule);

// A character literal's spelling **including its quotes**, as the lexer
// produced it. One or more characters, escapes decoded, packed most-significant
// first -- the value GCC produces for a multi-character constant, which is the
// least surprising rule available.
[[nodiscard]] IntegerLiteral parseCharLiteral(std::string_view text);

// A string literal's spelling **including its quotes**, decoded to bytes.
//
// The trailing NUL is deliberately *not* in `bytes`: whether a `str` has a
// terminator at its end is a fact about the object the lowering builds, not
// about what the source spelled, and a reader that appended one would make
// `"a\0b"` and `"a\0b\0"` indistinguishable.
//
// `\u`/`\U` escapes are encoded as UTF-8, because the one reader and the one
// writer of an escape have to agree about what the bytes are, and the language's
// string is a byte string (README, *Literals*). A `\x` escape that would not fit
// in one byte is refused rather than truncated: a value the reader cannot
// represent is a mistake it can point at.
struct StringLiteral {
  std::vector<std::uint8_t> bytes;
  bool ok = false;
  std::string message;
};

[[nodiscard]] StringLiteral parseStringLiteral(std::string_view text);

// The code unit at `index` and the index past it. A character that is not a
// backslash is itself; a backslash introduces `\n`, `\t`, `\r`, `\a`, `\b`,
// `\f`, `\v`, `\\`, `\'`, `\"`, up to three octal digits, or `\x` with one or
// more hex digits. `nullopt` for an unknown or truncated escape.
[[nodiscard]] std::optional<std::pair<std::uint64_t, std::size_t>>
decodeCharOrEscape(std::string_view body, std::size_t index);

} // namespace minc::support
