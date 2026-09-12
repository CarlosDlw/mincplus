// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "support/span/span_ops.h"

#include <sstream>

namespace minc::support {

Span merge(Span a, Span b) {
  if (!a.valid() || !b.valid() || a.file != b.file) {
    return Span{};
  }
  const std::uint32_t begin = a.begin < b.begin ? a.begin : b.begin;
  const std::uint32_t end = a.end > b.end ? a.end : b.end;
  return Span(a.file, begin, end);
}

void extendToCover(Span& span, Span other) {
  if (!other.valid()) {
    return;
  }
  if (!span.valid()) {
    span = other;
    return;
  }
  if (span.file != other.file) {
    return;
  }
  span = merge(span, other);
}

std::string toString(Span span) {
  if (!span.valid()) {
    return "<invalid>";
  }
  std::ostringstream out;
  out << "f" << span.file << ":" << span.begin << "-" << span.end;
  return out.str();
}

} // namespace minc::support
