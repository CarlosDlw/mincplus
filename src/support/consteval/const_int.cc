// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "support/consteval/const_int.h"

#include <cstdint>
#include <limits>
#include <optional>

namespace minc::support {
namespace {

// The smallest signed value, as bits. Division of it by -1 overflows, and the
// language's answer is the wrapped result rather than undefined behaviour --
// which is what keeps this file from being the one place a sanitizer build can
// abort on hostile input.
constexpr std::int64_t kInt64Min = std::numeric_limits<std::int64_t>::min();

} // namespace

std::optional<ConstInt> divide(ConstInt a, ConstInt b) {
  if (isZero(b)) {
    return std::nullopt;
  }
  if (a.isUnsigned || b.isUnsigned) {
    return ConstInt::fromUnsigned(a.bits / b.bits);
  }
  if (a.signedValue() == kInt64Min && b.signedValue() == -1) {
    return ConstInt::fromSigned(kInt64Min);
  }
  return ConstInt::fromSigned(a.signedValue() / b.signedValue());
}

std::optional<ConstInt> remainder(ConstInt a, ConstInt b) {
  if (isZero(b)) {
    return std::nullopt;
  }
  if (a.isUnsigned || b.isUnsigned) {
    return ConstInt::fromUnsigned(a.bits % b.bits);
  }
  if (a.signedValue() == kInt64Min && b.signedValue() == -1) {
    return ConstInt::fromSigned(0);
  }
  return ConstInt::fromSigned(a.signedValue() % b.signedValue());
}

std::optional<ConstInt> shiftLeft(ConstInt a, ConstInt b) {
  if (b.bits >= kConstIntWidth) {
    return std::nullopt;
  }
  // An unsigned shift count is still a count; a "negative" one is huge and is
  // caught by the test above, which is the standard's rule for `#if` too.
  return ConstInt{a.bits << b.bits, a.isUnsigned};
}

std::optional<ConstInt> shiftRight(ConstInt a, ConstInt b) {
  if (b.bits >= kConstIntWidth) {
    return std::nullopt;
  }
  const std::uint64_t count = b.bits;
  if (count == 0) {
    return ConstInt{a.bits, a.isUnsigned};
  }
  if (!a.negative()) {
    return ConstInt{a.bits >> count, a.isUnsigned};
  }
  // Arithmetic shift, written out rather than relying on a signed right shift.
  // Right-shifting a negative signed value is implementation-defined; this is
  // the definition, so Linux and Windows cannot disagree about `-1 >> 1`.
  const std::uint64_t mask = ~std::uint64_t{0} << (kConstIntWidth - count);
  return ConstInt{(a.bits >> count) | mask, a.isUnsigned};
}

int compare(ConstInt a, ConstInt b) {
  if (a.isUnsigned || b.isUnsigned) {
    if (a.bits < b.bits) {
      return -1;
    }
    return a.bits > b.bits ? 1 : 0;
  }
  const std::int64_t lhs = a.signedValue();
  const std::int64_t rhs = b.signedValue();
  if (lhs < rhs) {
    return -1;
  }
  return lhs > rhs ? 1 : 0;
}

} // namespace minc::support
