// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "support/session/session.h"

namespace minc::support {

std::optional<std::uint32_t> Session::revisionOf(FileId id) const {
  const SourceFile* file = sources_.find(id);
  if (file == nullptr) {
    return std::nullopt;
  }
  return file->revision;
}

} // namespace minc::support
