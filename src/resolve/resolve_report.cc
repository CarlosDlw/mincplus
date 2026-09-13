// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "resolve/resolve_report.h"

#include <cstddef>
#include <string>
#include <string_view>

#include "resolve/resolve_error.h"
#include "support/diag/diag_bag.h"

namespace minc::resolve {
namespace {

void reportOne(const ResolveError& error, bool warning, support::DiagBag& diags) {
  const std::string code(toString(error.code));
  if (warning) {
    diags.warning(error.span, error.message, code);
  } else {
    diags.error(error.span, error.message, code);
  }
  if (!error.note.empty()) {
    // The note is anchored where the useful answer is, not where the mistake is:
    // a note on the same span as the error would just repeat it.
    diags.note(error.noteSpan, error.note);
  }
}

} // namespace

std::size_t reportAstErrors(std::span<const ast::AstError> errors, support::DiagBag& diags) {
  const std::size_t before = diags.size();
  for (const ast::AstError& error : errors) {
    diags.error(error.span, error.message, std::string(ast::toString(error.code)));
  }
  return diags.size() - before;
}

std::size_t reportResolveErrors(std::span<const ResolveError> errors, support::DiagBag& diags) {
  const std::size_t before = diags.size();
  for (const ResolveError& error : errors) {
    reportOne(error, /*warning=*/false, diags);
  }
  return diags.size() - before;
}

std::size_t reportResolveWarnings(std::span<const ResolveError> warnings, support::DiagBag& diags) {
  const std::size_t before = diags.size();
  for (const ResolveError& warning : warnings) {
    reportOne(warning, /*warning=*/true, diags);
  }
  return diags.size() - before;
}

} // namespace minc::resolve
