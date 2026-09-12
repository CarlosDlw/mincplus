// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Collects diagnostics during lex/parse/sema. Never throws, never prints.
//
// Retention is capped at kMaxDiagnostics so a cascading error in a large file
// cannot exhaust memory. Once the cap is hit, further diagnostics are dropped
// and counted; renderAll() reports how many were suppressed.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "support/diag/diagnostic.h"

namespace minc::support {

class DiagBag {
public:
  DiagBag() = default;

  void add(Diagnostic diag);
  void error(Span span, std::string message, std::string code = "");
  void warning(Span span, std::string message, std::string code = "");
  void note(Span span, std::string message);

  [[nodiscard]] bool hasErrors() const {
    return errorCount_ > 0;
  }
  [[nodiscard]] bool empty() const {
    return diags_.empty();
  }
  [[nodiscard]] std::size_t size() const {
    return diags_.size();
  }
  [[nodiscard]] std::size_t errorCount() const {
    return errorCount_;
  }
  [[nodiscard]] std::size_t warningCount() const {
    return warningCount_;
  }
  // Diagnostics dropped because the retention cap was reached.
  [[nodiscard]] std::size_t droppedCount() const {
    return dropped_;
  }

  [[nodiscard]] const std::vector<Diagnostic>& all() const {
    return diags_;
  }
  void clear();

private:
  std::vector<Diagnostic> diags_;
  std::size_t errorCount_ = 0;
  std::size_t warningCount_ = 0;
  std::size_t dropped_ = 0;
};

} // namespace minc::support
