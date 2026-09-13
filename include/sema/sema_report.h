// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The typing errors -> `DiagBag`. The only file in `src/sema` that knows about
// severity.
//
// The split is the same one every stage above makes, for the same two reasons:
// `minc_sema` links no diagnostics, so a test can check a unit with no `Session`
// and no terminal and can read the error *values* directly; and one run produces
// every problem, because nothing here stops the walk.
#pragma once

#include <cstddef>
#include <span>

#include "sema/sema_error.h"
#include "support/diag/diag_bag.h"

namespace minc::sema {

// Errors, in the order the checker produced them (source order, because the walk
// is). Returns how many diagnostics were added, which can be fewer than the
// number of errors when the bag's retention cap is reached.
[[nodiscard]] std::size_t reportSemaErrors(std::span<const SemaError> errors,
                                           support::DiagBag& diags);

// The same, at warning severity. Two functions rather than a severity argument:
// the two vectors already carry the distinction, and a call site that has to
// choose is a call site that can choose wrong.
[[nodiscard]] std::size_t reportSemaWarnings(std::span<const SemaError> warnings,
                                             support::DiagBag& diags);

} // namespace minc::sema
