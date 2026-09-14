// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "backend/backend_report.h"

#include <cstddef>
#include <string>

#include "backend/codegen.h"
#include "support/diag/diag_bag.h"
#include "support/span/span.h"

namespace minc::backend {

std::size_t reportBackendDiagnostics(std::span<const CodegenDiagnostic> diagnostics,
                                     support::DiagBag& diags) {
  const std::size_t before = diags.size();
  for (const CodegenDiagnostic& diagnostic : diagnostics) {
    // Every one of these is an error and never a warning, including the
    // environment ones. A missing linker is not a note about the weather: the
    // command the user typed did not produce the artifact they asked for, and a
    // warning would let a build script continue with an executable that is not
    // there.
    //
    // The span is empty for most of them, deliberately: a missing `clang` is not
    // about a line of the program, and pointing a caret at the first byte of the
    // file would be a lie about where the problem is.
    if (diagnostic.span.valid()) {
      diags.error(diagnostic.span, diagnostic.message, std::string(toString(diagnostic.code)));
    } else {
      diags.error(support::Span{}, diagnostic.message, std::string(toString(diagnostic.code)));
    }
  }
  return diags.size() - before;
}

} // namespace minc::backend
