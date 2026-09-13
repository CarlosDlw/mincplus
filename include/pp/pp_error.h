// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// A preprocessor error, as a value.
//
// Same shape as the lexer's token flags and the parser's `ParseError`: this
// stage never reports. `pp_report.h` is the only file in `src/pp` that knows
// about severity and `DiagBag`, so `minc_pp` links no diagnostics and the
// expander can be tested without a `Session` in sight.
//
// Design record: `docs/architectures/preprocessor.md`.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "pp/pp_token.h"
#include "support/span/span.h"

namespace minc::pp {

// The closed set of things that can go wrong. A code is an enumerator with one
// table row rather than a string literal at each call site, so the stage cannot
// invent a code no test knows about, and two sites cannot spell the same
// condition two ways. The safety codes are in here on purpose: hitting a budget
// *is* an error with a name, never a silent truncation and never a hang.
enum class PPErrorCode : std::uint8_t {
  // Directives.
  InvalidDirective,     // `#foo` where nothing is known by that name
  UnknownPragma,        // parsed and preserved; reported only when asked
  NotSupported,         // reserved syntax that is not accepted yet (`#embed`, ...)
  ErrorDirective,       // `#error`
  WarningDirective,     // `#warning`
  InvalidLineDirective, // `#line` with a non-number or a bad file name

  // Conditionals.
  UnterminatedConditional,    // end of file inside a conditional
  UnexpectedConditional,      // `#elif`/`#else`/`#endif` with nothing open
  ElseAfterElse,              // a second `#else` in one conditional
  ConditionalNestingExceeded, // past `support::kMaxConditionalNesting`

  // Macros.
  MacroRedefined,             // same name, a different replacement list
  MacroParameterLimit,        // past `support::kMaxMacroParameters`
  MissingMacroArguments,      // invoked with fewer arguments than parameters
  TooManyMacroArguments,      // invoked with more arguments than parameters
  UnterminatedMacroArguments, // `(` with no matching `)` before end of file
  InvalidPaste,               // `##` whose operands do not make one token
  InvalidHashOperand,         // `#` applied to something that is not a parameter
  MissingMacroName,           // `#define` with no name
  ExpansionDepthExceeded,     // past `support::kMaxExpansionDepth`
  ExpansionBudgetExceeded,    // past `PPOptions::Budgets::expandedTokens`
  ExpressionSyntax,           // `#if` operand that is not a constant expression
  UndefinedIdentifier,        // a name in `#if` that is not a macro (-Wundef)

  // Operators the preprocessor gives no meaning to where they were written:
  // a `#` that does not start a line, or a `##` outside a macro body.
  StrayHashOperator,
  // `_Pragma` whose operand is not one string literal, so there is no pragma to
  // name.
  InvalidPragmaOperand,

  // Includes.
  IncludeNotFound,       // nothing on the search list resolved the path
  IncludeUnreadable,     // the path resolved, but the file's bytes cannot be loaded
  IncludeSelfReference,  // the include chain names this file again
  IncludeDepthExceeded,  // past `support::kMaxIncludeDepth`
  IncludeBudgetExceeded, // past `support::kMaxIncludesPerUnit`
  MissingIncludeGuard,   // read twice with no guard and not elided

  // Output and pipeline.
  PreprocessedBytesExceeded, // past `PPOptions::Budgets::preprocessedBytes`
  TokenTooLong,              // past `support::kMaxTokenBytes`
  DateWithoutEpoch,          // `__DATE__`/`__TIME__` with no `SOURCE_DATE_EPOCH`
};

// Code and its stable short name (`pp-include-not-found`). Keeping them in one
// row means the string a user greps for cannot drift from the enumerator.
struct PPErrorCodeInfo {
  PPErrorCode code;
  const char* name;
};

[[nodiscard]] std::span<const PPErrorCodeInfo> ppErrorCodeInfos();

// Every code, derived from the table, so a code added to the enum without a row
// is caught by the tests.
[[nodiscard]] std::span<const PPErrorCode> allPPErrorCodes();

[[nodiscard]] std::string_view toString(PPErrorCode code);

struct PPError {
  // Points at what the user must fix: the directive, the macro name, the
  // invocation. For a token that came out of a macro, this is the spelling site;
  // the expansion chain is what turns it into "invoked here".
  support::Span span;
  std::string message;
  PPErrorCode code = PPErrorCode::InvalidDirective;
  // Where the expansion that produced the offending token started, when there
  // was one. Rendering walks it as a chain of notes.
  ExpansionId expansion = kNoExpansion;
};

} // namespace minc::pp
