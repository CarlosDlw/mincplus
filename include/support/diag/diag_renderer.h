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
//
// `renderAll` is the form every command uses, and it is not `render` in a loop:
// a note that lands on exactly the span of the diagnostic above it has no
// excerpt to add -- the source line and the carets would repeat verbatim, which
// reads as two diagnostics for one mistake. That one is elided. A note that
// points somewhere else, like a backtrace note at the macro's invocation, keeps
// its excerpt, because there the second excerpt is new information. That is what
// the market's compilers do, and it is why the rule lives on the sequence: which
// excerpt is redundant is not a property of a note on its own.
class DiagRenderer {
public:
  explicit DiagRenderer(const SourceManager* sources, RenderOptions options = {});

  // One diagnostic, on its own: always with its excerpt.
  [[nodiscard]] std::string render(const Diagnostic& diag) const;
  // A sequence, with the repetition above folded away.
  [[nodiscard]] std::string renderAll(const DiagBag& bag) const;

private:
  [[nodiscard]] std::string render(const Diagnostic& diag, bool withSnippet) const;

  const SourceManager* sources_;
  RenderOptions options_;
};

} // namespace minc::support
