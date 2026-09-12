// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The seam the design promised: the parser reads the preprocessor's output.
//
// `parse::TokenSource` exists precisely so this can be a second implementation
// instead of a rewrite of the grammar, and this file is the whole of it. The
// preprocessed stream is already significant-token-only and already ordered, so
// the adapter is an index and a clamp.
//
// It lives in its own target (`minc_pp_parse`) rather than in `minc_pp`, because
// the preprocessor itself must not link the parser: it has no idea a parser
// exists, and keeping that true is what lets the preprocessor be reused by the
// language server, the `pp` command and any future tool without dragging the
// grammar in.
#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include "parse/token_source.h"
#include "pp/pp_token.h"

namespace minc::pp {

// A `TokenSource` over the preprocessed tokens. `tokens` must outlive the source;
// in the normal flow that is the `PPResult` the driver holds.
//
// Trivia is skipped here, exactly as `TokenStreamSource` skips it over a lexed
// file: `PPResult::tokens` carries whitespace (the tree builder needs it), and
// the grammar must never see it. So this source always answers with the next
// *significant* token, and `spanOfCurrent` reports where that token was written
// -- which can be a header, since provenance survives expansion.
[[nodiscard]] std::unique_ptr<parse::TokenSource>
makePPTokenSource(std::span<const PPToken> tokens);

} // namespace minc::pp
