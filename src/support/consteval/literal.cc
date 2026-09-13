// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "support/consteval/literal.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace minc::support {
namespace {

constexpr std::uint64_t kUint64Max = ~std::uint64_t{0};

// Value of one hex digit, or -1.
[[nodiscard]] int hexValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return 10 + (c - 'a');
  }
  if (c >= 'A' && c <= 'F') {
    return 10 + (c - 'A');
  }
  return -1;
}

[[nodiscard]] std::string quoted(std::string_view text) {
  return "'" + std::string(text) + "'";
}

} // namespace

std::optional<std::pair<std::uint64_t, std::size_t>> decodeCharOrEscape(std::string_view body,
                                                                        std::size_t index) {
  if (index >= body.size()) {
    return std::nullopt;
  }
  if (body[index] != '\\') {
    return std::make_pair(static_cast<std::uint64_t>(static_cast<unsigned char>(body[index])),
                          index + 1);
  }
  if (index + 1 >= body.size()) {
    return std::nullopt;
  }
  const char escape = body[index + 1];
  switch (escape) {
  case 'n':
    return std::make_pair(10U, index + 2);
  case 't':
    return std::make_pair(9U, index + 2);
  case 'r':
    return std::make_pair(13U, index + 2);
  case 'a':
    return std::make_pair(7U, index + 2);
  case 'b':
    return std::make_pair(8U, index + 2);
  case 'f':
    return std::make_pair(12U, index + 2);
  case 'v':
    return std::make_pair(11U, index + 2);
  case '0':
  case '1':
  case '2':
  case '3':
  case '4':
  case '5':
  case '6':
  case '7': {
    std::uint64_t value = 0;
    std::size_t next = index + 1;
    std::size_t digits = 0;
    while (next < body.size() && digits < 3 && body[next] >= '0' && body[next] <= '7') {
      value = value * 8U + static_cast<std::uint64_t>(body[next] - '0');
      ++next;
      ++digits;
    }
    return std::make_pair(value, next);
  }
  case 'x':
  case 'X': {
    std::uint64_t value = 0;
    std::size_t next = index + 2;
    std::size_t digits = 0;
    while (next < body.size()) {
      const int digit = hexValue(body[next]);
      if (digit < 0) {
        break;
      }
      // A hex escape may not exceed one byte: consuming more digits would
      // silently wrap, which is the one thing a reader must not do here.
      value = value * 16U + static_cast<std::uint64_t>(digit);
      ++next;
      ++digits;
    }
    if (digits == 0) {
      return std::nullopt;
    }
    return std::make_pair(value, next);
  }
  case '\\':
  case '\'':
  case '"':
    return std::make_pair(static_cast<std::uint64_t>(static_cast<unsigned char>(escape)),
                          index + 2);
  default:
    return std::nullopt;
  }
}

IntegerLiteral parseIntegerLiteral(std::string_view text, IntegerBaseRule baseRule) {
  IntegerLiteral result;
  if (text.empty()) {
    result.message = "an empty integer literal";
    return result;
  }

  std::size_t index = 0;
  unsigned base = 10;
  if (text.size() >= 2 && text[0] == '0') {
    const char prefix = text[1];
    if (prefix == 'x' || prefix == 'X') {
      base = 16;
      index = 2;
    } else if (prefix == 'b' || prefix == 'B') {
      base = 2;
      index = 2;
    } else if (prefix == 'o' || prefix == 'O') {
      // `.mx`'s explicit octal. Accepted in both rules: it is unambiguous, and a
      // reader that accepts it in one context and not the other would be the
      // kind of difference this file exists to remove.
      base = 8;
      index = 2;
    } else if (baseRule == IntegerBaseRule::ImplicitOctal && prefix >= '0' && prefix <= '7') {
      base = 8;
      index = 1;
    }
  }

  std::uint64_t value = 0;
  bool overflowed = false;
  bool anyDigit = false;
  for (; index < text.size(); ++index) {
    const int digit = hexValue(text[index]);
    if (digit < 0 || static_cast<unsigned>(digit) >= base) {
      break;
    }
    anyDigit = true;
    const auto digitValue = static_cast<std::uint64_t>(digit);
    // Overflow is decided *before* multiplying, against the exact room left. A
    // "did the value shrink" or `value > max / base` test rejects
    // `0xFFFFFFFFFFFFFFFF`, which fits in 64 bits and is the very literal a
    // `#if` is most likely to compare against.
    if (value > (kUint64Max - digitValue) / base) {
      overflowed = true;
    }
    // Keep consuming digits after the overflow: the suffix scan starts where
    // the digits end, and stopping early would leave `...0bignumULL` half lexed.
    value = value * base + digitValue;
  }
  if (!anyDigit) {
    result.message = "malformed integer literal " + quoted(text);
    return result;
  }

  bool isUnsigned = false;
  for (; index < text.size(); ++index) {
    const char suffix = text[index];
    if (suffix == 'u' || suffix == 'U') {
      isUnsigned = true;
    } else if (suffix != 'l' && suffix != 'L') {
      result.message = "unknown suffix in integer literal " + quoted(text);
      return result;
    }
  }
  if (overflowed) {
    result.value = ConstInt{value, isUnsigned};
    result.tooWide = true;
    result.message = "integer literal " + quoted(text) + " does not fit in 64 bits";
    return result;
  }
  // A decimal literal too large for `intmax_t` is evaluated as unsigned, which
  // is the standard's behaviour rather than a diagnostic.
  if (!isUnsigned && value > static_cast<std::uint64_t>(INT64_MAX)) {
    isUnsigned = true;
  }
  result.value = ConstInt{value, isUnsigned};
  result.ok = true;
  return result;
}

IntegerLiteral parseCharLiteral(std::string_view text) {
  IntegerLiteral result;
  // The lexer guarantees the quotes; the body may be empty or escaped, which is
  // the lexer's finding to report, not this reader's to invent.
  if (text.size() < 2) {
    result.message = "malformed character literal " + quoted(text);
    return result;
  }
  const std::string_view body = text.substr(1, text.size() - 2);
  std::uint64_t value = 0;
  std::size_t index = 0;
  while (index < body.size()) {
    const std::optional<std::pair<std::uint64_t, std::size_t>> decoded =
        decodeCharOrEscape(body, index);
    if (!decoded.has_value()) {
      result.message = "unknown escape in character literal " + quoted(text);
      return result;
    }
    index = decoded->second;
    // Multi-character literals are implementation-defined; this is the packed
    // value GCC produces, which is the least surprising choice and the one the
    // preprocessor already used.
    value = (value << 8U) | decoded->first;
  }
  result.value = ConstInt::fromSigned(static_cast<std::int64_t>(value));
  result.ok = true;
  return result;
}

} // namespace minc::support
