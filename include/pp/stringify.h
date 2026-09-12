// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The two places a token is *synthesized*: `#` and `##`.
//
// Both are pure functions of text, so both are here instead of inside the
// expander, and both take a `TokenText` rather than a preprocessor: the tests
// drive them with a fake, and the expander drives them with the real one.
//
// The rules implemented here are the ones that are easy to get subtly wrong:
//
//   * `#` reads the argument's **unexpanded** tokens and re-spells them from
//     source, so `1e10`, `0x1F` and `'x'` stringify as written rather than as
//     reconstructed-from-the-kind. One space is inserted exactly where the
//     original had whitespace.
//   * `##` concatenates two spellings and **re-lexes** with the same `lexOne`
//     the file went through. An empty operand becomes a *placemarker* so the
//     operator still has two operands, and a paste that does not yield exactly
//     one token is a diagnostic rather than undefined behaviour.
//
// Design record: `docs/architectures/preprocessor.md`.
#pragma once

#include <span>
#include <string>

#include "pp/pp_token.h"
#include "pp/token_text.h"

namespace minc::pp {

// The spelling of a string literal that results from `#argument`, quotes
// included, with `"` and `\` inside the argument escaped as the standard
// requires. Never empty: the empty argument stringifies to `""`.
[[nodiscard]] std::string stringifyTokens(const TokenText& text, std::span<const PPToken> tokens);

// Pastes `left` and `right` into `out`, which becomes a scratch-spelled token
// whose kind comes from re-lexing the concatenation. Returns false when the
// result is not exactly one preprocessing token, in which case `out` is left
// alone and the caller reports the two spellings.
//
// `left` or `right` being a placemarker is not a failure: the result is the
// other operand, which is what makes `#define CAT(a, b) a ## b` work when one
// argument is empty.
[[nodiscard]] bool pasteTokens(TokenText& text, const PPToken& left, const PPToken& right,
                               PPToken& out);

} // namespace minc::pp
