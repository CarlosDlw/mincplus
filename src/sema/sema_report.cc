// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "sema/sema_report.h"

#include <cstddef>
#include <string>

#include "sema/sema_error.h"
#include "support/diag/diag_bag.h"

namespace minc::sema {
namespace {

void reportOne(const SemaError& error, bool warning, support::DiagBag& diags) {
  const std::string code(toString(error.code));
  if (warning) {
    diags.warning(error.span, error.message, code);
  } else {
    diags.error(error.span, error.message, code);
  }
  if (!error.note.empty()) {
    // Anchored where the answer is, not where the mistake is: a note on the same
    // span as the error would only repeat it.
    diags.note(error.noteSpan.valid() ? error.noteSpan : error.span, error.note);
  }
}

} // namespace

std::size_t reportSemaErrors(std::span<const SemaError> errors, support::DiagBag& diags) {
  const std::size_t before = diags.size();
  for (const SemaError& error : errors) {
    reportOne(error, /*warning=*/false, diags);
  }
  return diags.size() - before;
}

std::size_t reportSemaWarnings(std::span<const SemaError> warnings, support::DiagBag& diags) {
  const std::size_t before = diags.size();
  for (const SemaError& warning : warnings) {
    reportOne(warning, /*warning=*/true, diags);
  }
  return diags.size() - before;
}

} // namespace minc::sema
