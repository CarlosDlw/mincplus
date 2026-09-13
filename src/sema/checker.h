// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The checker, internal to `src/sema`.
//
// One object per unit, one pass, everything about it in one place. It is split
// across `check_stmt.cc` (declarations, statements, types) and
// `check_expr.cc` (expressions) but it is *one* class: an expression cannot be
// typed without the enclosing function's return type, a statement cannot be
// checked without the same environment, and splitting the state would mean
// passing the same fifteen things down every call.
//
// The checker owns three pieces of derived state for the duration of a unit:
//
//   * `defTypes_` -- every declaration's type, filled by the signature pass
//     before any body is checked, which is what makes a call to a function
//     written *later* in the file work;
//   * `defConstValues_` -- what a `const` binding is worth, so folding a `const`
//     expression does not need the initializer again;
//   * `depth_` -- the AST-depth guard, so a pathological tree is a diagnostic
//     and not a stack overflow.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ast/ast.h"
#include "parse/syntax_kind.h"
#include "resolve/map.h"
#include "sema/sema.h"
#include "sema/sema_error.h"
#include "sema/type_store.h"
#include "sema/typed_ast.h"
#include "support/intern/interner.h"

// The operator tokens the checker names, spelled once. Internal to this module,
// so it is included the way a sibling source file is: by name.
#include "tokens.h"

namespace minc::sema {

class Checker {
public:
  Checker(const ast::LoweredFile& file, const resolve::DefMap& defs,
          const support::Interner& symbols, TypeStore& types, SemaOptions options);

  [[nodiscard]] SemaOutput run();

private:
  // --- tree access -----------------------------------------------------------

  [[nodiscard]] bool inError(ast::AstId id) const {
    return file_.inErrorRegion(id);
  }
  [[nodiscard]] ast::AstId childOf(ast::AstId id, ast::NodeKind kind) const {
    return file_.childOfKind(id, kind);
  }
  [[nodiscard]] std::vector<ast::AstId> childrenOf(ast::AstId id, ast::NodeKind kind) const {
    return file_.childrenOfKind(id, kind);
  }
  // The children that are not tokens, in source order: for most nodes this is
  // exactly the operands, which is what keeps the checker from re-implementing
  // the grammar.
  [[nodiscard]] std::vector<ast::AstId> operandsOf(ast::AstId id) const;
  // The single token child, for the nodes whose operator or literal is one:
  // `BinaryExpr`, `PrefixExpr`, `PostfixExpr`, `AssignExpr`, `LiteralExpr`.
  // `kInvalidAst` when there is none.
  [[nodiscard]] ast::AstId tokenOf(ast::AstId id) const;
  [[nodiscard]] ast::NodeKind kindOf(ast::AstId id) const {
    return file_.at(id).kind;
  }
  [[nodiscard]] std::string_view spelling(ast::AstId id) const {
    return file_.spellingOf(id);
  }
  [[nodiscard]] support::Span origin(ast::AstId id) const {
    return file_.at(id).origin;
  }

  // --- results --------------------------------------------------------------

  void setType(ast::AstId id, TypeId type);
  void setExpr(ast::AstId id, const ExprInfo& info);

  // --- diagnostics ----------------------------------------------------------
  //
  // One place that appends, so ordering (source order, because the walk is)
  // and the error budget are decisions of the checker and not of a call site.
  void error(ast::AstId at, SemaErrorCode code, std::string message);
  void errorAt(support::Span span, SemaErrorCode code, std::string message);
  void warning(ast::AstId at, SemaErrorCode code, std::string message);
  void attachNote(SemaErrorCode code, support::Span span, std::string note);

  // --- declarations and statements ------------------------------------------

  void runSignatures();
  void checkFunction(const FunctionInfo& info);
  void checkBody(ast::AstId block, TypeId returnType);
  void checkStatement(ast::AstId stmt, TypeId returnType);
  void checkBlock(ast::AstId block, TypeId returnType);

  // -- control flow -----------------------------------------------------------

  // The one place a condition is decided, for `if`, `while`, `for` and `?:`
  // alike: a condition is a `bool`, and an arithmetic value is not silently one.
  // `what` names the construct, so the sentence says which condition it is about.
  void checkCondition(ast::AstId condition, std::string_view what);
  void checkIf(ast::AstId stmt, TypeId returnType);
  void checkWhile(ast::AstId stmt, TypeId returnType);
  void checkFor(ast::AstId stmt, TypeId returnType);

