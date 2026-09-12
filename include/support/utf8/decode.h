// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Single-scalar UTF-8 decode. Strict: rejects overlong forms, surrogates,
// values above U+10FFFF, truncated sequences, and stray continuation bytes.
// This is the single decoder used by validation and by every later stage, so
// "what the validator accepts" and "what the decoder produces" cannot drift.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace minc::support::utf8 {

struct Decoded {
  char32_t codePoint = 0;
  std::uint32_t length = 0; // bytes consumed (1..4)
};

// Decodes one scalar at `offset`. Returns nullopt when `offset` is past the
// end or the sequence starting there is not a well-formed scalar.
[[nodiscard]] std::optional<Decoded> decodeOne(std::string_view text, std::size_t offset);

// Number of scalars in `text`. Well-formed input decodes one per scalar; each
// malformed byte is counted once so the result is always total.
[[nodiscard]] std::size_t countCodePoints(std::string_view text);

} // namespace minc::support::utf8
