// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "support/source/source_manager.h"

#include <string>
#include <utility>

#include "support/limits.h"
#include "support/source/file_io.h"
#include "support/utf8/bom.h"
#include "support/utf8/validate.h"

namespace minc::support {
namespace {

// Brings raw bytes up to the guarantees documented in source_file.h, or
// explains what is wrong with them.
[[nodiscard]] Fallible<std::string> normalizeSource(const std::string& path, std::string text) {
  if (text.size() > kMaxSourceBytes) {
    return makeUnexpected<std::string>("source '" + path + "' is " + std::to_string(text.size()) +
                                       " bytes; the limit is " + kMaxSourceBytesText);
  }

  const utf8::ByteOrderMark mark = utf8::detectByteOrderMark(text);
  if (utf8::isUnsupportedEncoding(mark)) {
    // A UTF-16 BOM shows up as invalid UTF-8, which is a confusing message.
    // Name the real problem instead.
    return makeUnexpected<std::string>("source '" + path + "' is " + utf8::toString(mark) +
                                       "; convert it to UTF-8");
  }
  if (mark == utf8::ByteOrderMark::Utf8) {
    text.erase(0, utf8::byteOrderMarkLength(mark));
  }

  const std::size_t nul = text.find('\0');
  if (nul != std::string::npos) {
    return makeUnexpected<std::string>("source '" + path + "' contains a NUL byte at offset " +
                                       std::to_string(nul) + " (binary file?)");
  }

  const std::optional<std::size_t> invalid = utf8::firstInvalidOffset(text);
  if (invalid.has_value()) {
    return makeUnexpected<std::string>("source '" + path +
                                       "' is not valid UTF-8 (first bad byte at offset " +
                                       std::to_string(*invalid) + ")");
  }

  return text;
}

} // namespace

Fallible<FileId> SourceManager::addFile(std::string path, std::string text) {
  if (files_.size() >= kMaxSourceFiles) {
    return makeUnexpected<std::string>("too many source files (limit " +
                                       std::to_string(kMaxSourceFiles) + ")");
  }

  Fallible<std::string> normalized = normalizeSource(path, std::move(text));
  if (!normalized) {
    return makeUnexpected<std::string>(normalized.error());
  }

  const FileId id = static_cast<FileId>(files_.size());
  files_.emplace_back(id, std::move(path), std::move(normalized.value()));
  return id;
}

Fallible<FileId> SourceManager::loadFromDisk(const std::string& path) {
  Fallible<std::string> bytes = readFileBytes(path);
  if (!bytes) {
    return makeUnexpected<std::string>(bytes.error());
  }
  return addFile(path, std::move(bytes.value()));
}

const SourceFile* SourceManager::find(FileId id) const {
  if (id >= files_.size()) {
    return nullptr;
  }
  return &files_[id];
}

} // namespace minc::support
