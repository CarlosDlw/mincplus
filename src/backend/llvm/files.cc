// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// One temporary directory per invocation, and the reason it is not a name.
//
// A compiler writes objects, and a `run` writes an executable, so both need a
// place. `/tmp/mincc-out.o` is the name everybody writes first and it is the
// wrong one twice over: two compilers running at once overwrite each other, and
// in a world-writable directory the name can be a symlink someone else chose, so
// the write lands wherever they aimed it. `createUniqueDirectory` is
// `mkdir` on a name with a random suffix (the create is atomic, so the loser of
// a race gets a different name rather than a surprise), and the prefix carries
// the program's name so a stray directory is attributable.
//
// The removal is in the destructor for the reason destructors exist: the error
// path is the one that leaks, and the error path is the one that runs when
// someone is already looking at a diagnostic and does not want to also clean up.
#include <string>
#include <system_error>

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include "backend/codegen.h"

namespace minc::backend {

TempDir::TempDir() {
  // The *system* temp directory and not `$TMPDIR` from a header: LLVM's path
  // layer already applies the platform's rules (and on Windows it prefers the
  // user's directory over a world-writable one), which is the whole reason this
  // module contains no `#if defined(_WIN32)`.
  llvm::SmallString<128> base;
  llvm::sys::path::system_temp_directory(/*ErasedOnReboot=*/false, base);
  llvm::SmallString<128> pattern(base);
  llvm::sys::path::append(pattern, "minc+-%%%%%%");

  llvm::SmallString<128> created;
  const std::error_code error = llvm::sys::fs::createUniqueDirectory(pattern, created);
  if (error) {
    return; // `path_` stays empty and `valid()` is false.
  }
  path_.assign(created.begin(), created.end());
}

TempDir::~TempDir() {
  if (!path_.empty()) {
    // Best effort: a directory that cannot be removed is a mess, not a failure
    // of the compilation the user asked for, and reporting it would be a
    // diagnostic about our housekeeping in the middle of theirs.
    // NOLINTNEXTLINE(bugprone-unused-return-value)
    (void)llvm::sys::fs::remove_directories(path_, /*IgnoreErrors=*/true);
  }
}

TempDir::TempDir(TempDir&& other) noexcept : path_(std::move(other.path_)) {
  // Moved-from: the source must not remove the directory the target now owns.
  other.path_.clear();
}

TempDir& TempDir::operator=(TempDir&& other) noexcept {
  if (this != &other) {
    if (!path_.empty()) {
      // NOLINTNEXTLINE(bugprone-unused-return-value)
      (void)llvm::sys::fs::remove_directories(path_, /*IgnoreErrors=*/true);
    }
    path_ = std::move(other.path_);
    other.path_.clear();
  }
  return *this;
}

std::string TempDir::file(std::string_view name) const {
  if (path_.empty()) {
    return std::string{};
  }
  llvm::SmallString<128> result(path_);
  llvm::sys::path::append(result, llvm::Twine(name));
  return std::string(result.begin(), result.end());
}

} // namespace minc::backend
