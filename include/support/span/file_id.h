// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace minc::support {

using FileId = std::uint32_t;
inline constexpr FileId kInvalidFile = static_cast<FileId>(-1);

} // namespace minc::support
