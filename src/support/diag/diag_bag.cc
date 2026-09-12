// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "support/diag/diag_bag.h"

#include "support/limits.h"

namespace minc::support {

void DiagBag::add(Diagnostic diag) {
  if (diags_.size() >= kMaxDiagnostics) {
    ++dropped_;
    return;
  }
  if (diag.severity == Severity::Error) {
    ++errorCount_;
  } else if (diag.severity == Severity::Warning) {
    ++warningCount_;
  }
  diags_.push_back(std::move(diag));
}

void DiagBag::error(Span span, std::string message, std::string code) {
  add(Diagnostic{Severity::Error, span, std::move(code), std::move(message), {}});
}

void DiagBag::warning(Span span, std::string message, std::string code) {
  add(Diagnostic{Severity::Warning, span, std::move(code), std::move(message), {}});
}

void DiagBag::note(Span span, std::string message) {
  add(Diagnostic{Severity::Note, span, {}, std::move(message), {}});
}

void DiagBag::clear() {
  diags_.clear();
  errorCount_ = 0;
  warningCount_ = 0;
  dropped_ = 0;
}

} // namespace minc::support
