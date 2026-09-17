// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Definite assignment: the paths that never gave a binding a value.
//
// `let x: i32;` is legal and is the C idiom -- declare, then assign in the
// branch that knows the answer -- and it is the one hole through which this
// language could read an object nobody ever wrote. Every language that closed
// that hole did it the same way: a *definite assignment* analysis, computed
// before the read is accepted, with the answer coming from the shape of the
// control flow rather than from a convention the programmer is trusted to keep.
// Java specifies it in full (JLS 16), C# and Swift make it an error rather than
// a warning, and Rust refuses to hand out a value it cannot prove was written.
// This is that analysis over this grammar.
//
// It is a separate pass over the body, and deliberately not a thread through the
// typing walk:
//
//   * an assignment is an *expression* here, so the state has to move left to
//     right inside an expression, and `x + (x = 1)` must not be accepted;
//   * the target of a plain assignment is not a read, and the typing walk has no
//     reason to distinguish the two when it types the target;
//   * the artifact it needs -- the folded value of a condition, to know whether
//     a loop can fall out of the bottom -- is complete only after the body has
//     been typed. So it runs after, over the same tree, and reads the answer
//     instead of recomputing it.
//
// The state is a set of definitions, and the rules are a merge per construct:
//
//   `S1; S2`              sequential: the state after S1 is the state before S2
//   `if (c) A else B`     intersection: only one arm runs
//   `if (c) A`            the state from before: the arm may not run at all
//   `while (c) S`         the body is analyzed from the entry state (it can run
//                         zero times), and so is the code after it
//   `while (true) S`      nothing falls out of the bottom, so the exit state is
//                         the intersection of the states at its own `break`s;
//                         with no `break`, nothing after it is reachable
//   `for (i; c; s) S`     `i` then a `while`: the step is checked with the state
//                         the body ended in, the code after the loop is not
//   `a && b`, `a || b`    `b` may not run, so the merge is an intersection
//   `c ? a : b`           the arms merge by intersection
//   `x = e`               a store: the state after is the state before, plus `x`
//   `x op= e`, `x++`      a read and a store: the read is checked first
//
// There is no fixpoint and no CFG, and that is not a shortcut: it is the same
// choice the specs make, because the language's own structure answers the
// question exactly. What it costs is the precision a dataflow would have about a
// value assigned *inside* a loop and read by the loop's condition on a later
// iteration; the answer there is "not proved", which is the conservative side
// and the side that has no false negatives.
#include "checker.h"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace minc::sema {
namespace {

// Bitwise intersection: after a merge, only what both sides had.
void intersectWith(std::vector<bool>& into, const std::vector<bool>& from) {
  const std::size_t shared = into.size() < from.size() ? into.size() : from.size();
  for (std::size_t i = 0; i < shared; ++i) {
    into[i] = into[i] && from[i];
  }
}

// The condition of a `while`, or of an `if`: the operand that is not the body
// and not the `else` arm. Found by kind rather than by position, so a change to
// the order of a statement's children cannot make this read the wrong node.
[[nodiscard]] ast::AstId conditionOf(const ast::LoweredFile& file, ast::AstId stmt) {
  for (const ast::AstId operand : file.childrenOf(stmt)) {
    if (file.at(operand).isToken()) {
      continue;
    }
    switch (file.at(operand).kind) {
    case ast::NodeKind::Block:
    case ast::NodeKind::ElseClause:
      continue;
    default:
      return operand;
    }
  }
  return ast::AstId{};
}

} // namespace

// --- the pass ----------------------------------------------------------------

