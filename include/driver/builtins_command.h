// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "driver/cli.h"

namespace minc::driver {

// `mincc builtins`: the table, rendered.
//
// The only command that reads no file: what it prints is the compiler's own list
// of names, which is exactly why it is worth having -- a language reference that
// drifts from the compiler is a reference nobody can trust, and this is the same
// data the checker and the lowering read (`builtins/dump.h`).
[[nodiscard]] int runBuiltins(const CliOptions& options);

} // namespace minc::driver
