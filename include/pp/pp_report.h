// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Turns preprocessor errors into diagnostics.
//
// The only file in `src/pp` that knows about `DiagBag` and severity, for the
// same reason the lexer and the parser have the same split: the stage reports
// errors as values, so it can be tested without a diagnostic bag and one run can
// report *every* error instead of stopping at the first.
//
// The expansion chain is rendered here because this is the only layer that has
// both the table and the bag. A token that came out of `MAX(1, 2)` should read
// as "in expansion of macro 'MAX'", with the invocation as a note -- that is the
// whole point of keeping provenance as a separate field instead of compressing
// it into one location.
#pragma once

#include <cstddef>
#include <span>

#include "pp/expansion_table.h"
#include "pp/pp_error.h"
#include "pp/preprocessor.h"
#include "support/diag/diag_bag.h"
#include "support/intern/interner.h"

namespace minc::pp {

// Reports the lexical problems of every file the run read, in read order.
//
// The `#` of a directive is skipped: the lexer has no `#` kind on purpose
// (`lexer.md`, decision 13), so on this path those bytes belong to the
// preprocessor and reporting them as an invalid character would blame the stage
// that is not wrong. Nothing else is filtered, so an invalid byte in a header is
// an error like it is in the main file -- which is the only way a reader learns
// which header is broken.
[[nodiscard]] std::size_t reportLexedFileErrors(const PPResult& result, support::DiagBag& diags);

// Adds one error diagnostic per error, preserving order, each followed by a note
// per expansion frame that produced it. `symbols` is the interner the frames'
// macro ids belong to, or null when the caller cannot provide one -- the notes
// then say "in expansion of macro" without the name, which is still better than
// nothing.
//
// Returns how many diagnostics were added, which can be fewer than the number of
// errors when the bag's retention cap is reached.
[[nodiscard]] std::size_t reportPPErrors(std::span<const PPError> errors,
                                         const ExpansionTable& expansions, support::DiagBag& diags,
                                         const support::Interner* symbols = nullptr);

// The same, at warning severity. Kept separate rather than a severity argument
// because the preprocessor's two output vectors already carry the distinction,
// and a call site that has to choose invites a wrong choice.
[[nodiscard]] std::size_t reportPPWarnings(std::span<const PPError> warnings,
                                           const ExpansionTable& expansions,
                                           support::DiagBag& diags,
                                           const support::Interner* symbols = nullptr);

} // namespace minc::pp
