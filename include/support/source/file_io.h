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

} // namespace minc::support
