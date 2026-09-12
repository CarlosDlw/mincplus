// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Half-open byte range [begin, end) inside one source file.
#pragma once

#include <cstdint>

#include "support/limits.h"
#include "support/span/file_id.h"

namespace minc::support {

struct Span {
  FileId file = kInvalidFile;
  std::uint32_t begin = 0;
  std::uint32_t end = 0;

  constexpr Span() = default;
  constexpr Span(FileId f, std::uint32_t b, std::uint32_t e) : file(f), begin(b), end(e) {}

  // One-byte span at `offset`, saturating at the uint32 ceiling instead of
  // wrapping to 0 (which would silently produce a span at the file start).
  static constexpr Span at(FileId f, std::uint32_t offset) {
    return offset == kMaxOffset ? Span(f, offset, offset) : Span(f, offset, offset + 1);
  }

  [[nodiscard]] constexpr bool valid() const {
    return file != kInvalidFile && begin <= end;
  }
  [[nodiscard]] constexpr bool empty() const {
    return begin == end;
  }
  // Never underflows: a mis-ordered span reports zero length rather than a
  // ~4 GiB value. Callers that need to reject it should check valid().
  [[nodiscard]] constexpr std::uint32_t size() const {
    return end > begin ? end - begin : 0;
  }
  [[nodiscard]] constexpr bool contains(std::uint32_t offset) const {
    return begin <= offset && offset < end;
  }
  [[nodiscard]] constexpr bool containsClosed(std::uint32_t offset) const {
    return begin <= offset && offset <= end;
  }
};

} // namespace minc::support
