// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "support/line/line_table.h"

namespace minc::support {

void LineTable::rebuild(std::string_view text) {
  starts_.clear();
  starts_.push_back(0);

  const std::size_t size = text.size();
  for (std::size_t i = 0; i < size; ++i) {
    const char byte = text[i];
    if (byte == '\n') {
      starts_.push_back(static_cast<std::uint32_t>(i + 1));
      continue;
    }
    if (byte == '\r') {
      // CRLF is one break; a lone CR is also a break (classic Mac endings).
      std::size_t next = i + 1;
      if (next < size && text[next] == '\n') {
        ++next;
      }
      starts_.push_back(static_cast<std::uint32_t>(next));
      i = next - 1; // the loop increment moves past the terminator
    }
  }
}

LineCol LineTable::lookup(std::uint32_t offset, std::uint32_t textSize) const {
  if (starts_.empty()) {
    return LineCol{1, 1};
  }
  if (offset > textSize) {
    offset = textSize;
  }

  // Last start <= offset. starts_ is sorted and non-empty.
  std::uint32_t lo = 0;
  std::uint32_t hi = static_cast<std::uint32_t>(starts_.size());
  while (lo + 1 < hi) {
    const std::uint32_t mid = lo + (hi - lo) / 2;
    if (starts_[mid] <= offset) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return LineCol{lo + 1, offset - starts_[lo] + 1};
}

std::optional<std::string_view> LineTable::lineText(std::string_view text,
                                                    std::uint32_t line) const {
  const std::optional<ByteRange> range = lineRange(text, line);
  if (!range.has_value()) {
    return std::nullopt;
  }
  return text.substr(range->begin, range->end - range->begin);
}

std::optional<ByteRange> LineTable::lineRange(std::string_view text, std::uint32_t line) const {
  if (line == 0 || line > starts_.size()) {
    return std::nullopt;
  }
  const std::uint32_t begin = starts_[line - 1];
  std::uint32_t end = static_cast<std::uint32_t>(text.size());
  if (line < starts_.size()) {
    // The next start points just past this line's terminator; trim it so the
    // returned text never contains CR or LF.
    end = starts_[line];
    if (end > begin && text[end - 1] == '\n') {
      --end;
    }
    if (end > begin && text[end - 1] == '\r') {
      --end;
    }
  }
  return ByteRange{begin, end};
}

} // namespace minc::support
