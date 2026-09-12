// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "support/diag/severity.h"

namespace minc::support {

const char* toString(Severity severity) {
  switch (severity) {
  case Severity::Note:
    return "note";
  case Severity::Warning:
    return "warning";
  case Severity::Error:
    return "error";
  }
  return "unknown";
}

} // namespace minc::support
