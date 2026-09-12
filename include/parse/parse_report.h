// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Turns syntax errors into diagnostics.
//
// The only place in the parse module that knows about `Span`, severity, and
// `DiagBag`; the parser reports errors as values instead, which is what lets one
// run produce *every* syntax error rather than stopping at the first, and what
// keeps the grammar testable with no diagnostic machinery linked at all.
#pragma once

#include <cstddef>
#include <span>

#include "parse/parse_error.h"
#include "support/diag/diag_bag.h"

namespace minc::parse {

// Adds one diagnostic per error, preserving order. Never prints. Returns how
// many were added, which can be fewer than the number of errors when the bag's
// retention cap is reached.
[[nodiscard]] std::size_t reportParseErrors(std::span<const ParseError> errors,
                                            support::DiagBag& diags);

} // namespace minc::parse
