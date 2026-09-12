// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Terminal capabilities, isolated in one module.
//
// This is the only place in `support` that contains platform-specific code
// (`<windows.h>`, `<unistd.h>`), and it exists so no other module has to. The
// rest of the codebase asks a yes/no question and never learns which OS it is
// running on, which is what keeps the compiler's output identical everywhere
// and testable without a terminal.
#pragma once

#include <cstdint>

namespace minc::support {

// Whether rendered text carries color at all. Everything that formats output
// and can be colored takes one of these instead of a bool, so "colored?" and
// "which escape sequences?" stay one decision.
enum class ColorMode : std::uint8_t {
  Plain, // no escape sequences: safe for files, pipes, and tests
  Ansi,  // ANSI SGR colors; only enabled for a capable terminal
};

// True when writing ANSI SGR sequences to this stream produces color rather
// than literal escape text.
//
// False when the stream is redirected (so output stays clean in a pipe or a
// log file), when NO_COLOR is set to anything, when TERM=dumb, or when the
// Windows console cannot be put into virtual-terminal mode. On Windows the
// enabling call happens at most once per process and never throws.
//
// The answer is stable: it is a question about the process's environment and
// its standard streams, so asking twice never disagrees.
[[nodiscard]] bool stdoutSupportsColor();
[[nodiscard]] bool stderrSupportsColor();

// The two environment rules as pure predicates, so the behavior is testable
// without a terminal and without mutating the process environment.
//
// `noColorValue` is the value of NO_COLOR, or nullptr when it is unset. Per the
// NO_COLOR convention (https://no-color.org) the *presence* of the variable
// turns color off, whatever it holds -- including the empty string.
[[nodiscard]] constexpr bool colorDisabledByEnvironmentValue(const char* noColorValue) {
  return noColorValue != nullptr;
}

// `termValue` is the value of TERM, or nullptr when it is unset. TERM=dumb is
// the POSIX way of saying "no control sequences", honored on every platform so
// the same environment produces the same output everywhere.
[[nodiscard]] bool terminalIsDumbForValue(const char* termValue);

// Convenience wrapper for the common "pick a mode from a stream" call.
[[nodiscard]] constexpr ColorMode colorModeFrom(bool supportsColor) {
  return supportsColor ? ColorMode::Ansi : ColorMode::Plain;
}

} // namespace minc::support
