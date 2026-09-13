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

#include "support/consteval/const_int.h"

namespace minc::support {

// How a leading zero is read. See the file comment: this is the one rule the two
// callers disagree about, so it is explicit at every call.
enum class IntegerBaseRule : std::uint8_t {
  DecimalLeadingZero, // `.mx` source: `0755` is 755
  ImplicitOctal,      // C: `0755` is 493
};

struct IntegerLiteral {
  ConstInt value;
  // The spelling is a well-formed integer literal.
  bool ok = false;
  // The spelling is well formed but the value does not fit the core's 64 bits.
  // Kept apart from `ok == false` so a caller with a wider target type (`i128`,
  // `u128`) can accept it and one without can report `message`.
  bool tooWide = false;
  // Set when `ok` is false, and when `tooWide` is set. Always a complete
  // sentence, because it is what a diagnostic prints.
  std::string message;
};

// The value of an integer literal's spelling. An empty spelling is refused
// rather than read as zero.
[[nodiscard]] IntegerLiteral parseIntegerLiteral(std::string_view text, IntegerBaseRule baseRule);

// A character literal's spelling **including its quotes**, as the lexer
// produced it. One or more characters, escapes decoded, packed most-significant
// first -- the value GCC produces for a multi-character constant, which is the
// least surprising rule available.
[[nodiscard]] IntegerLiteral parseCharLiteral(std::string_view text);

// The code unit at `index` and the index past it. A character that is not a
// backslash is itself; a backslash introduces `\n`, `\t`, `\r`, `\a`, `\b`,
// `\f`, `\v`, `\\`, `\'`, `\"`, up to three octal digits, or `\x` with one or
// more hex digits. `nullopt` for an unknown or truncated escape.
[[nodiscard]] std::optional<std::pair<std::uint64_t, std::size_t>>
decodeCharOrEscape(std::string_view body, std::size_t index);

} // namespace minc::support
