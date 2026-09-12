// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Human-readable rendering of a syntax tree, for `mincc parse`.
//
// Pure formatting, no I/O: it returns a string and never writes, so a test can
// pin the exact output and the driver decides where it goes. Like the token
// dump it prints kinds, offsets, and lexemes -- never an address -- so the same
// input produces byte-identical output on every run and every platform, which
// is what lets the golden files be compared.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "support/term/terminal.h"
#include "syntax/tree.h"

namespace minc::syntax {

struct DumpOptions {
  // Ansi adds SGR colors. Alignment is computed on the uncolored text, so
  // turning color on never shifts a column.
  support::ColorMode color = support::ColorMode::Plain;
  // Trivia is the bulk of a tree and never what a grammar test is looking at.
  bool showTrivia = true;
  // Longest lexeme shown before `...`, in bytes of the input, so a multi-byte
  // escape is never cut in half.
  std::size_t maxTextBytes = 40;
  // Stop descending past this depth. 0 means no limit.
  std::uint32_t maxDepth = 0;
};

// `path` only appears in the header line; the tree knows its FileId and the
// path lives in the SourceFile it came from.
[[nodiscard]] std::string dumpTree(const SyntaxTree& tree, std::string_view path,
                                   DumpOptions options = {});

} // namespace minc::syntax
