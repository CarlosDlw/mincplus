// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Human-readable rendering of a typed unit, for `mincc check`.
//
// What it prints is the stage's whole answer, in the three parts a reader asks
// about: the **type table** (what the compilation decided a type is and how wide
// it is), the **typed tree** (every node with the type it was given, so a
// surprising conversion is visible at the node that performed it), and the
// **functions** (the signature the checker committed to before it looked at any
// body).
//
// Pure formatting: a string, never a write, no address and no hash-order
// iteration. The same input produces byte-identical output on every run and
// every platform, which is what makes the mode testable.
#pragma once

#include <cstddef>
#include <string>

#include "ast/ast.h"
#include "sema/type_store.h"
#include "sema/typed_ast.h"

namespace minc::sema {

struct TypedDumpOptions {
  // Longest spelling shown before `...`, in bytes.
  std::size_t maxTextBytes = 32;
  // The type table, once per run rather than once per file.
  bool types = true;
  // The functions the checker typed, in declaration order.
  bool functions = true;
  // One line per node, with its type and, for an expression, its facts.
  bool nodes = true;
};

// `file` supplies the tree and the spellings; `types` is the store every
// `TypeId` in `typed` indexes. They must come from the same compilation.
[[nodiscard]] std::string dumpTypeStore(const TypeStore& types);
[[nodiscard]] std::string dumpTypedFile(const ast::LoweredFile& file, const TypedFile& typed,
                                        const TypeStore& types, TypedDumpOptions options = {});

} // namespace minc::sema
