// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Byte-order-mark detection. Editors on Windows routinely save with a UTF-8
// BOM and occasionally with a UTF-16 one; treating those bytes as source text
// would corrupt the first token, so the boundary detects them explicitly.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace minc::support::utf8 {

enum class ByteOrderMark : std::uint8_t {
  None,
  Utf8,
  Utf16LittleEndian,
  Utf16BigEndian,
  Utf32LittleEndian,
  Utf32BigEndian,
};

// Detects a leading mark. Checked longest-first so UTF-32LE (FF FE 00 00) is
// not mistaken for UTF-16LE (FF FE).
[[nodiscard]] ByteOrderMark detectByteOrderMark(std::string_view text);

// Length of the mark in bytes (0 when there is none).
[[nodiscard]] std::size_t byteOrderMarkLength(ByteOrderMark mark);

// True for encodings this compiler does not accept (everything but UTF-8).
[[nodiscard]] bool isUnsupportedEncoding(ByteOrderMark mark);

// Human-readable encoding name for diagnostics ("UTF-16 little-endian", ...).
[[nodiscard]] const char* toString(ByteOrderMark mark);

} // namespace minc::support::utf8
