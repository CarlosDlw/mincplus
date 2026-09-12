// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Owned source text plus its line table. FileId comes from SourceManager.
//
// Every SourceFile created through SourceManager is guaranteed to be valid
// UTF-8, free of embedded NUL bytes, and within kMaxSourceBytes, with any
// UTF-8 BOM already stripped. Downstream stages may rely on that instead of
// re-checking.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "support/line/line_table.h"
#include "support/span/span.h"

namespace minc::support {

struct SourceFile {
  FileId id = kInvalidFile;
  // Bumped every time the text is replaced. Byte offsets are only meaningful
  // within one revision: inserting a character at the front shifts every
  // later offset, so a span, token, or diagnostic taken from revision N must
  // never be applied to revision N+1. Caches key on this value.
  std::uint32_t revision = 0;
  std::string path;
  std::string text;
  LineTable lines;

  SourceFile() = default;
  SourceFile(FileId fileId, std::string filePath, std::string fileText);

  // Replaces the text and rebuilds the line table. Not part of the public
  // flow because callers must go through SourceManager, which validates and
  // bumps the revision.
  void resetText(std::string fileText);

  [[nodiscard]] std::uint32_t size() const {
    return static_cast<std::uint32_t>(text.size());
  }
  [[nodiscard]] std::uint32_t lineCount() const {
    return lines.lineCount();
  }
  [[nodiscard]] LineCol lookup(std::uint32_t offset) const {
    return lines.lookup(offset, size());
  }

  [[nodiscard]] std::optional<std::string_view> lineText(std::uint32_t line) const;

  // Clamped slice: both bounds are capped to the text size and swapped if
  // reversed, so this never reads out of range.
  [[nodiscard]] std::string_view slice(std::uint32_t begin, std::uint32_t end) const;

  // Slice for a span, or nullopt when the span is invalid or from another file.
  [[nodiscard]] std::optional<std::string_view> sliceSpan(Span span) const;
};

} // namespace minc::support
