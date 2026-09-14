// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "driver/stage_report.h"

#include <ostream>

#include "backend/backend_report.h"
#include "ir/ir_report.h"
#include "support/diag/diag_bag.h"
#include "support/diag/diag_renderer.h"

namespace minc::driver {

void renderStageDiagnostics(std::span<const ir::IRDiagnostic> diagnostics,
                            const support::SourceManager& sources,
                            const support::RenderOptions& options, std::ostream& err) {
  if (diagnostics.empty()) {
    return;
  }
  support::DiagBag bag;
  (void)ir::reportIRDiagnostics(diagnostics, bag);
  const support::DiagRenderer renderer(&sources, options);
  err << renderer.renderAll(bag);
  err.flush();
}

void renderStageDiagnostics(std::span<const backend::CodegenDiagnostic> diagnostics,
                            const support::RenderOptions& options, std::ostream& err) {
  if (diagnostics.empty()) {
    return;
  }
  support::DiagBag bag;
  (void)backend::reportBackendDiagnostics(diagnostics, bag);
  // No source manager: a backend diagnostic is about the environment or about
  // this compiler, so it has no span to point at and nothing to look up.
  const support::DiagRenderer renderer(nullptr, options);
  err << renderer.renderAll(bag);
  err.flush();
}

} // namespace minc::driver
