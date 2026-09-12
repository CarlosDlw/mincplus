// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "support/utf8/decode.h"

namespace minc::support::utf8 {

std::optional<Decoded> decodeOne(std::string_view text, std::size_t offset) {
  const std::size_t size = text.size();
  if (offset >= size) {
    return std::nullopt;
  }

  const auto lead = static_cast<unsigned char>(text[offset]);
  if (lead < 0x80) {
    return Decoded{lead, 1};
  }

  std::uint32_t codePoint = 0;
  std::uint32_t minimum = 0;
  std::size_t length = 0;
  if ((lead & 0xE0) == 0xC0) {
    length = 2;
    codePoint = lead & 0x1Fu;
    minimum = 0x80u;
  } else if ((lead & 0xF0) == 0xE0) {
    length = 3;
    codePoint = lead & 0x0Fu;
    minimum = 0x800u;
  } else if ((lead & 0xF8) == 0xF0) {
    length = 4;
    codePoint = lead & 0x07u;
    minimum = 0x10000u;
  } else {
    return std::nullopt; // stray continuation byte or invalid lead
  }

  // Compare against the remaining bytes first so `offset + length` can never
  // overflow.
  if (length > size - offset) {
    return std::nullopt;
  }
  for (std::size_t i = 1; i < length; ++i) {
    const auto byte = static_cast<unsigned char>(text[offset + i]);
    if ((byte & 0xC0) != 0x80) {
      return std::nullopt;
    }
    codePoint = (codePoint << 6) | (byte & 0x3Fu);
  }

  if (codePoint < minimum || codePoint > 0x10FFFFu) {
    return std::nullopt; // overlong form or beyond the Unicode range
  }
  if (codePoint >= 0xD800u && codePoint <= 0xDFFFu) {
    return std::nullopt; // UTF-16 surrogate half
  }
  return Decoded{static_cast<char32_t>(codePoint), static_cast<std::uint32_t>(length)};
}

std::size_t countCodePoints(std::string_view text) {
  std::size_t count = 0;
  std::size_t offset = 0;
  while (offset < text.size()) {
    const std::optional<Decoded> decoded = decodeOne(text, offset);
    if (!decoded.has_value()) {
      ++count; // malformed byte counts once, keeping the result total
      ++offset;
      continue;
    }
    ++count;
    offset += decoded->length;
  }
  return count;
}

} // namespace minc::support::utf8
