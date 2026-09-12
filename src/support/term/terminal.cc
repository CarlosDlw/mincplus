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

// getenv is flagged as unsafe by MSVC's CRT deprecation warnings. The concern is
// thread safety with a concurrently-modified environment, and these read the
// environment during single-threaded startup.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
[[nodiscard]] const char* environmentValue(const char* name) {
  return std::getenv(name);
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

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
  if (colorDisabledByEnvironmentValue(environmentValue("NO_COLOR")) ||
      terminalIsDumbForValue(environmentValue("TERM"))) {
    return false;
  }
#if defined(_WIN32)
  return windowsSupportsColor(stream);
#else
  return isTerminal(stream);
#endif
}

} // namespace

bool terminalIsDumbForValue(const char* termValue) {
  return termValue != nullptr && std::string_view(termValue) == "dumb";
}

bool stdoutSupportsColor() {
  return supportsColor(stdout);
}

bool stderrSupportsColor() {
  return supportsColor(stderr);
}

} // namespace minc::support
