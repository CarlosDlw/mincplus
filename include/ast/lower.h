// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Lowering: the syntax tree becomes the analysis tree.
//
// A mechanical mapping and nothing else. Trivia is dropped, every identifier is
// interned once, four identifiers collapse into one `Name` node's `SymId`, and
// each node keeps both the range it occupies in the translation unit's text and
// the span it was *written* at. No name is looked up, no type is guessed, no
// expression is folded: every decision here is one the source already made.
//
// Design record: `docs/architectures/resolve.md`, decisions 1-8.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ast/ast.h"
#include "ast/ast_error.h"
#include "lex/token_stream.h"
#include "support/intern/interner.h"
#include "support/span/span.h"
#include "syntax/tree.h"

namespace minc::ast {

// Where the bytes a node covers were *written*.
//
// The tree is built over the preprocessed text, where a token that came out of a
// header or a macro body sits next to tokens from other files. The stream that
// produced it knows each token's written span, so it is the translation between
// the two and it is kept here rather than re-derived. A null `stream` means the
// text is its own source: every origin equals the unit range, which is the case
// for a file with no includes and no macros.
struct OriginTable {
  const lex::TokenStream* stream = nullptr;

  [[nodiscard]] support::Span originOf(support::FileId unit, std::uint32_t begin,
                                       std::uint32_t end) const;
};

// The bounds lowering obeys. Same rule as the preprocessor's budgets: they can
// be lowered for a constrained environment or for a test, never disabled, and
// the check happens before the allocation so hitting one is a diagnostic and not
// a kill.
struct LowerLimits {
  std::size_t maxNodes = support::kMaxAstNodesPerUnit;
};

struct LowerOutput {
  LoweredFile file;
  // Empty for input the language accepts. A non-empty list always means the
  // lowering stopped early; there is no warning severity at this layer.
  std::vector<AstError> errors;
};

// Lowers one translation unit. `origins` may be defaulted, and `symbols` is the
// session's interner, because a `SymId` from one interner means nothing in
// another.
[[nodiscard]] LowerOutput lowerFile(const syntax::SyntaxTree& tree, support::Interner& symbols,
                                    const OriginTable& origins = {}, LowerLimits limits = {});

} // namespace minc::ast
