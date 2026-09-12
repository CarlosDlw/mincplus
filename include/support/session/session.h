// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Central per-compilation state.
//
// One Session owns everything shared across the pipeline: the loaded sources,
// the symbol interner, the diagnostic bag, and the arena the later stages
// allocate AST/IR nodes from. Frontend stages take a `Session&` instead of
// reaching for globals, which is what makes them reusable for the language
// server as well as the batch compiler:
//
//   * the LSP keeps one Session per analysis pass,
//   * an edit goes through updateFile(), which keeps the FileId stable and
//     bumps the file's revision,
//   * derived data keyed on the old revision is dropped rather than repaired.
//
// A Session owns its members directly and is neither copyable nor movable, so
// the addresses of those members and of everything they hand out (SourceFile
// pointers, interned views) stay stable for its lifetime.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "support/diag/diag_bag.h"
#include "support/expected/fallible.h"
#include "support/intern/interner.h"
#include "support/mem/arena.h"
#include "support/source/source_manager.h"
#include "support/span/file_id.h"

namespace minc::support {

class Session {
public:
  Session() = default;

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  [[nodiscard]] SourceManager& sources() {
    return sources_;
  }
  [[nodiscard]] const SourceManager& sources() const {
    return sources_;
  }

  [[nodiscard]] Interner& symbols() {
    return symbols_;
  }
  [[nodiscard]] const Interner& symbols() const {
    return symbols_;
  }

  [[nodiscard]] DiagBag& diags() {
    return diags_;
  }
  [[nodiscard]] const DiagBag& diags() const {
    return diags_;
  }

  // Backing store for AST/IR nodes. Later stages may add their own arenas,
  // but anything that outlives a single phase belongs here.
  [[nodiscard]] Arena& arena() {
    return arena_;
  }

  // Thin wrappers so callers do not have to reach through sources().
  [[nodiscard]] Fallible<FileId> addFile(std::string path, std::string text) {
    return sources_.addFile(std::move(path), std::move(text));
  }
  [[nodiscard]] Fallible<FileId> loadFromDisk(const std::string& path) {
    return sources_.loadFromDisk(path);
  }

  // Editor/LSP path: replace a file's contents in place, keeping its FileId
  // and bumping its revision.
  [[nodiscard]] Fallible<void> updateFile(FileId id, std::string text) {
    return sources_.replaceText(id, std::move(text));
  }

  // Revision to key caches on, or nullopt when the file is unknown.
  [[nodiscard]] std::optional<std::uint32_t> revisionOf(FileId id) const;

private:
  SourceManager sources_;
  Interner symbols_;
  DiagBag diags_;
  Arena arena_;
};

} // namespace minc::support
