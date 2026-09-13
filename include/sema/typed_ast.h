// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The typed AST: what `sema` returns.
//
// It is a **parallel array beside** the lowered tree, not a `type` field inside
// its nodes -- the same decision `resolve` made for its `NameRef`s, for the same
// three reasons. The lowered AST stays a pure value, so it stays hashable and
// the item-tree cache keeps working. `src/ast` never depends on the type
// language. And a revision's types can be dropped without touching the tree.
//
// Two properties make it pleasant to consume:
//
//   * **total on expressions.** `types[e]` is a real type or the poison; there is
//     no "sema did not get here", so an IR builder never has to handle a missing
//     answer.
//   * **idempotent.** Nothing here is consumed on read. A later stage may ask
//     the same question as often as it likes, which is what makes the checker
//     re-queryable rather than a one-shot pass.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ast/node.h"
#include "sema/type.h"
#include "support/consteval/const_int.h"
#include "support/span/span.h"

namespace minc::sema {

// What an expression is, beyond its type. Computed once, read by everyone.
struct ExprInfo {
  // A *modifiable* lvalue is an lvalue that is not a `const` binding; the two
  // questions are separate because `const c = 1; c = 2;` must be one specific
  // diagnostic and not "not an lvalue".
  bool isLvalue = false;
  // Every operand was a literal or a constant, so the value is known.
  bool isConstant = false;
  // ... and the value is an integer, so `value` means something. Float folding
  // is deliberately not done here: the value would need a float parser whose
  // rounding this stage cannot verify, and no check this stage makes needs it
  // (`sema.md`, *Constant folding*).
  bool hasIntValue = false;
  support::ConstInt value;
};

// A *modifiable* lvalue is an lvalue whose declaration is not a `const`. The two
// questions are separate on purpose: `const c = 1; c = 2;` must be one specific
// diagnostic ("`c` is a `const`") and not "this is not a place a value can be
// stored", and only the def map knows which declaration a name denotes.

// One checked function.
struct FunctionInfo {
  ast::AstId decl;
  // The function's own type: return type plus parameters.
  TypeId functionType;
  // Just the return type, which is what a `return` statement is checked against.
  TypeId returnType;
  // `kInvalidAst` when the declaration has no body.
  ast::AstId body;
  // The declared name, for a diagnostic that has to name the function.
  support::SymId name = support::kInvalidSym;
};

struct TypedFile {
  TypedFile() = default;

  [[nodiscard]] bool empty() const {
    return typeTable.empty();
  }
  [[nodiscard]] std::size_t nodeCount() const {
    return typeTable.size();
  }

  // The type of any node, or the poison when the node has none (tokens, a
  // declaration's structural nodes). Never throws, never reads out of range:
  // an id from another unit answers `kTypeError` rather than a wrong type.
  [[nodiscard]] TypeId typeOf(ast::AstId id) const {
    if (!id.valid() || id.index >= typeTable.size()) {
      return kTypeError;
    }
    return typeTable[id.index];
  }
  // The expression facts, or a default-constructed `ExprInfo` when the node is
  // not an expression.
  [[nodiscard]] const ExprInfo& infoOf(ast::AstId id) const {
    static const ExprInfo none;
    if (!id.valid() || id.index >= exprFacts.size()) {
      return none;
    }
    return exprFacts[id.index];
  }
  [[nodiscard]] const FunctionInfo* functionOf(ast::AstId decl) const {
    for (const FunctionInfo& info : functionTable) {
      if (info.decl == decl) {
        return &info;
      }
    }
    return nullptr;
  }

  [[nodiscard]] std::span<const TypeId> types() const {
    return typeTable;
  }

  // `sema` fills these; public so a test can build a typed file by hand.
  std::vector<TypeId> typeTable;
  std::vector<ExprInfo> exprFacts;
  std::vector<FunctionInfo> functionTable;
};

} // namespace minc::sema