void Checker::checkDefiniteAssignment(ast::AstId body) {
  if (!body.valid()) {
    return;
  }
  // The state is one bit per definition *in the unit*, not per definition in
  // this function: both readers -- `markAssigned` from a store, and
  // `reportIfUnassigned` from a read -- hold a global def index, and the
  // obvious compaction (a slot per function-local binding) needs an index the
  // stage below does not hand out yet.
  //
  // That makes the pass O(functions x definitions) bit writes, and a merge a copy
  // of the same size. It is bounded and not unbounded -- the def budget is the
  // bound -- and it is small next to the pipeline on the only inputs where it
  // could show: on a 17 MiB unit (3000 functions, 757k definitions) the state is
  // 95 KiB, so the whole pass is under a second of memory traffic while lexing,
  // preprocessing and lowering take tens of seconds on the same file before the
  // checker runs at all. Recorded rather than left to be discovered: if a
  // profile ever disagrees, the fix is a per-function slot table, and it is a
  // change to the state and not to the rules.
  assigned_.assign(defs_.defs.size(), false);
  reportedUnassigned_.assign(defs_.defs.size(), false);
  breakStack_.clear();

  // A parameter is a binding that already holds a value when the first statement
  // runs, so it starts assigned. Nothing else does: a `let` with no initializer
  // has to be assigned first, and a `const` with no value is refused a stage
  // earlier (`validate`), so it can never be the reason a read is wrong.
  for (std::size_t i = 0; i < defs_.defs.size(); ++i) {
    if (defs_.defs[i].kind == resolve::DefKind::Parameter) {
      assigned_[i] = true;
    }
  }

  flowStatement(body);

  assigned_.clear();
  reportedUnassigned_.clear();
  breakStack_.clear();
}

Checker::AssignmentSet Checker::loopExit(const AssignmentSet& entry, const BreakStates& breaks,
                                         ast::AstId condition) {
  // No condition at all is the language's spelling of `true` -- `for ;; {}` is
  // an infinite loop -- so `condition.valid()` is what tells the two apart.
  const bool alwaysTrue = !condition.valid() || constantCondition(condition).value_or(false);

  AssignmentSet out = alwaysTrue ? AssignmentSet(entry.size(), true) : entry;
  for (const AssignmentSet& atBreak : breaks) {
    intersectWith(out, atBreak);
  }
  return out;
}

// --- statements --------------------------------------------------------------

