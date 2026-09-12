// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Filesystem questions the pipeline needs, answered in one place.
//
// This is the only module that knows what a path *is* on a given platform, the
// same way `support/term` is the only module that knows about terminals. The
// preprocessor asks "are these two paths the same file?" and gets a yes/no; it
// never learns whether the answer came from an inode, a Windows file index, or
// a case-folded canonical string.
//
// Two things here are load-bearing for correctness rather than convenience:
//
//   * **Identity is not the spelling.** macOS and Windows filesystems are
//     case-insensitive, and every platform has symlinks and `..`. A compiler
//     that keys "have I read this header?" on the path text reads `Foo.h` and
//     `foo.h` twice, and then two copies of the same include guard macro fight
//     over the same name. Identity is `(device, inode)` on POSIX and the
//     file index on Windows, with a case-folded canonical path as the fallback
//     where neither is available.
//   * **Nothing here reads the environment** or the working directory behind
//     the caller's back. Canonicalization is relative to the process's cwd,
//     which the driver controls; that is the only ambient input.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace minc::support {

// The directory part of `path`, without the trailing separator. Returns "." for
// a bare file name and "" for a path with no separator at all at the root
// (`/x` gives "/").
[[nodiscard]] std::string directoryOf(const std::string& path);

// Base name with the directory stripped.
[[nodiscard]] std::string fileNameOf(const std::string& path);

// Joins a directory and a relative name with the platform-independent '/',
// which every supported platform accepts in a path the compiler itself passes
// to the OS. A `name` that is already absolute wins, and an empty/`.` directory
// yields the name unchanged (so the result of joining never gains a leading
// "./" that would show up in diagnostics).
[[nodiscard]] std::string joinPath(const std::string& directory, const std::string& name);

[[nodiscard]] bool isAbsolutePath(const std::string& path);

// Lexically removes "." and ".." segments and duplicated separators. It does
// **not** touch the filesystem, so it can be applied to a path that does not
// exist (a missing include is still worth naming in a tidy way).
[[nodiscard]] std::string normalizePath(const std::string& path);

// Best-effort canonical absolute path. Falls back to `normalizePath` of the
// input when the file does not exist or the query fails, so the result is
// always usable and this function never throws.
[[nodiscard]] std::string canonicalPath(const std::string& path);

// True when `path` exists, is readable, and is a regular file. Used before
// opening a candidate include so a directory named like a header is not read.
[[nodiscard]] bool isRegularFile(const std::string& path);

// True when `path` exists and is a directory.
[[nodiscard]] bool isDirectory(const std::string& path);

// Identity of a file, comparable and hashable.
//
// `device`/`inode` are filled on platforms that expose them and stay zero
// otherwise; `canonical` is always filled. Two identities are equal when either
// the inode pair matches or, where there is no inode pair, the canonical paths
// match under the platform's case rules.
struct FileIdentity {
  std::uint64_t device = 0;
  std::uint64_t inode = 0;
  std::string canonical;
  // False when the path could not be identified at all (it does not exist).
  bool identified = false;
  // True when this identity's comparison must be case-sensitive, which is only
  // known for sure where the filesystem told us. Conservatively case-sensitive
  // on POSIX, case-insensitive on Windows.
  bool caseSensitive = true;
};

[[nodiscard]] FileIdentity identifyFile(const std::string& path);

[[nodiscard]] bool operator==(const FileIdentity& left, const FileIdentity& right);
[[nodiscard]] bool operator!=(const FileIdentity& left, const FileIdentity& right);

struct FileIdentityHash {
  [[nodiscard]] std::size_t operator()(const FileIdentity& identity) const;
};

} // namespace minc::support
