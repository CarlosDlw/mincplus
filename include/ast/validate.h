// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Structural validation: the shapes the parser accepted that the language
// forbids, checked without a symbol table.
//
// A pass of its own rather than code inside the lowerer, for the reason rustc
// gives -- its `AstValidator` runs after lowering and before resolution,
// "doesn't perform any complex analysis, type checking or name resolution", and
// is described as language surface that grows with the language while lowering
// stays a mechanical mapping. This runs on the lowered AST, so it never sees
// trivia and never has to reason about `Error`-node nesting.
//
// One rule keeps it honest: a region the parser already reported is not reported
// again. Every check below skips `inError` nodes. Two diagnostics for one
// mistake is worse than one.
//
// Design record: `docs/architectures/resolve.md`, layer 2.
#pragma once

#include <vector>

#include "ast/ast.h"
#include "ast/ast_error.h"

namespace minc::ast {

// Returns the structural errors, in source order (the node array is in source
// order, so a linear scan is enough -- no second walk and no sorting).
[[nodiscard]] std::vector<AstError> validate(const LoweredFile& file);

} // namespace minc::ast
