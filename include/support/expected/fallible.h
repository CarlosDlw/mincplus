// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include <string>

#include "support/expected/expected.h"

namespace minc::support {

template <typename T> using Fallible = Expected<T, std::string>;

} // namespace minc::support
