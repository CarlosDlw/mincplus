// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "support/utf8/validate.h"

#include "support/utf8/decode.h"

namespace minc::support::utf8 {

bool isValid(std::string_view text) {
  return !firstInvalidOffset(text).has_value();
}

std::optional<std::size_t> firstInvalidOffset(std::string_view text) {
  // Validation is exactly "walk the text with the strict decoder". Keeping a
  // second hand-written state machine here would be a place for the two to
  // disagree, so there is only one.
  std::size_t offset = 0;
  while (offset < text.size()) {
    const std::optional<Decoded> decoded = decodeOne(text, offset);
    if (!decoded.has_value()) {
      return offset;
    }
    offset += decoded->length;
  }
  return std::nullopt;
}

} // namespace minc::support::utf8
