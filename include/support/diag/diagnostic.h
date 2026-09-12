// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Diagnostic value types. Collection lives in diag_bag.h, text in diag_renderer.h.
#pragma once

#include <string>
#include <vector>

#include "support/diag/severity.h"
#include "support/span/span.h"

namespace minc::support {

// Attached note with its own location (e.g. "first defined here").
struct DiagNote {
  Span span;
  std::string message;
};

struct Diagnostic {
  Severity severity = Severity::Error;
  Span span;
  std::string code;
  std::string message;
  std::vector<DiagNote> notes;
};

} // namespace minc::support
