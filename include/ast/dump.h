// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Human-readable rendering of the lowered AST, for `mincc resolve --ast`.
//
// Pure formatting: it returns a string and never writes, so a test can pin the
// exact output and the driver decides where it goes. Like the token and tree
// dumps it prints kinds and ranges and never an address, so the same input
// produces byte-identical output on every run and every platform.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "ast/ast.h"

namespace minc::ast {

struct AstDumpOptions {
  // Longest spelling shown before `...`, in bytes, so a multi-byte escape is
  // never cut in half.
  std::size_t maxTextBytes = 32;
  // Print the item tree instead of the node tree.
  bool items = false;
};

// The node tree, one line per node, in the order the node array holds them.
[[nodiscard]] std::string dumpAst(const LoweredFile& file, AstDumpOptions options = {});

} // namespace minc::ast
