// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The one constant integer, shared by every stage that evaluates a constant.
//
// `#if` arithmetic and constant folding are the same arithmetic over different
// inputs -- the preprocessor's are tokens, the type checker's are a typed tree --
// and two implementations of "what does `1 / 0` do" are two implementations that
// disagree the first time one of them is fixed. So the value and its operations
// live here, and both stages call them.
//
// The model is the one an implementation of C actually needs:
//
//   * the value is **bits plus signedness**, so a wrap-around is defined
//     (two's complement) rather than implementation-defined, exactly as the
//     standard's unsigned arithmetic is;
//   * the width is **64 bits**, which is `intmax_t`/`uintmax_t`: the width
//     `#if` uses by the standard's own rule, and the width every constant here
//     is widened to. A literal too large for it is reported by the literal
//     reader rather than silently truncated;
//   * the only operations that are *not* a value are division or remainder by
//     zero and a shift count at or past the width. They return `std::nullopt`,
//     so a caller cannot forget to check a flag.
#pragma once

#include <cstdint>
#include <optional>

namespace minc::support {

// The width every operation wraps at. `static_assert`ed against the storage so a
// future widening is a compile error and not a silent truncation.
inline constexpr unsigned kConstIntWidth = 64;

struct ConstInt {
  // Two's complement bits; `isUnsigned` says how they are to be read. Keeping
  // the bits and the signedness apart is what makes wrap-around well defined
  // instead of implementation-defined.
  std::uint64_t bits = 0;
  bool isUnsigned = false;

  [[nodiscard]] constexpr std::int64_t signedValue() const {
    return static_cast<std::int64_t>(bits);
  }
  [[nodiscard]] constexpr bool truthy() const {
    return bits != 0;
  }
  // True when the value is read as a negative number. An unsigned value is never
  // negative, however its high bit looks.
  [[nodiscard]] constexpr bool negative() const {
    return !isUnsigned && signedValue() < 0;
  }
  [[nodiscard]] static constexpr ConstInt fromSigned(std::int64_t value) {
    return ConstInt{static_cast<std::uint64_t>(value), false};
  }
  [[nodiscard]] static constexpr ConstInt fromUnsigned(std::uint64_t value) {
    return ConstInt{value, true};
  }

  friend constexpr bool operator==(ConstInt, ConstInt) = default;
};

static_assert(kConstIntWidth == 64, "ConstInt storage is 64 bits");

[[nodiscard]] constexpr bool isZero(ConstInt value) {
  return value.bits == 0;
}

// --- arithmetic --------------------------------------------------------------
//
// These wrap, so they cannot fail. The result is unsigned when either operand is,
// which is the standard's rule for the operators that produce a value.

[[nodiscard]] constexpr ConstInt add(ConstInt a, ConstInt b) {
  return ConstInt{a.bits + b.bits, a.isUnsigned || b.isUnsigned};
}
[[nodiscard]] constexpr ConstInt sub(ConstInt a, ConstInt b) {
  return ConstInt{a.bits - b.bits, a.isUnsigned || b.isUnsigned};
}
[[nodiscard]] constexpr ConstInt mul(ConstInt a, ConstInt b) {
  return ConstInt{a.bits * b.bits, a.isUnsigned || b.isUnsigned};
}
[[nodiscard]] constexpr ConstInt bitAnd(ConstInt a, ConstInt b) {
  return ConstInt{a.bits & b.bits, a.isUnsigned || b.isUnsigned};
}
[[nodiscard]] constexpr ConstInt bitOr(ConstInt a, ConstInt b) {
  return ConstInt{a.bits | b.bits, a.isUnsigned || b.isUnsigned};
}
[[nodiscard]] constexpr ConstInt bitXor(ConstInt a, ConstInt b) {
  return ConstInt{a.bits ^ b.bits, a.isUnsigned || b.isUnsigned};
}
// The result of a unary operator has the promoted type of its operand, so the
// signedness is carried through unchanged.
[[nodiscard]] constexpr ConstInt negate(ConstInt a) {
  return ConstInt{0U - a.bits, a.isUnsigned};
}
[[nodiscard]] constexpr ConstInt bitNot(ConstInt a) {
  return ConstInt{~a.bits, a.isUnsigned};
}
// `!x` is `int` 0 or 1, always signed -- it is C's operator, not a bit operation.
[[nodiscard]] constexpr ConstInt logicalNot(ConstInt a) {
  return ConstInt::fromSigned(a.truthy() ? 0 : 1);
}

// `nullopt` when `b` is zero. The signed case is computed as signed, which the
// preprocessor's older inlined version did not do; a negative dividend divided
// by the raw bits is not division, and the differential test would have found it
// the first time a corpus entry used one.
[[nodiscard]] std::optional<ConstInt> divide(ConstInt a, ConstInt b);
[[nodiscard]] std::optional<ConstInt> remainder(ConstInt a, ConstInt b);

// `nullopt` when the count is at or past the width: that is the one shift
// condition that is neither defined nor a value, so it is a diagnostic and not a
// silent zero.
[[nodiscard]] std::optional<ConstInt> shiftLeft(ConstInt a, ConstInt b);
[[nodiscard]] std::optional<ConstInt> shiftRight(ConstInt a, ConstInt b);

// --- comparison --------------------------------------------------------------

// Bit equality. After the usual conversions an `intmax_t` and a `uintmax_t` are
// compared as unsigned, so comparing the bits is the standard's answer.
[[nodiscard]] constexpr bool equal(ConstInt a, ConstInt b) {
  return a.bits == b.bits;
}

// Negative when `a < b`, zero when equal, positive when `a > b`. Unsigned when
// either side is, which is the same rule `equal` follows.
[[nodiscard]] int compare(ConstInt a, ConstInt b);

} // namespace minc::support
