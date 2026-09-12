// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "support/source/source_file.h"

#include <algorithm>
#include <utility>

namespace minc::support {

SourceFile::SourceFile(FileId fileId, std::string filePath, std::string fileText)
    : id(fileId), path(std::move(filePath)), text(std::move(fileText)) {
  // Rebuild from the member, not the parameter: the parameter has already been
  // moved from, and reading it here used to leave every file with a single
  // line, so all diagnostics pointed at line 1.
  lines.rebuild(this->text);
}

void SourceFile::resetText(std::string fileText) {
  text = std::move(fileText);
  lines.rebuild(text);
  ++revision;
}

std::optional<std::string_view> SourceFile::lineText(std::uint32_t line) const {
  return lines.lineText(text, line);
}

std::string_view SourceFile::slice(std::uint32_t begin, std::uint32_t end) const {
  const std::uint32_t size = this->size();
  begin = std::min(begin, size);
  end = std::min(end, size);
  if (begin > end) {
    begin = end;
  }
  return std::string_view(text.data() + begin, end - begin);
}

std::optional<std::string_view> SourceFile::sliceSpan(Span span) const {
  if (!span.valid() || span.file != id) {
    return std::nullopt;
  }
  return slice(span.begin, span.end);
}

} // namespace minc::support
