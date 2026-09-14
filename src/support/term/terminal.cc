// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "support/term/terminal.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include <cctype>

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
#include <sys/ioctl.h>
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

// `COLUMNS`, or nothing. The value is only believed when it is a number the
// terminal could actually be: an empty string, a word, and `0` are all ignored
// rather than clamped, because a caller that set `COLUMNS=0` meant "no opinion"
// far more often than it meant "one column".
[[nodiscard]] std::optional<unsigned> columnsFromEnvironment(const char* value) {
  if (value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  unsigned width = 0;
  for (const char* digit = value; *digit != '\0'; ++digit) {
    if (std::isdigit(static_cast<unsigned char>(*digit)) == 0) {
      return std::nullopt;
    }
    // A number long enough to overflow is not a terminal width; refuse rather
    // than wrap around into a small one.
    if (width > 1000000U) {
      return std::nullopt;
    }
    width = width * 10U + static_cast<unsigned>(*digit - '0');
  }
  if (width == 0U) {
    return std::nullopt;
  }
  return width;
}

#if defined(_WIN32)

[[nodiscard]] std::optional<unsigned> terminalWidthOf(std::FILE* stream) {
  const auto handle =
      reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(_get_osfhandle(_fileno(stream))));
  if (handle == INVALID_HANDLE_VALUE) {
    return std::nullopt;
  }
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (GetConsoleScreenBufferInfo(handle, &info) == 0) {
    return std::nullopt;
  }
  // The *window*, not the buffer: a buffer is often wider than the window that
  // shows part of it, and wrapping to the buffer produces lines the user cannot
  // see the end of. A window with no width (a hidden console) is no answer.
  const int columns = info.srWindow.Right - info.srWindow.Left + 1;
  if (columns <= 0) {
    return std::nullopt;
  }
  return static_cast<unsigned>(columns);
}

#else

[[nodiscard]] std::optional<unsigned> terminalWidthOf(std::FILE* stream) {
  if (!isTerminal(stream)) {
    return std::nullopt;
  }
  ::winsize size{};
  if (::ioctl(::fileno(stream), TIOCGWINSZ, &size) != 0 || size.ws_col == 0) {
    return std::nullopt;
  }
  return static_cast<unsigned>(size.ws_col);
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

std::optional<ColorChoice> colorChoiceFromName(std::string_view name) {
  if (name == "auto") {
    return ColorChoice::Auto;
  }
  if (name == "always") {
    return ColorChoice::Always;
  }
  if (name == "never") {
    return ColorChoice::Never;
  }
  return std::nullopt;
}

const char* toString(ColorChoice choice) {
  switch (choice) {
  case ColorChoice::Auto:
    return "auto";
  case ColorChoice::Always:
    return "always";
  case ColorChoice::Never:
    return "never";
  }
  return "auto";
}

unsigned terminalWidth(std::FILE* stream) {
  // The terminal first and `COLUMNS` second would be the other order and it is
  // the wrong one: a user who exported a width did it precisely because the
  // program's guess was not what they wanted.
  if (const std::optional<unsigned> fromEnvironment =
          columnsFromEnvironment(environmentValue("COLUMNS"))) {
    return *fromEnvironment;
  }
  if (const std::optional<unsigned> fromTerminal = terminalWidthOf(stream)) {
    return *fromTerminal;
  }
  return kDefaultTerminalWidth;
}

} // namespace minc::support
