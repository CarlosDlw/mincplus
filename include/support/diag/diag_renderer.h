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

#include <cstddef>

#include "support/limits.h"

struct RenderOptions {
  ColorMode color = ColorMode::Plain;
  // Tabs are expanded so a caret lands under the right character; a value of
  // 0 is treated as 1.
  std::uint32_t tabWidth = 4;
  // `-ferror-limit`: how many errors are *shown* before rendering stops, with
  // the count of what was left out reported below the last one.
  //
  // A display bound, and deliberately not a compile bound. The compiler has
  // already done its work by the time anything is rendered -- every unit still
  // gets its tables and its summary, so `--stats` and `--ast` stay meaningful --
  // and the hazard this closes is the one rendering always carries: a single
  // mistake in a macro body is a thousand diagnostics, each with an excerpt.
  //
  // `kMaxDiagnostics` (the retention cap) is the default and therefore means
  // "everything that was kept". `0` from the command line is stored as that same
  // value, which is what makes it mean "no limit": the bag cannot hold more.
  std::size_t errorLimit = kMaxDiagnostics;
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
  // The same, spending an error limit that outlives one bag.
  //
  // `shownErrors` is in and out: a caller that renders one bag per input -- which
  // is what the front end does, because the bag is cleared per translation unit
  // -- passes the same counter every time, so `-ferror-limit=3` means three
  // errors on the command line and not three per file. State nobody has to
  // remember is passed in rather than kept in the renderer, which stays a pure
  // formatting object.
  [[nodiscard]] std::string renderAll(const DiagBag& bag, std::size_t& shownErrors) const;

private:
  [[nodiscard]] std::string render(const Diagnostic& diag, bool withSnippet) const;

  const SourceManager* sources_;
  RenderOptions options_;
};

} // namespace minc::support
