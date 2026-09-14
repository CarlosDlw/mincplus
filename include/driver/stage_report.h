// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Rendering a *stage's* diagnostics, once, for every command that runs one.
//
// The front end's diagnostics are rendered by the front end itself (`FrontEnd::run`
// owns that, because it is the layer that knows which session the spans belong
// to). The two stages past it -- `ir` and `backend` -- are reached by more than
// one command: `ir` prints modules, `build` and `run` link them. Each of those
// commands would otherwise copy the same eight lines -- build a `DiagBag`, convert
// the stage's records into it, hand it to the renderer -- and two copies are two
// places for a caret to be drawn differently.
//
// A backend diagnostic has no span (`codegen` refuses things about the
// environment and about this compiler, never about a place in the program), and
// that is not a special case here: the renderer already prints a spanless
// diagnostic as a line with a code and a message.
#pragma once

#include <iosfwd>
#include <span>

#include "backend/codegen.h"
#include "ir/ir.h"
#include "support/source/source_manager.h"
#include "support/term/terminal.h"

namespace minc::driver {

// The two sources of diagnostics a stage can produce, so one function serves
// both and a new stage adds one overload rather than one command each.
void renderStageDiagnostics(std::span<const ir::IRDiagnostic> diagnostics,
                            const support::SourceManager& sources, support::ColorMode color,
                            std::ostream& err);

void renderStageDiagnostics(std::span<const backend::CodegenDiagnostic> diagnostics,
                            support::ColorMode color, std::ostream& err);

} // namespace minc::driver
