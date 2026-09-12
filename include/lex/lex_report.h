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

#include "lex/token_stream.h"
#include "support/diag/diag_bag.h"

namespace minc::lex {

// Adds one error per flagged token and one per `Invalid` byte, with the
// token's span attached. Never prints. Returns how many diagnostics were added
// (which may be less than the number of problems if the bag hit its
// retention cap).
[[nodiscard]] std::size_t reportLexErrors(const TokenStream& stream, support::DiagBag& diags);

} // namespace minc::lex
