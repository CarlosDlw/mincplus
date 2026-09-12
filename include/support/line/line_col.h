// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace minc::support {

struct LineCol {
  std::uint32_t line = 1;
  std::uint32_t col = 1;
};

} // namespace minc::support
