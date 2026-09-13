// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// `#if` arithmetic, over preprocessing integers.
//
// The values are the shared `support::ConstInt` (`intmax_t`/`uintmax_t`, 64
// bits) chosen by the standard's rules, so there is no `int`/`long` question
// here and no operation is undefined: shifts out of range and division by zero
// are *errors with locations*, not UB, because a directive that cannot be
// evaluated is a thing the user has to fix.
//
// The value and its operations live in `support/consteval` and not here, so
// that `#if` and the type checker's constant folding cannot disagree about what
// `1 / 0` does or about what `0755` means. This file is the *grammar* over
// tokens; the arithmetic is shared.
//
// The evaluator is given tokens that have already been macro-expanded, with
// `defined X` and `__has_include(...)` already replaced by integer literals.
// That split is why this file needs no opinion about macros and can be tested
// with a vector of tokens and nothing else.
//
// Design record: `docs/architectures/preprocessor.md`.
#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "pp/pp_error.h"
#include "pp/pp_token.h"
#include "pp/token_text.h"
#include "support/consteval/const_int.h"
#include "support/limits.h"

namespace minc::pp {

struct ConstExprResult {
  bool ok = false;
  support::ConstInt value;
  // The first hard error. Evaluation does not recover: a broken `#if` has one
  // thing to fix, and continuing would bury it under consequences.
  PPError error;
  // Identifiers that were not macros and evaluated to 0. Collected rather than
  // reported so the caller decides whether they are warnings (-Wundef).
  std::vector<PPError> undefinedNames;
};

struct ConstExprOptions {
  // Required: the evaluator reads literal spellings, which only the source can
  // provide.
  const TokenText* text = nullptr;
  // Nesting cap for the expression grammar itself. Reuses the parser's limit:
  // it is the same hazard (a recursive descent over hostile input).
  std::size_t maxDepth = support::kMaxNestingDepth;
};

[[nodiscard]] ConstExprResult evaluateConstExpr(std::span<const PPToken> tokens,
                                                const ConstExprOptions& options);

} // namespace minc::pp
