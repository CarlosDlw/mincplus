// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace minc::support {

using SymId = std::uint32_t;
inline constexpr SymId kInvalidSym = static_cast<SymId>(-1);

} // namespace minc::support
