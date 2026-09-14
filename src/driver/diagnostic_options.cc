// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "driver/diagnostic_options.h"

namespace minc::driver {

support::RenderOptions diagnosticOptions(support::ColorMode color, std::size_t errorLimit) {
  support::RenderOptions options;
  options.color = color;
  options.tabWidth = kDiagnosticTabWidth;
  options.errorLimit = errorLimit;
  return options;
}

} // namespace minc::driver
