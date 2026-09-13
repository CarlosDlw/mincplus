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

#include <algorithm>
#include <cstddef>
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
  // The type a compound assignment performs its operation at, when this
  // expression is one: `x <<= n` on a `u16` shifts at `promote(u16)` = `i32`,
  // and `typeOf` is the *store*'s type, so without this the width is not in the
  // tree at all (`ir.md`, *The fourth fact nobody recorded*). `kInvalidType`
  // everywhere else.
  TypeId opType = kInvalidType;
};

// One implicit conversion, recorded where it happens.
//
// The lowering materialises these instead of deciding a conversion of its own: a
// conversion is a fact about the program, and a stage that recomputes one holds
// a second copy of the rule (`ir.md`, *The coercion record*).
//
// Keyed by the **consumer**, not by the value, and the distinction is not
// cosmetic: a consumer is where the conversion is *applied*, and an expression
// node has one consumer only while the tree stays a tree. A parenthesised
// expression is the consumer of its operand and applies the conversion itself,
// so a record keyed on the value would sit on the wrong node and the lowering
// would convert twice or not at all.
struct Coercion {
  // The node doing the consuming -- a statement, when the value is an
  // initializer or a `return` operand.
  ast::AstId consumer;
  // Which operand of the consumer, in source order, counting only the operands
  // (not tokens): `BinaryExpr`'s left operand is 0, a `LetStmt`'s initializer is
  // 0, a call's first argument is 1 (the callee is 0).
  std::uint8_t operand = 0;
  // The expression whose value is converted, kept so the pair can be checked
  // against the tree (`from` must equal `typeOf(node)`) and so a consumer that
  // was deleted -- a folded `?:` arm, say -- is visible instead of implied.
  ast::AstId node;
  // What the operand produced, and what the consumer needs. `from` is always
  // `typeOf(operand)`, and it is stored rather than looked up because the pair
  // *is* the conversion; a test asserts the two agree.
  //
  // Both are always types the IR can map: never a deferred literal (its width
  // would have to be guessed) and never the poison (a tree with one is not
  // lowered at all).
  TypeId from = kInvalidType;
  TypeId to = kInvalidType;
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

  // The conversions the consumer applies, in the order they were recorded.
  // Between `buildCoercionIndex()` calls this is a plain list.
  [[nodiscard]] std::span<const Coercion> coercions() const {
    return coercions_;
  }

  // The conversions `consumer` applies, in operand order. Empty for a node that
  // converts nothing, which is most of them.
  [[nodiscard]] std::span<const Coercion> coercionsOf(ast::AstId consumer) const {
    if (!consumer.valid() || consumer.index + 1 >= coercionFirst_.size()) {
      return {};
    }
    const std::uint32_t first = coercionFirst_[consumer.index];
    const std::uint32_t end = coercionFirst_[consumer.index + 1];
    if (first > end || end > coercions_.size()) {
      return {};
    }
    return {coercions_.data() + first, end - first};
  }

  // The conversion `consumer` applies to one operand, or nullptr. A scan of the
  // node's own list, which is as long as the consumer has operands.
  [[nodiscard]] const Coercion* coercionAt(ast::AstId consumer, std::uint8_t operand) const {
    for (const Coercion& coercion : coercionsOf(consumer)) {
      if (coercion.operand == operand) {
        return &coercion;
      }
    }
    return nullptr;
  }

  // Sorts the list and builds the per-consumer index. A function of the list and
  // nothing else, so a test can hand-build both and get the same answers, and so
  // the sorted order -- which is what a dump prints -- is deterministic. The
  // order is `(consumer, operand)`, which is also the order a lowering visits
  // them in, so the list reads left to right the way the source does.
  void buildCoercionIndex(std::size_t nodeCount) {
    std::stable_sort(coercions_.begin(), coercions_.end(),
                     [](const Coercion& a, const Coercion& b) {
                       if (a.consumer.index != b.consumer.index) {
                         return a.consumer.index < b.consumer.index;
                       }
                       return a.operand < b.operand;
                     });
    coercionFirst_.assign(nodeCount + 1, 0);
    for (const Coercion& coercion : coercions_) {
      if (coercion.consumer.index < nodeCount) {
        ++coercionFirst_[coercion.consumer.index + 1];
      }
    }
    for (std::size_t i = 1; i < coercionFirst_.size(); ++i) {
      coercionFirst_[i] += coercionFirst_[i - 1];
    }
  }

  void addCoercion(const Coercion& coercion) {
    coercions_.push_back(coercion);
  }

  // `sema` fills these; public so a test can build a typed file by hand.
  std::vector<TypeId> typeTable;
  std::vector<ExprInfo> exprFacts;
  std::vector<FunctionInfo> functionTable;

private:
  // Private with `addCoercion`/`buildCoercionIndex` as the only writers: the two
  // have to stay in step, and a public vector would let a caller append and
  // leave the index describing a list that no longer exists.
  std::vector<Coercion> coercions_;
  std::vector<std::uint32_t> coercionFirst_;
};

} // namespace minc::sema
