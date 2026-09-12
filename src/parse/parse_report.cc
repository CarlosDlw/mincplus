// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "parse/parse_report.h"

#include <cstddef>
#include <string>
#include <utility>

namespace minc::parse {

std::size_t reportParseErrors(std::span<const ParseError> errors, support::DiagBag& diags) {
  const std::size_t before = diags.size();
  for (const ParseError& error : errors) {
    // The span is carried through unchanged so the caret lands where the parser
    // says it should -- including a zero-width span for an inserted token,
    // which is where the user has to type.
    diags.error(error.span, std::string(error.message), std::string(error.codeName()));
  }
  return diags.size() - before;
}

} // namespace minc::parse
