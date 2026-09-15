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
#include <string_view>
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

// --- the access record -------------------------------------------------------
//
// `memory.md` states one obligation per access -- inside a live object, at the
// alignment the type states, over bytes that were written -- and one *assumption*
// the optimizer is allowed to make about the pointer it goes through. This is
// where the stage that has the tree writes that answer down, so the lowering
// reads it instead of re-deriving it: the same rule the coercion record follows,
// for the same reason.
//
// It is written by the checker at the node that *denotes* the access (`*p`, and
// `p[i]`), which is the node the lowering is standing on when it emits the load
// or the store.

// Which obligations apply, and which of them the source has relaxed.
//
// One enumerator today, and deliberately: the two the model names -- an access
// whose alignment is not required (`unaligned`) and one performed exactly as
// written (`volatile`) -- arrive with the syntax that asks for them, and this
// table grows with it. A kind no input can produce is a kind no test can pin,
// which is the same rule `SemaErrorCode` states for its own list.
enum class AccessKind : std::uint8_t {
  // The access carries the whole rule: it is aligned as its type requires, and
  // it is performed once, in order, with the value its type says.
  Ordinary,
};

// Where the pointer's permission comes from, as far as this stage can *prove*.
//
// "Prove" is the operative word, and the two answers are the two ends of it. The
// rule is syntactic and therefore sound and incomplete on purpose: it recognises
// the address of an object this unit named, moved only by arithmetic since, and
// it says `Foreign` for everything else -- a parameter, a value read from memory,
// a value returned by a call. Being incomplete costs the optimizer an assumption
// it could have had; being wrong would cost a correct program its meaning, and
// only one of those is recoverable.
enum class ProvenanceKind : std::uint8_t {
  // `&x`, or arithmetic over it: the pointer's provenance is `x`'s allocation,
  // and nothing else can be reached through it.
  Object,
  // Anything the compiler cannot name. The conservative answer, and the one that
  // assumes nothing.
  Foreign,
};

// One access. `type` is what is accessed -- the access's size and alignment are
// its -- and the lowering reads those from the store rather than from the
// pointer's own type.
struct AccessObligation {
  // The place-expression the access goes through: a `*p`, a `p[i]` or an
  // `a[i]` where `a` is an array.
  ast::AstId place;
  // The type being accessed. Its size is the access's width and its alignment is
  // the access's alignment, so a lowering that reads them re-derives nothing.
  TypeId type = kInvalidType;
  AccessKind kind = AccessKind::Ordinary;
  ProvenanceKind provenance = ProvenanceKind::Foreign;
  // How many elements the object the place is *inside* has, when this stage can
  // prove one: an array subscript always can, because the count is in the type.
  // `0` is "not known", which is every `*p` -- a pointer's extent is not a
  // number this stage can see.
  //
  // It is the half of the checked build's guard that the module can answer on
  // its own: `object` provenance *plus* a count is a bounds check the lowering
  // can emit without a shadow memory (`arrays.md` decision 26, `ir.md`). A
  // subscript through a pointer keeps its `foreign` answer and no guard, which
  // is why the two spellings of "element i" are not the same program here.
  std::uint64_t extent = 0;
};

// The stable name of an access kind, in one table with the enumeration so a kind
// added without a row is caught by a test (`allAccessKinds`).
struct AccessKindInfo {
  AccessKind kind;
  const char* name;
};

[[nodiscard]] std::span<const AccessKindInfo> accessKindInfos();
// Every access kind, derived from the table above.
[[nodiscard]] std::span<const AccessKind> allAccessKinds();
[[nodiscard]] std::string_view toString(AccessKind kind);

struct ProvenanceKindInfo {
  ProvenanceKind kind;
  const char* name;
};

[[nodiscard]] std::span<const ProvenanceKindInfo> provenanceKindInfos();
[[nodiscard]] std::span<const ProvenanceKind> allProvenanceKinds();
[[nodiscard]] std::string_view toString(ProvenanceKind kind);

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

