// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "pp/pp_report.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "lex/lex_report.h"
#include "support/limits.h"

namespace minc::pp {
namespace {

// The tokens the preprocessor gives meaning to, which the lexical report must
// therefore not call errors. Position decides whether they are legal, and
// position is this stage's business, not the lexer's.
[[nodiscard]] bool isPreprocessorToken(const lex::Token& token, std::string_view) {
  return lex::isPreprocessorOp(token.kind);
}

// The notes that turn "unknown identifier" into "in expansion of macro 'MAX'".
// The chain is walked innermost first and capped: a diagnostic that renders
// twenty frames of a recursive expansion is a diagnostic nobody reads, and the
// cap is the same one the parser uses for nesting.
void addExpansionNotes(const PPError& error, const ExpansionTable& expansions,
                       const support::Interner* symbols, support::DiagBag& diags) {
  if (error.expansion == kNoExpansion) {
    return;
  }
  std::vector<ExpansionId> chain;
  expansions.chain(error.expansion, chain);
  std::size_t shown = 0;
  for (const ExpansionId id : chain) {
    if (shown >= support::kMaxMacroBacktrace) {
      break;
    }
    const ExpansionFrame& frame = expansions.at(id);
    if (frame.macro == support::kInvalidSym) {
      continue;
    }
    std::string message = "in expansion of macro";
    if (symbols != nullptr) {
      const std::string_view name = symbols->lookup(frame.macro);
      if (!name.empty()) {
        message += " '" + std::string(name) + "'";
      }
    }
    diags.note(frame.invocation.span(), std::move(message));
    ++shown;
  }
}

// "This location is inside a system header."
//
// `begin`-relative rather than containment, unlike the header-name filter: a
// system header stays a system header to the end of the file, and the region a
// `#pragma GCC system_header` opens has no token boundary to stop at.
[[nodiscard]] bool inSystemHeader(const support::Span& span,
                                  std::span<const support::Span> regions) {
  for (const support::Span& region : regions) {
    if (region.file == span.file && span.begin >= region.begin) {
      return true;
    }
  }
  return false;
}

std::size_t report(std::span<const PPError> errors, const ExpansionTable& expansions,
                   const support::Interner* symbols, support::DiagBag& diags, bool warning,
                   std::span<const support::Span> systemRegions) {
  const std::size_t before = diags.size();
  for (const PPError& error : errors) {
    // A warning from a system header is not the user's to fix, and a header they
    // did not write is not somewhere they can fix it. This is the whole reason
    // `isystem` exists as a separate list: `-I /usr/include` says "find it here",
    // `-isystem /usr/include` says "find it here and do not blame me for it".
    // Errors are *not* filtered -- a header that does not parse is broken
    // wherever it lives.
    if (warning && inSystemHeader(error.span, systemRegions)) {
      continue;
    }
    const std::string code = std::string(toString(error.code));
    if (warning) {
      diags.warning(error.span, error.message, code);
    } else {
      diags.error(error.span, error.message, code);
    }
    addExpansionNotes(error, expansions, symbols, diags);
  }
  return diags.size() - before;
}

} // namespace

std::size_t reportPPErrors(std::span<const PPError> errors, const ExpansionTable& expansions,
                           support::DiagBag& diags, const support::Interner* symbols) {
  return report(errors, expansions, symbols, diags, /*warning=*/false, {});
}

std::size_t reportPPWarnings(std::span<const PPError> warnings, const ExpansionTable& expansions,
                             support::DiagBag& diags, const support::Interner* symbols,
                             std::span<const support::Span> systemRegions) {
  return report(warnings, expansions, symbols, diags, /*warning=*/true, systemRegions);
}

std::size_t reportLexedFileErrors(const PPResult& result, support::DiagBag& diags) {
  const std::size_t before = diags.size();
  const lex::LexFilter filter{&isPreprocessorToken, result.headerNames};
  for (const std::shared_ptr<const lex::TokenStream>& stream : result.lexed) {
    if (stream != nullptr) {
      (void)lex::reportLexErrors(*stream, diags, filter);
    }
  }
  return diags.size() - before;
}

} // namespace minc::pp
