// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Binary file read used by SourceManager. Kept separate for testing.
#pragma once

#include <string>

#include "support/expected/fallible.h"

namespace minc::support {

// Reads a whole file as raw bytes. The path is treated as UTF-8 so non-ASCII
// names work on Windows as well as POSIX. Errors are human-readable and name
// the path; directories, missing files, oversized files, and short reads are
// all reported instead of silently producing empty input.
[[nodiscard]] Fallible<std::string> readFileBytes(const std::string& path);

// Reads all of standard input as raw bytes, under the same size limit.
//
// On Windows the stream is switched to binary mode first: without that the CRT
// rewrites CRLF to LF on the way in and the compiler would see a different
// file than the one on disk, in exactly the case (piping a file) where nobody
// would think to check.
[[nodiscard]] Fallible<std::string> readStdinBytes();

} // namespace minc::support
