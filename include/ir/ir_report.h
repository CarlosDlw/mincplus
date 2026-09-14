// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// IR diagnostics -> the diagnostic bag.
//
// The last of the stage-report targets, and it exists for the same reason as the
// other five: a consumer that only wants a module never links `DiagBag`, and the
// one place that decides how an `IRDiagnostic` reads is a single file.
#pragma once

#include <cstddef>
#include <span>

#include "ir/ir.h"
#include "support/diag/diag_bag.h"

namespace minc::ir {

// Appends one diagnostic per refusal and returns how many were added.
[[nodiscard]] std::size_t reportIRDiagnostics(std::span<const IRDiagnostic> diagnostics,
                                              support::DiagBag& diags);

} // namespace minc::ir
