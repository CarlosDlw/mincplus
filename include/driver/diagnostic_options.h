// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// How a command's diagnostics are rendered, decided in one place.
//
// Three facts go into a rendering, and each comes from somewhere different: the
// color is a property of the stream the diagnostics go to, the tab width is a
// driver convention, and the error limit is what the user asked for with
// `-ferror-limit`. Assembling them at each call site would be six chances to
// disagree -- and the disagreement is visible: two commands expanding a tab a
// different number of columns draw their carets under different characters, in
// the same file, for the same diagnostic.
//
// This is a small file on purpose. It is the one dependency the front-end
// commands need to render anything, and putting it in `stage_report.h` would pull
// `ir` and `backend` into `mincc lex`.
#pragma once

#include <cstddef>

#include "support/diag/diag_renderer.h"
#include "support/term/terminal.h"

namespace minc::driver {

// The tab width every diagnostic in this compiler is rendered with. Four, and a
// constant rather than a literal because a caret's column is arithmetic on it.
inline constexpr std::uint32_t kDiagnosticTabWidth = 4;

[[nodiscard]] support::RenderOptions diagnosticOptions(support::ColorMode color,
                                                       std::size_t errorLimit);

} // namespace minc::driver