  // Can control reach the end of this statement? Exact rather than conservative
  // wherever the language allows an exact answer: a `return`; a block whose last
  // reachable statement terminates; an `if` whose *both* arms terminate; a loop
  // whose condition is a constant `true` and whose body contains no `break` that
  // could leave it; and a region the parser already reported (where saying
  // "control falls off the end" would be a second sentence about one mistake).
  [[nodiscard]] bool terminates(ast::AstId stmt) const;
  // Is this loop guaranteed to leave only through a `return`? True for a
  // constant-true condition with no `break` aimed at *this* loop -- a `break`
  // inside a nested loop belongs to that loop and does not count.
  [[nodiscard]] bool loopsForever(ast::AstId stmt) const;
  [[nodiscard]] bool hasBreakForThisLoop(ast::AstId node) const;
  // The folded value of a condition, when the checker folded one. `nullopt` for
  // a condition it could not resolve to a constant.
  [[nodiscard]] std::optional<bool> constantCondition(ast::AstId condition) const;
  // `break`/`continue` are the only statements whose *validity* depends on where
  // they were written, so the loop nesting is tracked as the walk descends.
  std::uint32_t loopDepth_ = 0;

  // --- definite assignment ---------------------------------------------------
  //
  // A binding with no initializer holds no value, and this is what proves it is
  // never read that way. The analysis is a *separate pass* over the body and not
  // a thread through the typing walk, for two reasons: an assignment is an
  // expression, so the state has to move left to right *inside* an expression,
  // and a name being stored into is not a name being read, which is a
  // distinction the typing walk does not make when it types the target.
  //
  // The rules are the ones every production language with this check uses (JLS
  // 16, the C# and Swift specs): the state is a set of assigned definitions, a
  // conditional merges by *intersection* because only one side runs, a loop is
  // analyzed with the state from before it because it can run zero times, and a
  // loop whose condition is constantly true is left only by `break`, so its exit
  // state is the intersection of the states at its own breaks. No fixpoint and
  // no CFG: the shape of the language is enough, which is what keeps the answer
  // exact in step with what the checker already knows about reachability.
  //
  // One slot per definition in the unit; whether a slot is meaningful is
  // decided by the definition's kind (`Variable` and nothing else can be
  // unassigned).
  using AssignmentSet = std::vector<bool>;
  using BreakStates = std::vector<AssignmentSet>;

  void checkDefiniteAssignment(ast::AstId body);
  void flowStatement(ast::AstId stmt);
  void flowExpression(ast::AstId expr);
  // The target of a plain assignment: a store, not a load. Reads inside it (a
  // pointer being dereferenced, later) still count, which is why it is not
  // simply skipped.
  void flowStoreTarget(ast::AstId place);
  void markAssigned(ast::AstId place);
  void reportIfUnassigned(resolve::DefId def, ast::AstId at);
  // The state after a loop leaves, from the state at its entry and the states at
  // the `break`s aimed at it.
  [[nodiscard]] AssignmentSet loopExit(const AssignmentSet& entry, const BreakStates& breaks,
                                       ast::AstId condition);
  AssignmentSet assigned_;
  // Which definitions have already been reported. One mistake, one
  // diagnostic: a binding nobody assigned and read five times is one thing to
  // fix, and the five reads are one sentence rather than five.
  AssignmentSet reportedUnassigned_;
  // One per nested loop, innermost last: a `break` is recorded in the list of
  // the loop it belongs to, which a stack decides without a second scan.
  std::vector<BreakStates> breakStack_;

  // --- types -----------------------------------------------------------------

  // The words of a `Type` node, in source order.
  [[nodiscard]] std::vector<std::string_view> typeWords(ast::AstId typeNode) const;
  [[nodiscard]] TypeId resolveTypeNode(ast::AstId typeNode);
  // The type a binding has when nothing constrained it: `i32` / `f64` for a
  // deferred literal, the type itself otherwise.
  [[nodiscard]] TypeId defaultValue(TypeId type);
  // A deferred literal takes the type its context gave it, with a range check.
  // Anything else comes back unchanged; the assignment conversion is separate.
  [[nodiscard]] TypeId adaptTo(TypeId type, TypeId expected, ast::AstId at, const ExprInfo& info);
  // The assignment conversion, reported with the code of the context that asked.
  void checkAssignable(TypeId from, TypeId to, ast::AstId at, SemaErrorCode code,
                       std::string_view what);
  [[nodiscard]] std::string suggestTypeName(std::string_view word) const;

  // --- expressions -----------------------------------------------------------

  // The one entry point for an expression. It computes the node's type, adapts a
  // deferred literal to the context, writes both the type and the facts into the
  // artifact, and returns the adapted type -- so *every* caller sees the type the
  // expression actually has in context, and no call site can forget to adapt.
  [[nodiscard]] TypeId checkExpr(ast::AstId expr, TypeId expected);

