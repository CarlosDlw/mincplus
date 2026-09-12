// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The seam between "a token" and "the text of a token".
//
// Two parts of this stage need that text and cannot get it from the token:
// `#`/`##` (which re-spell tokens that may themselves be synthesized) and the
// `#if` evaluator (which must read a literal's digits). Both are pure functions
// of text, so both are testable against a three-line fake instead of a whole
// preprocessor.
//
// The three operations are exactly the three the preprocessor can do that a
// token cannot do for itself:
//
//   * `spelling`  -- read the token's bytes, from the source or from scratch;
//   * `addScratch` -- give a synthesized spelling a home, returning an index;
//   * `relex`     -- re-tokenize a spelling with the same lexer the file went
//                    through, which is what makes `##` produce real tokens
//                    instead of a second, subtly different tokenizer.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "pp/pp_token.h"

namespace minc::pp {

class TokenText {
public:
  TokenText() = default;
  TokenText(const TokenText&) = delete;
  TokenText& operator=(const TokenText&) = delete;
  virtual ~TokenText() = default;

  // The token's spelling. For every token that was written down this is a view
  // into source text and stays valid while the source is loaded; for a
  // synthesized token it is a view into the scratch buffer. Empty for a
  // placemarker.
  [[nodiscard]] virtual std::string_view spelling(const PPToken& token) const = 0;

  // Stores a synthesized spelling and returns the index to put in
  // `PPToken::scratch`. The returned view stays valid for the preprocessor's
  // lifetime.
  [[nodiscard]] virtual std::uint32_t addScratch(std::string text) = 0;

  // Lexes `text` as exactly one preprocessing token. False when it is not
  // exactly one token -- two tokens, or none -- which is the diagnostic for an
  // invalid paste. On success `out` points at scratch text, not at a file.
  [[nodiscard]] virtual bool relex(std::string_view text, PPToken& out) = 0;
};

} // namespace minc::pp
