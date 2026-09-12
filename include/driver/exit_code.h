// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Process exit codes. A stable contract for scripts, CI, and `make`.
#pragma once

#include <cstdint>

namespace minc::driver {

enum class ExitCode : std::uint8_t {
  Ok = 0,      // success
  Failure = 1, // compilation or runtime failure
  Usage = 2,   // invalid command line
};

[[nodiscard]] constexpr int exitCode(ExitCode code) {
  return static_cast<int>(code);
}

} // namespace minc::driver
