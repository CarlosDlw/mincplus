// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The suffix of a numeric literal: one table, three readers.
//
// `10u8` is **one token**: the suffix is part of the literal's spelling, and the
// only way three stages can agree about what it means is if they ask the same
// table. They are
//
//   * the **scanner**, which has to decide how many bytes the token is (`10u8`
//     is one token, `10z` is two) -- and it decides it with `suffixLengthAt`,
//     which claims a trailing identifier run *only* when the run is a spelling
//     this file knows;
//   * the **reader** (`literal.h`), which has to split the spelling into the
//     number and the suffix before it can read the number;
//   * the **checker**, which has to turn the suffix into a type -- and which is
//     the only one of the three that owns a target, so a suffix whose type the
//     target decides (`10L`, `1.5L`) is answered here as a *descriptor* and
//     resolved there (`casts.md`, *Cross-platform*).
//
// The table is closed on purpose. An unknown run after a number is **not** part
// of the token: swallowing any identifier into the literal is how `1else` stops
// being `1` followed by `else`, and the existing `NumberStopsBeforeAnIdentifier`
// invariant is the record of that decision. The parser names the mistake
// (`parse-invalid-literal-suffix`) because it is the stage with the spans.
//
// Two suffixes are *known and refused*: C23's `wb`/`uwb` name a `_BitInt`, and
// this language has no bit-precise type, so the run is claimed and the sentence
// says to write `i64`. Refusing a spelling *by name* is the difference between a
// reader learning what to type and one being told "invalid".
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace minc::support {

// The type a suffix names, before the target's answers.
//
// The language's own widths are fixed (`i8` is eight bits on every target); the
// C spellings whose width is a property of the ABI are named as *what they are*
// (`CUnsignedInt`, not `U32`) and resolved by the checker against
// `sema::TargetInfo`. A `long long` is not in that group: it is 64 bits on every
// triple this compiler states, so it is `U64`/`I64` and not a question.
// NOLINTBEGIN(readability-identifier-naming): the names are the language's.
enum class SuffixType : std::uint8_t {
  None,
  I8,
  I16,
  I32,
  I64,
  I128,
  Isize,
  U8,
  U16,
  U32,
  U64,
  U128,
  Usize,
  F32,
  F64,
  F80,
  F128,
  // `10u`: `unsigned int`, whose width comes from the target's `intBits` (32 on
  // every triple named, and read rather than assumed).
  CUnsignedInt,
  // `10L`: the target's `long` -- 64 on the Unices and Darwin, 32 on Windows.
  CLong,
  // `10UL`: the target's `unsigned long`.
  CUnsignedLong,
  // `1.5L`: the target's `long double` -- x87 `f80` on System V, `f64` under
  // MSVC and on `aarch64-apple-darwin`, IEEE `f128` on aarch64/riscv64 Linux.
  CLongDouble,
};
// NOLINTEND(readability-identifier-naming)

// The stable spelling (`u8`, `c-unsigned-int`), for a test that walks the table
// and for a diagnostic that has to name the type a suffix asks for.
[[nodiscard]] std::string_view toString(SuffixType type);

// True for the suffixes that name an unsigned type. The readers of a spelling
// need it -- `ConstInt` carries a signedness and the digits' own reading depends
// on it -- and `CUnsignedLongDouble` does not exist, which is why a float is
// answered `false` rather than being an error.
[[nodiscard]] constexpr bool isUnsignedSuffix(SuffixType type) {
  switch (type) {
  case SuffixType::U8:
  case SuffixType::U16:
  case SuffixType::U32:
  case SuffixType::U64:
  case SuffixType::U128:
  case SuffixType::Usize:
  case SuffixType::CUnsignedInt:
  case SuffixType::CUnsignedLong:
    return true;
  default:
    return false;
  }
}

// True when the *target* decides this suffix's type. The checker asks it so the
// resolution of a suffix happens in one place instead of at each arm.
[[nodiscard]] constexpr bool isTargetDependent(SuffixType type) {
  switch (type) {
  case SuffixType::CUnsignedInt:
  case SuffixType::CLong:
  case SuffixType::CUnsignedLong:
  case SuffixType::CLongDouble:
    return true;
  default:
    return false;
  }
}

enum class SuffixStatus : std::uint8_t {
  // Not a suffix this language knows: the number ends before the run, and the
  // run is the next token.
  None,
  // The run names a type, and it is a type a literal may take.
  Typed,
  // The run is a spelling the language knows and refuses. `message` is the whole
  // sentence, because it is what a diagnostic prints.
  Refused,
};

struct LiteralSuffix {
  SuffixStatus status = SuffixStatus::None;
  SuffixType type = SuffixType::None;
  // The suffix is a *float* suffix written on an integer-spelled literal:
  // `12f` is `12.0` as an `f32`, and the value is exact. This is Rust's
  // behaviour rather than C's (`casts.md`, decision 10 and the table).
  bool makesFloat = false;
  std::string message;

  [[nodiscard]] bool present() const {
    return status != SuffixStatus::None;
  }
  [[nodiscard]] bool typed() const {
    return status == SuffixStatus::Typed;
  }
  [[nodiscard]] bool refused() const {
    return status == SuffixStatus::Refused;
  }
};

// Classify one suffix spelling. `literalIsFloat` is the *spelling's own* class --
// a dot or an exponent makes it a float -- and it decides two rows: `l`/`L` are
// `long` on an integer and `long double` on a float, and an integer suffix on a
// float is refused rather than read.
[[nodiscard]] LiteralSuffix classifySuffix(std::string_view text, bool literalIsFloat);

// The length of a suffix this language knows, starting at `at`, or `0` when what
// is there is not one. The run is the maximal identifier-continue run, and it
// counts only when the *whole* run classifies: `10u8` claims `u8`, and `10u8x`
// claims nothing, which is what keeps the rule "a number ends at the first byte
// that cannot continue it" true.
[[nodiscard]] std::size_t suffixLengthAt(std::string_view text, std::size_t at,
                                         bool literalIsFloat);

// The numeric part of a *float* literal's spelling, with the suffix removed.
//
// A view into `text`, and the reader for a float has to use it rather than the
// whole spelling: `APFloat`'s reader is handed a number, and a trailing `f32` is
// not one. The scan is the scanner's own numeric grammar (prefix, digits,
// fraction, exponent), and it is here rather than in `src/lex` because the
// reader is what needs it; a spelling the scanner would not have produced comes
// back whole, and the reader's own failure is then what reports it.
[[nodiscard]] std::string_view numericPartOfFloat(std::string_view text);

// Every suffix spelling the table accepts, in the table's order, as
// `{spelling, kind}`. The test that walks it is what keeps "a suffix added to
// the language is a row here and nothing else" true.
struct SuffixSpelling {
  std::string_view spelling;
  bool floatLiteral;
};

[[nodiscard]] std::span<const SuffixSpelling> allSuffixSpellings();

} // namespace minc::support