void Checker::flowStatement(ast::AstId stmt) {
  if (!stmt.valid() || inError(stmt)) {
    return;
  }

  switch (kindOf(stmt)) {
  case ast::NodeKind::LetStmt:
  case ast::NodeKind::ConstStmt: {
    // The initializer is checked first, and it cannot see the bindings it belongs
    // to: resolution scopes a `let` from the statement after it, so a name in its
    // own initializer is the outer one, and the outer one is what is read.
    const ast::AstId init = initializerOf(stmt);
    if (init.valid()) {
      flowExpression(init);
    }

    // A binding with an initializer holds a value from here; one without holds
    // nothing until an assignment reaches it on every path that gets here.
    //
    // **Every name the statement binds** and not the one it usually binds: a
    // destructuring assigns all of its names at once, because the value they are
    // taken from is one value and it is evaluated once (`tuples.md`, decision 6).
    if (init.valid()) {
      for (const ast::AstId name : file_.bindingNamesOf(stmt)) {
        if (const std::optional<resolve::DefId> def = defAtName(name)) {
          if (def->index < assigned_.size()) {
            assigned_[def->index] = true;
          }
        }
      }
    }
    return;
  }
  case ast::NodeKind::ReturnStmt: {
    const std::vector<ast::AstId> operands = operandsOf(stmt);
    if (!operands.empty()) {
      flowExpression(operands.front());
    }
    return;
  }
  case ast::NodeKind::ExprStmt: {
    const std::vector<ast::AstId> operands = operandsOf(stmt);
    if (!operands.empty()) {
      flowExpression(operands.front());
    }
    return;
  }
  case ast::NodeKind::Block:
    for (const ast::AstId child : operandsOf(stmt)) {
      flowStatement(child);
    }
    return;
  case ast::NodeKind::ElseClause:
    // The arm is a block or an `if` (the `else if` chain); either way it is a
    // statement, and the caller has already merged the two arms.
    for (const ast::AstId child : operandsOf(stmt)) {
      flowStatement(child);
    }
    return;
  case ast::NodeKind::IfStmt: {
    flowExpression(conditionOf(file_, stmt));

    const AssignmentSet before = assigned_;
    const ast::AstId thenBlock = childOf(stmt, ast::NodeKind::Block);
    flowStatement(thenBlock);
    const AssignmentSet afterThen = assigned_;

    const ast::AstId elseClause = childOf(stmt, ast::NodeKind::ElseClause);
    if (elseClause.valid()) {
      assigned_ = before;
      flowStatement(elseClause);
      // Only one arm runs, so a binding is assigned afterwards only when both
      // arms ended with it assigned.
      intersectWith(assigned_, afterThen);
    } else {
      // The arm may not run at all, so it can add nothing.
      assigned_ = before;
    }
    return;
  }
  case ast::NodeKind::WhileStmt: {
    const ast::AstId condition = conditionOf(file_, stmt);
    flowExpression(condition);

    // The body is analyzed from the entry state, and so is everything after the
    // loop: the condition can be false on the first test, and a value the body
    // assigns on one iteration is not a value the next iteration or the exit can
    // rely on. That is the conservative answer, and it is the one the specs
    // take.
    const AssignmentSet entry = assigned_;
    breakStack_.emplace_back();
    flowStatement(childOf(stmt, ast::NodeKind::Block));
    const BreakStates breaks = std::move(breakStack_.back());
    breakStack_.pop_back();

    assigned_ = loopExit(entry, breaks, condition);
    return;
  }
  case ast::NodeKind::ForStmt: {
    ast::AstId init = ast::AstId{};
    ast::AstId condition;
    ast::AstId step;
    ast::AstId body;
    for (const ast::AstId operand : operandsOf(stmt)) {
      switch (kindOf(operand)) {
      case ast::NodeKind::ForCondition:
        condition = operand;
        break;
      case ast::NodeKind::ForStep:
        step = operand;
        break;
      case ast::NodeKind::Block:
        body = operand;
        break;
      default:
        init = operand;
        break;
      }
    }

    flowStatement(init);

    // The init is the only part that certainly ran, so it is what the exit state
    // starts from -- same rule as the `while` above, with the initializer's
    // assignments included.
    const AssignmentSet entry = assigned_;
    const std::vector<ast::AstId> conditionExprs = operandsOf(condition);
    const ast::AstId conditionExpr = conditionExprs.empty() ? ast::AstId{} : conditionExprs.front();
    if (conditionExpr.valid()) {
      flowExpression(conditionExpr);
    }

    breakStack_.emplace_back();
    flowStatement(body);
    const BreakStates breaks = std::move(breakStack_.back());
    breakStack_.pop_back();

    // The step runs after the body, so it is checked with what the body left --
    // but it contributes nothing to the exit state, because the exit that skips
    // it (the condition being false first) is always possible.
    const std::vector<ast::AstId> stepOperands = operandsOf(step);
    if (!stepOperands.empty()) {
      flowExpression(stepOperands.front());
    }

    assigned_ = loopExit(entry, breaks, conditionExpr);
    return;
  }
  case ast::NodeKind::BreakStmt:
    // Recorded against the innermost loop, which is the one a stack makes
    // innermost. A `break` in a nested loop lands in that loop's list and cannot
    // be credited to this one.
    if (!breakStack_.empty()) {
      breakStack_.back().push_back(assigned_);
    }
    return;
  case ast::NodeKind::ContinueStmt:
  case ast::NodeKind::EmptyStmt:
    // `continue` leaves the iteration and not the loop, so it is not an exit the
    // state after the loop has to consider.
    return;
  default:
    // A node at statement position the analysis does not know: walk into it
    // rather than skipping it, so a name nested inside is still checked.
    for (const ast::AstId child : operandsOf(stmt)) {
      flowStatement(child);
    }
    return;
  }
}

// --- expressions -------------------------------------------------------------