  // The per-kind workers. They return the type and fill `info`; they never adapt
  // and never write to the artifact, which is what keeps `checkExpr` the only
  // place a node's answer is stored.
  // The literal needs `expected`: a value too large for the 64-bit core is
  // accepted only when the context asks for a type that can hold it (`i128`,
  // `u128`), and refused with a range error everywhere else.
  [[nodiscard]] TypeId checkLiteral(ast::AstId expr, TypeId expected, ExprInfo& info);
  [[nodiscard]] TypeId checkPath(ast::AstId expr, ExprInfo& info);
  [[nodiscard]] TypeId checkPrefix(ast::AstId expr, ExprInfo& info);
  [[nodiscard]] TypeId checkPostfix(ast::AstId expr, ExprInfo& info);
  [[nodiscard]] TypeId checkBinary(ast::AstId expr, ExprInfo& info);
  [[nodiscard]] TypeId checkConditional(ast::AstId expr, TypeId expected, ExprInfo& info);
  [[nodiscard]] TypeId checkAssign(ast::AstId expr, ExprInfo& info);
  [[nodiscard]] TypeId checkCall(ast::AstId expr, ExprInfo& info);

  // Folding for the operators that have a folded value. False when a constant
  // division or remainder by zero was reported: that is a diagnostic here for
  // the same reason the preprocessor diagnoses it in a `#if`, rather than an
  // undefined behaviour for the IR to inherit.
  [[nodiscard]] bool foldBinary(ast::AstId opToken, Tag op, const ExprInfo& left,
                                const ExprInfo& right, ExprInfo& info);

  // True when `operand` may be stored to. Reports the specific reason when it
  // may not: a `const` binding, or something that is not a place at all. The
  // two are separate diagnostics because their fixes are different.
  [[nodiscard]] bool checkModifiable(ast::AstId operand, TypeId type, ast::AstId at,
                                     SemaErrorCode code, std::string_view what);

  // --- declaration lookup ----------------------------------------------------

  void reportLimit(ast::AstId at);

  [[nodiscard]] std::optional<resolve::DefId> defAtName(ast::AstId nameNode) const;
  [[nodiscard]] const resolve::Def* defFor(resolve::DefId id) const;
  [[nodiscard]] TypeId typeOfDef(resolve::DefId id) const;
  [[nodiscard]] bool isConstDef(resolve::DefId id) const;
  // The declaration a `PathExpr` denotes, resolved by the stage below.
  [[nodiscard]] std::optional<resolve::DefId> defOfPath(ast::AstId pathExpr) const;
  // The same, seen through parentheses -- the `const` question, which is asked
  // about a *place* and not about a path.
  [[nodiscard]] std::optional<resolve::DefId> defOfPlace(ast::AstId expr) const;
  // A name to put in a message about `expr`: its spelling when it is a path,
  // and a description otherwise, so no diagnostic says "this expression" about
  // something the reader can see is a name.
  [[nodiscard]] std::string nameOf(ast::AstId expr) const;

  // A folded value as the digits the reader would have written for it. Signedness
  // decides how the two's-complement bits are read; printing the raw bits of a
  // negative value would put `18446744073709551615` in a message about `-1`.
  // Here rather than in one file because two diagnostics print a value and the
  // two spellings must not drift.
  [[nodiscard]] static std::string valueText(support::ConstInt value);

  // How deep the checker will follow a tree before it stops and reports. The
  // lowered tree's depth is bounded by the parser, which asserts the same bound
  // at its entry; this is the second belt.
  [[nodiscard]] bool enterDepth();

  const ast::LoweredFile& file_;
  const resolve::DefMap& defs_;
  const support::Interner& symbols_;
  TypeStore& types_;
  SemaOptions options_;
  SemaOutput out_;

  std::vector<TypeId> defTypes_;
  std::vector<support::ConstInt> defConstValues_;
  std::vector<bool> defHasConstValue_;
  std::vector<bool> defIsConst_;
  // Name-use span start -> the reference that answers it. Resolved once, so a
  // `PathExpr` costs a lookup and not a scan. The key is `(file, offset)`
  // packed, because a unit spans several files and offsets restart in each.
  std::unordered_map<std::uint64_t, std::size_t> refByOffset_;
  // Declaration name-span start -> def, for the reverse direction.
  std::unordered_map<std::uint64_t, resolve::DefId> defByNameOffset_;

  TypeId currentReturn_ = kTypeError;
  std::uint32_t currentFunctionName_ = support::kInvalidSym;
  std::size_t depth_ = 0;
  bool limitReported_ = false;
};

} // namespace minc::sema
