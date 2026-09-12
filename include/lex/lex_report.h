// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Turns lexical problems into diagnostics.
//
// This is the only place in the lex module that knows about `Span`, severity,
// and `DiagBag`; `lexOne` reports problems as flags on the token instead, which
// is what lets one pass produce *every* lexical error rather than stopping at
// the first. Reporting is a separate step so a tool that only wants tokens
// (the dump, a highlighter, a test) pays nothing for it.
#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "lex/token_stream.h"
#include "support/diag/diag_bag.h"
#include "support/span/span.h"

namespace minc::lex {

// "These tokens are not mine to report." A caller that *claims* some of the
// file filters here instead of re-deriving the mapping, so the table below stays
// the only place a lexical problem becomes a diagnostic.
using TokenSkip = bool (*)(const Token& token, std::string_view text);

// What a caller claims of a file it handed to the lexer.
//
// Two kinds, because there are two ways bytes stop being the lexer's business:
// a whole token whose *kind* carries its own rules (`skip`), and a byte range
// that spans several tokens and is read under different rules altogether
// (`claimed` -- a header-name, where `//` is not a comment and an escape is not
// an escape, so the tokens the plain lexer made of it are not wrong, just not
// the reading that applies).
struct LexFilter {
  TokenSkip skip = nullptr;
  std::span<const support::Span> claimed;
};

// Adds one error per flagged token and one per `Invalid` byte, with the
// token's span attached. Never prints. Returns how many diagnostics were added
// (which may be less than the number of problems if the bag hit its
// retention cap).
[[nodiscard]] std::size_t reportLexErrors(const TokenStream& stream, support::DiagBag& diags);

// The same, skipping whatever `filter` claims. Both halves of the filter are
// consulted for one token before any of its problems are reported, so a claimed
// token is silent whole rather than partially.
[[nodiscard]] std::size_t reportLexErrors(const TokenStream& stream, support::DiagBag& diags,
                                          const LexFilter& filter);

} // namespace minc::lex
