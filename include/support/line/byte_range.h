// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace minc::support {

struct ByteRange {
  std::uint32_t begin = 0;
  std::uint32_t end = 0;
};

} // namespace minc::support
