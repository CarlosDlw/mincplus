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

std::size_t report(std::span<const PPError> errors, const ExpansionTable& expansions,
                   const support::Interner* symbols, support::DiagBag& diags, bool warning) {
  const std::size_t before = diags.size();
  for (const PPError& error : errors) {
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
  return report(errors, expansions, symbols, diags, /*warning=*/false);
}

std::size_t reportPPWarnings(std::span<const PPError> warnings, const ExpansionTable& expansions,
                             support::DiagBag& diags, const support::Interner* symbols) {
  return report(warnings, expansions, symbols, diags, /*warning=*/true);
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
