// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The raw lexer.
//
// `lexOne` is a *pure total function* of (text, offset): no state, no
// allocation, no SourceManager, no Interner, no DiagBag. That is deliberate:
//
//   * it is fuzzable with a three-line harness and testable with no context;
//   * it can be re-run on any suffix of a file, which is what incremental
//     re-lexing for the language server needs.
//
// The usual reason a lexer needs resumable state is trivia: a lexer that
// *skips* comments must remember it is inside one when it restarts. Here
// trivia is emitted as ordinary tokens, comments are scanned through their
// terminator, and a string or char literal can never cross a line. Every
// token boundary is therefore also a valid restart point, and no state has to
// be threaded through at all.
//
// Changing any of those rules (multi-line strings, line splicing, nesting
// block comments) is exactly when a state parameter must come back; the
// signature is shaped so adding one is a mechanical change.
#pragma once

#include <cstdint>
#include <string_view>

#include "lex/token.h"

namespace minc::lex {

// Precondition: `offset <= text.size()`.
//
// Returns EndOfFile with length 0 when `offset == text.size()`. Otherwise the
// returned token has `length >= 1`, so a caller looping from 0 always makes
// progress and always terminates exactly at the end of the input.
[[nodiscard]] Token lexOne(std::string_view text, std::uint32_t offset);

} // namespace minc::lex