void Checker::flowExpression(ast::AstId expr) {
  if (!expr.valid() || inError(expr)) {
    return;
  }

  switch (kindOf(expr)) {
  case ast::NodeKind::LiteralExpr:
    return;
  case ast::NodeKind::PathExpr: {
    if (const std::optional<resolve::DefId> def = defOfPath(expr)) {
      reportIfUnassigned(*def, expr);
    }
    return;
  }
  case ast::NodeKind::ParenExpr: {
    const std::vector<ast::AstId> operands = operandsOf(expr);
    if (!operands.empty()) {
      flowExpression(operands.front());
    }
    return;
  }
  case ast::NodeKind::BinaryExpr: {
    const std::vector<ast::AstId> operands = operandsOf(expr);
    if (operands.size() < 2) {
      for (const ast::AstId operand : operands) {
        flowExpression(operand);
      }
      return;
    }
    const Tag kind = tagOf(kindOf(tokenOf(expr)));
    flowExpression(operands[0]);
    const AssignmentSet afterLeft = assigned_;
    flowExpression(operands[1]);
    if (kind == kTokAmpAmp || kind == kTokPipePipe) {
      // Short-circuit: the right side runs only when the left side says so, so
      // what it assigns cannot be assumed after the operator.
      intersectWith(assigned_, afterLeft);
    }
    return;
  }
  case ast::NodeKind::ConditionalExpr: {
    const std::vector<ast::AstId> operands = operandsOf(expr);
    if (operands.size() < 3) {
      for (const ast::AstId operand : operands) {
        flowExpression(operand);
      }
      return;
    }
    flowExpression(operands[0]);
    const AssignmentSet afterCondition = assigned_;
    flowExpression(operands[1]);
    const AssignmentSet afterThen = assigned_;
    assigned_ = afterCondition;
    flowExpression(operands[2]);
    intersectWith(assigned_, afterThen);
    return;
  }
  case ast::NodeKind::AssignExpr: {
    const std::vector<ast::AstId> operands = operandsOf(expr);
    if (operands.size() < 2) {
      for (const ast::AstId operand : operands) {
        flowExpression(operand);
      }
      return;
    }
    const Tag kind = tagOf(kindOf(tokenOf(expr)));
    if (kind == kTokEqual) {
      // A plain assignment stores: the target is not read, and storing into a
      // binding that has never held a value is the legal half of the rule.
      flowStoreTarget(operands[0]);
    } else {
      // `x += e` reads `x` before it writes it.
      flowExpression(operands[0]);
    }
    flowExpression(operands[1]);
    markAssigned(operands[0]);
    return;
  }
  case ast::NodeKind::PrefixExpr: {
    const std::vector<ast::AstId> operands = operandsOf(expr);
    if (operands.empty()) {
      return;
    }
    const Tag kind = tagOf(kindOf(tokenOf(expr)));
    // `&x` reads nothing -- it names a place -- so its operand is walked as a
    // place and an unassigned `x` is not a read here. `memory.md` says taking an
    // address accesses no bytes, and the read that is a violation is the one
    // through the pointer, which the checked build traps because no static pass
    // can follow the address. This is the same split `*p = v` makes below.
    if (kind == kTokAmp) {
      flowStoreTarget(operands.front());
      return;
    }
    // `++x` reads and writes; `-x`, `!x` and `~x` only read, and the read is the
    // same check.
    flowExpression(operands.front());
    if (kind == kTokPlusPlus || kind == kTokMinusMinus) {
      markAssigned(operands.front());
    }
    return;
  }
  case ast::NodeKind::PostfixExpr: {
    const std::vector<ast::AstId> operands = operandsOf(expr);
    if (operands.empty()) {
      return;
    }
    flowExpression(operands.front());
    markAssigned(operands.front());
    return;
  }
  case ast::NodeKind::SliceExpr: {
    // Taking a view *reads*: the bounds are values, and the base is read in every
    // form but one. An **array** is the exception, and it is the same exception
    // `a[i]` gets: computing the address of an element reads no byte of the
    // object, so `a[0..1]` is legal on a binding that has never held a value --
    // which is what lets `let a: [4]i32; let v: []i32 = a[0..2];` be the two lines
    // it says it is. A **slice** and a **pointer** are values, and viewing one
    // reads it (`arrays.md` decision 23, `slices.md`).
    const ast::SliceParts parts = file_.slicePartsOf(expr);
    if (!parts.hasBase()) {
      return;
    }
    if (types_.isArray(out_.typed.typeOf(parts.base))) {
      flowStoreTarget(parts.base);
    } else {
      flowExpression(parts.base);
    }
    flowExpression(parts.begin);
    flowExpression(parts.end);
    return;
  }
  case ast::NodeKind::CallExpr:
    // The callee and then the arguments, left to right: the order the language
    // guarantees and the order the lowering must produce.
    for (const ast::AstId operand : operandsOf(expr)) {
      flowExpression(operand);
    }
    return;
  default:
    // An expression node the analysis does not know: walk it rather than skip
    // it, so nothing nested inside goes unchecked.
    for (const ast::AstId child : operandsOf(expr)) {
      flowExpression(child);
    }
    return;
  }
}

