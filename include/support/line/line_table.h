// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Byte-offset to 1-based (line, column) mapping for one source text.
//
// Accepted line terminators: LF, CRLF, and a lone CR. Windows checkouts use
// CRLF and a CR must never leak into a rendered line or a caret would sit one
// column to the right of the token it points at. Columns are byte-based, which
// is what "file:line:col" consumers expect; the diagnostic renderer converts
// to display columns when it draws the caret.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "support/line/byte_range.h"
#include "support/line/line_col.h"

namespace minc::support {

class LineTable {
public:
  LineTable() = default;
  explicit LineTable(std::string_view text) {
    rebuild(text);
  }

  void rebuild(std::string_view text);

  [[nodiscard]] std::uint32_t lineCount() const {
    return static_cast<std::uint32_t>(starts_.size());
  }
  [[nodiscard]] bool empty() const {
    return starts_.empty();
  }

  // Clamps `offset` to `textSize` before mapping, so an offset at end of file
  // resolves to the position just past the last byte instead of wrapping.
  [[nodiscard]] LineCol lookup(std::uint32_t offset, std::uint32_t textSize) const;

  // Line text without its terminator.
  [[nodiscard]] std::optional<std::string_view> lineText(std::string_view text,
                                                         std::uint32_t line) const;

  // Byte range of the line, excluding its terminator.
  [[nodiscard]] std::optional<ByteRange> lineRange(std::string_view text, std::uint32_t line) const;

private:
  std::vector<std::uint32_t> starts_;
};

} // namespace minc::support
