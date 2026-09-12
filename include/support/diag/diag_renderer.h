// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Renders diagnostics as "path:line:col: severity[code]: message" plus a
// source line and a caret underline.
#pragma once

#include <cstdint>
#include <string>

#include "support/diag/diag_bag.h"
#include "support/term/terminal.h"

namespace minc::support {

class SourceManager;

struct RenderOptions {
  ColorMode color = ColorMode::Plain;
  // Tabs are expanded so a caret lands under the right character; a value of
  // 0 is treated as 1.
  std::uint32_t tabWidth = 4;
};

// Pure formatting, no I/O. Unknown files render as "<unknown>:?:?: ".
class DiagRenderer {
public:
  explicit DiagRenderer(const SourceManager* sources, RenderOptions options = {});

  [[nodiscard]] std::string render(const Diagnostic& diag) const;
  [[nodiscard]] std::string renderAll(const DiagBag& bag) const;

private:
  const SourceManager* sources_;
  RenderOptions options_;
};

} // namespace minc::support
