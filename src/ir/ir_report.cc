// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "ir/ir_report.h"

#include <cstddef>
#include <string>

#include "ir/ir.h"
#include "support/diag/diag_bag.h"

namespace minc::ir {

std::size_t reportIRDiagnostics(std::span<const IRDiagnostic> diagnostics,
                                support::DiagBag& diags) {
  const std::size_t before = diags.size();
  for (const IRDiagnostic& diagnostic : diagnostics) {
    // Every refusal here is an error and never a warning. Even the two
    // "not built yet" codes are errors: a construct the language has and the
    // backend does not lower is a program this compiler cannot compile, and
    // saying so is the honest answer -- a warning would let a build script
    // produce an object file with a hole in it.
    diags.error(diagnostic.span, diagnostic.message, std::string(toString(diagnostic.code)));
  }
  return diags.size() - before;
}

} // namespace minc::ir
