// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Both error tables -> `DiagBag`. The only file in `src/ast` or `src/resolve`
// that knows about severity.
//
// Two stages, two tables, one converter each, for the same reason every other
// stage keeps the split: `minc_ast` and `minc_resolve` link no diagnostics, so
// their checks can be tested with no `Session` and no terminal, and one run can
// produce *every* problem instead of stopping at the first.
#pragma once

#include <cstddef>
#include <span>

#include "ast/ast_error.h"
#include "resolve/resolve_error.h"
#include "support/diag/diag_bag.h"

namespace minc::resolve {

// Structural errors, in order. Returns how many diagnostics were added, which
// can be fewer than the number of errors when the bag's retention cap is hit.
[[nodiscard]] std::size_t reportAstErrors(std::span<const ast::AstError> errors,
                                          support::DiagBag& diags);

// Resolution errors. A `note` on the error becomes a note diagnostic anchored at
// `noteSpan`, so "the name is not a value, but a tag exists" is one error with a
// pointer rather than two errors.
[[nodiscard]] std::size_t reportResolveErrors(std::span<const ResolveError> errors,
                                              support::DiagBag& diags);

// The same, at warning severity. Kept separate rather than a severity argument,
// because the resolver's two vectors already carry the distinction and a call
// site that has to choose invites a wrong choice.
[[nodiscard]] std::size_t reportResolveWarnings(std::span<const ResolveError> warnings,
                                                support::DiagBag& diags);

} // namespace minc::resolve
