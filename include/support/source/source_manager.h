// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Owns all loaded sources and hands out stable FileIds.
//
// This is the boundary where source bytes become trusted text: both entry
// points enforce the size limit, reject non-UTF-8 encodings, strip a UTF-8
// BOM, reject embedded NUL bytes, and validate UTF-8. Failures are returned,
// never printed, so the driver decides how to report them.
#pragma once

#include <cstdint>
#include <deque>
#include <string>

#include "support/expected/fallible.h"
#include "support/source/source_file.h"
#include "support/span/file_id.h"

namespace minc::support {

class SourceManager {
public:
  SourceManager() = default;

  // Store an in-memory buffer (tests, stdin, generated code).
  [[nodiscard]] Fallible<FileId> addFile(std::string path, std::string text);

  // Read a file from disk. Error is a human-readable message naming the path.
  [[nodiscard]] Fallible<FileId> loadFromDisk(const std::string& path);

  [[nodiscard]] const SourceFile* find(FileId id) const;
  [[nodiscard]] std::uint32_t fileCount() const {
    return static_cast<std::uint32_t>(files_.size());
  }
  [[nodiscard]] bool empty() const {
    return files_.empty();
  }

private:
  // std::deque keeps SourceFile addresses (and the string views inside them)
  // stable as more files are added.
  std::deque<SourceFile> files_;
};

} // namespace minc::support