// What a file-scope binding was initialized with, as a *value*.
//
// This is the record the lowering materialises, and it exists for two reasons
// that are both rules of this project. The first is that a file-scope object's
// bytes are written by the compiler rather than by a statement, so the value has
// to be *published* and cannot be computed a stage later. The second is that a
// lowering folding an initializer expression itself would be a second copy of
// the constant rules -- and the two copies are what disagree.
//
// The set is closed, and every alternative is a value **known before the program
// exists**, which is what "there is no dynamic initialization at file scope"
// means (`globals.md`, decision 2). A binding whose initializer is anything else
// never reaches this table: it is refused, by name, in the checker.
enum class GlobalValueKind : std::uint8_t {
  // No initializer. The object is zero -- the C ABI's `.bss` (`memory.md`,
  // decision 12).
  Zero,
  // A folded integer: every integer, `char` and `bool` constant, and any
  // arithmetic over them, because `#if` and this share one constant core
  // (`support/consteval`). `intValue` is the value.
  Int,
  // A single literal whose value the lowering reads from its own spelling: a
  // float, a `str`, or an integer wider than the 64-bit core. `node` is that
  // literal, so the reader is the one that already exists for it -- and no
  // arithmetic happens in the lowering either way. `negated` is the one
  // modification allowed on top of it, and it is a sign bit and not a value.
  Literal,
  // The null pointer.
  Null,
};

// What an enumerator is called, in one table with the enumeration, so a kind
// added without a name is caught by a test rather than printed as a number.
struct GlobalValueKindInfo {
  GlobalValueKind kind;
  const char* name;
};

[[nodiscard]] std::span<const GlobalValueKindInfo> globalValueKindInfos();
[[nodiscard]] std::span<const GlobalValueKind> allGlobalValueKinds();
[[nodiscard]] std::string_view toString(GlobalValueKind kind);

// One file-scope binding.
struct GlobalInfo {
  // The `LetStmt`/`ConstStmt` the binding was written as.
  ast::AstId decl;
  // The initializer expression, or `kInvalidAst` for a binding with none.
  ast::AstId init;
  // The binding's type: the annotation's, or the initializer's.
  TypeId type = kInvalidType;
  GlobalValueKind value = GlobalValueKind::Zero;
  // The value, when `value` is `Int`.
  support::ConstInt intValue;
  // The literal whose spelling *is* the value, when `value` is `Literal`.
  ast::AstId node;
  // True when the value is the **negation** of what `node`'s spelling says:
  // `-2.5`. It is not arithmetic and it is not a folder this stage declined to
  // write -- a negated literal is a sign bit, which is exact for both a float and
  // a two's-complement integer, so the *spelling* is still the whole value and
  // the lowering still reads no more than it for an unnegated one. Without this,
  // `const neg: f64 = -1.0;` would be refused for being "not constant", which it
  // plainly is, and rejecting the written form of a value is how a language
  // teaches its users to work around it.
  bool negated = false;
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

  // The file-scope binding declared at `decl`, or nullptr when that node is not
  // one in this unit. A scan of a table with one entry per file-scope binding,
  // and the lowering asks it once per binding it declares.
  [[nodiscard]] const GlobalInfo* globalOf(ast::AstId decl) const {
    for (const GlobalInfo& info : globalTable) {
      if (info.decl == decl) {
        return &info;
      }
    }
    return nullptr;
  }

  void addGlobal(const GlobalInfo& info) {
    globalTable.push_back(info);
  }

  [[nodiscard]] std::span<const TypeId> types() const {
    return typeTable;
  }

  // The conversions the consumer applies, in the order they were recorded.
  // Between `buildCoercionIndex()` calls this is a plain list.
  [[nodiscard]] std::span<const Coercion> coercions() const {
    return coercions_;
  }

  // The accesses through a pointer, in the order they were recorded. Between
  // `buildAccessIndex()` calls this is a plain list.
  [[nodiscard]] std::span<const AccessObligation> accesses() const {
    return accesses_;
  }

  // The access obligation belonging to `place`, or nullptr when that node
  // denotes no access through a pointer. A scan of a list that has one entry per
  // dereference in the unit, and the caller asks it once per dereference it
  // lowers, so the cost is paid once per node either way.
  [[nodiscard]] const AccessObligation* accessAt(ast::AstId place) const {
    for (const AccessObligation& access : accesses_) {
      if (access.place == place) {
        return &access;
      }
    }
    return nullptr;
  }

  void addAccess(const AccessObligation& access) {
    accesses_.push_back(access);
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
  std::vector<GlobalInfo> globalTable;

private:
  // Private with `addCoercion`/`buildCoercionIndex` as the only writers: the two
  // have to stay in step, and a public vector would let a caller append and
  // leave the index describing a list that no longer exists.
  std::vector<Coercion> coercions_;
  std::vector<std::uint32_t> coercionFirst_;
  // The accesses. Appended in the checker's walk order, which is source order,
  // because that is the order the walk has -- so the list is deterministic
  // without a sort, and a dump of it reads left to right like the source.
  std::vector<AccessObligation> accesses_;
};

} // namespace minc::sema
