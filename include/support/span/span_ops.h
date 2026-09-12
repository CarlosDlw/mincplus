// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Free operations over Span (merge, extend, print).
#pragma once

#include <string>

#include "support/span/span.h"

namespace minc::support {

// Bounding range of two same-file spans; invalid when files differ/invalid.
[[nodiscard]] Span merge(Span a, Span b);

// Grow `span` to cover `other` (same file only). No-op on invalid input.
void extendToCover(Span& span, Span other);

std::string toString(Span span);

} // namespace minc::support
