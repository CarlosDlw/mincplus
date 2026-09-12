// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace minc::support {

enum class Severity : std::uint8_t { Note, Warning, Error };

[[nodiscard]] const char* toString(Severity severity);

} // namespace minc::support