void Checker::flowStoreTarget(ast::AstId place) {
  // Parentheses are not part of the place (`(x) = 1` is legal), so they are seen
  // through, exactly as `defOfPlace` does.
  ast::AstId current = place;
  while (current.valid() && kindOf(current) == ast::NodeKind::ParenExpr) {
    const std::vector<ast::AstId> operands = operandsOf(current);
    current = operands.empty() ? ast::AstId{} : operands.front();
  }
  if (!current.valid()) {
    return;
  }
  if (kindOf(current) == ast::NodeKind::PathExpr) {
    return; // a plain store: nothing here is read
  }
  if (kindOf(current) == ast::NodeKind::IndexExpr) {
    const std::vector<ast::AstId> operands = operandsOf(current);
    if (operands.size() < 2) {
      flowExpression(current);
      return;
    }
    // The base decides which of the two spellings of `[i]` this is, and the two
    // differ in what the store reads.
    //
    // An **array** is an object, and reaching one of its elements computes an
    // address: none of its bytes are read, so `a[0] = 1` and `&a[0]` are legal on
    // a binding that has never held a value (`arrays.md` decision 23). A
    // **pointer** is a value, and `p[i] = v` is `*(p + i) = v`, which reads `p`.
    if (types_.isArray(out_.typed.typeOf(operands[0]))) {
      flowStoreTarget(operands[0]);
    } else {
      flowExpression(operands[0]);
    }
    // The index is a value in both spellings, so it is the half that has to hold
    // one.
    flowExpression(operands[1]);
    return;
  }
  // Anything richer than a name will compute an address out of values, and that
  // computation reads whatever it is built from -- `*p = 1` reads `p`.
  flowExpression(current);
}

void Checker::markAssigned(ast::AstId place) {
  // `defOfStoreTarget` and not `defOfPlace`: the two questions look alike and
  // are opposite here. A store into `a[0]` *belongs* to `a` -- which is what
  // makes `TABLE[0] = 1` refused on a `const` table -- and it does **not** give
  // the object a value, because the other elements were never written. Reading
  // `a[1]` after writing `a[0]` is reading an unassigned object, and marking the
  // whole binding here would be the analysis believing something false
  // (`arrays.md` decisions 23, 24).
  if (const std::optional<resolve::DefId> def = defOfStoreTarget(place)) {
    if (def->index < assigned_.size()) {
      assigned_[def->index] = true;
    }
  }
}

void Checker::reportIfUnassigned(resolve::DefId def, ast::AstId at) {
  const resolve::Def* declaration = defFor(def);
  if (declaration == nullptr) {
    return;
  }
  // Only a `let` can be declared without a value: a parameter has one when the
  // body starts, a `const` must have one (`validate`), and a function name is
  // not a value. Asking this of the *kind* is what keeps the pass from having an
  // opinion about anything but the one case that has no value to read.
  if (declaration->kind != resolve::DefKind::Variable) {
    return;
  }
  // A **file-scope** binding is initialized before the program runs: its bytes
  // are written by the compiler -- the value, or zero if the source gave none
  // (`globals.md`, decisions 2 and 4). There is no path into a function that
  // reaches one unassigned, so this analysis, which is about the paths *inside* a
  // function, has nothing to say about it. Without this the first read of a
  // file-scope `let` would be reported: the pass marks a binding assigned where
  // the source assigns it, and nothing in a body ever assigns one of these.
  if (declaration->scope == defs_.fileScope) {
    return;
  }
  if (def.index < assigned_.size() && assigned_[def.index]) {
    return;
  }
  // Reported once per binding: the fix is one assignment before the reads, so
  // the second read is the same sentence and not a second finding.
  if (def.index < reportedUnassigned_.size()) {
    if (reportedUnassigned_[def.index]) {
      return;
    }
    reportedUnassigned_[def.index] = true;
  }
  error(at, SemaErrorCode::UseBeforeAssignment,
        "`" + nameOf(at) +
            "` is read before it is assigned: not every path from its declaration gives it a "
            "value");
}

} // namespace minc::sema
