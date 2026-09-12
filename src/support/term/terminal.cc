// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "support/term/terminal.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#if defined(_WIN32)
// windows.h is included after our own headers so it cannot shadow anything we
// declare, and lean-and-mean keeps the translation unit fast to compile.
// NOMINMAX keeps the min/max macros from breaking any use of std::min/max.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace minc::support {
namespace {

// The NO_COLOR convention (https://no-color.org): if the variable is present at
// all, color is off -- the value is deliberately ignored.
[[nodiscard]] bool colorDisabledByEnvironment() {
#if defined(_MSC_VER)
  // getenv is flagged as unsafe by MSVC's CRT deprecation warnings. The concern
  // is thread safety, and this runs during single-threaded startup.
#pragma warning(suppress : 4996)
#endif
  return std::getenv("NO_COLOR") != nullptr;
}

// TERM=dumb is the POSIX way of saying "no control sequences". Honored on every
// platform so the same environment produces the same output under MSYS2 and
// under Linux.
[[nodiscard]] bool terminalIsDumb() {
  const char* term = std::getenv("TERM");
  return term != nullptr && std::string_view(term) == "dumb";
}

#if defined(_WIN32)

// Windows consoles do not interpret ANSI escapes until virtual terminal
// processing is turned on, and it is off by default. GetConsoleMode fails for a
// redirected stream, which is the correct signal to stay plain.
[[nodiscard]] bool windowsSupportsColor(std::FILE* stream) {
  const auto handle =
      reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(_get_osfhandle(_fileno(stream))));
  if (handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD mode = 0;
  if (GetConsoleMode(handle, &mode) == 0) {
    return false;
  }
  if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0) {
    return true;
  }
  // Pre-1511 Windows and non-console handles fail here; plain text it is.
  return SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
}

#else

[[nodiscard]] bool isTerminal(std::FILE* stream) {
  return ::isatty(::fileno(stream)) != 0;
}

#endif

[[nodiscard]] bool supportsColor(std::FILE* stream) {
  if (colorDisabledByEnvironment() || terminalIsDumb()) {
    return false;
  }
#if defined(_WIN32)
  return windowsSupportsColor(stream);
#else
  return isTerminal(stream);
#endif
}

} // namespace

bool stdoutSupportsColor() {
  return supportsColor(stdout);
}

bool stderrSupportsColor() {
  return supportsColor(stderr);
}

} // namespace minc::support
