// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Turns the parser's events into the green tree.
//
// This is the only component that sees trivia. The parser is trivia-blind: when
// it emits a token it means "the next significant token", and the builder
// flushes the whitespace and comments in between, into the node that is
// currently open, before emitting it. That single rule is what keeps the
// grammar free of whitespace while the tree stays lossless.
//
// It also resolves `forwardParent`: a node that a later node adopts as its
// child is entered *before* that later node when the builder reaches it, so a
// left-associative chain is nested correctly without moving anything.
#pragma once

#include <optional>
#include <vector>

#include "lex/token_stream.h"
#include "parse/event.h"
#include "support/mem/arena.h"
#include "syntax/green.h"
#include "syntax/tree.h"

namespace minc::syntax {

struct BuildResult {
  const GreenNode* root = nullptr;
  TreeStats stats;
};

// Builds a green tree from `events` and the token stream they describe.
//
// `events` is consumed (forward-parent targets are tombstoned as they are
// used), which is why it is taken by mutable reference. Returns nullopt only
// when the arena is exhausted; the tree is otherwise always total, because
// every byte of the stream ends up under some leaf.
[[nodiscard]] std::optional<BuildResult> buildGreenTree(support::Arena& arena,
                                                        std::vector<parse::Event>& events,
                                                        const lex::TokenStream& stream);

// The whole front end for one file: parse `stream` and build its tree. `arena`
// must outlive the returned tree.
[[nodiscard]] std::optional<SyntaxTree>
buildSyntaxTree(support::Arena& arena, const lex::TokenStream& stream, std::uint32_t revision);

} // namespace minc::syntax
