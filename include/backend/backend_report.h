// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Codegen diagnostics -> the diagnostic bag.
//
// The last of the stage-report targets, and it exists for the same reason as
// every other one: a consumer that only wants an object never links `DiagBag`,
// and the single place that decides how a `CodegenDiagnostic` reads is one file.
#pragma once

#include <cstddef>
#include <span>

#include "backend/codegen.h"
#include "support/diag/diag_bag.h"

namespace minc::backend {

// Appends one diagnostic per refusal and returns how many were added.
[[nodiscard]] std::size_t reportBackendDiagnostics(std::span<const CodegenDiagnostic> diagnostics,
                                                   support::DiagBag& diags);

} // namespace minc::backend
