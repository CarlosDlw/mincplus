// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Statements.
//
// Every construct here is one shape: create the blocks, branch into the first,
// lower the parts into the blocks they belong to, and finish with `branchTo` so
// the join is reached by whichever arm did not already leave. The blocks are
// created before anything is lowered into them, which is what lets a nested
// construct branch forward to a block that does not exist yet.
#include "lowering.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"

#include "ast/node.h"
#include "debug.h"
#include "sema/type.h"
#include "sema/typed_ast.h"
#include "support/intern/sym_id.h"

namespace minc::ir {
namespace {

// A condition as the `i1` a branch takes. The checker already proved it is a
// `bool`, and `bool` maps to `i1`, so this is an identity on every tree that
// reached here -- and the truncation below is what keeps a tree that did *not*
// from producing an invalid module instead of a crash.
[[nodiscard]] llvm::Value* asBool(llvm::IRBuilder<>& builder, const Value& value,
                                  llvm::IntegerType* boolType) {
  if (value.v == nullptr) {
    return nullptr;
  }
  if (value.v->getType() == boolType) {
    return value.v;
  }
  return builder.CreateICmpNE(value.v, llvm::ConstantInt::get(value.v->getType(), 0), "cond");
}

} // namespace

void Lowering::lowerStatement(ast::AstId stmt) {
  if (!stmt.valid() || failed_ || inError(stmt)) {
    return;
  }
  // One call here is total coverage for statements: every instruction the
  // statement emits inherits this location from the builder, so a later addition
  // to a lower-case here cannot lose its line number by forgetting to set one.
  locate(stmt);
  switch (kindOf(stmt)) {
  case ast::NodeKind::LetStmt:
  case ast::NodeKind::ConstStmt:
    lowerBinding(stmt);
    return;
  case ast::NodeKind::ReturnStmt:
    lowerReturn(stmt);
    return;
  case ast::NodeKind::ExprStmt: {
    const std::vector<ast::AstId> operands = operandsOf(stmt);
    if (!operands.empty()) {
      // The value is discarded, and that is the whole statement: an expression
      // statement is evaluated for its effects, and a call is the only effect
      // this grammar can have today.
      (void)lowerExpr(operands.front());
    }
    return;
  }
  case ast::NodeKind::Block:
    lowerBlock(stmt);
    return;
  case ast::NodeKind::IfStmt:
    lowerIf(stmt);
    return;
  case ast::NodeKind::WhileStmt:
    lowerWhile(stmt);
    return;
  case ast::NodeKind::ForStmt:
    lowerFor(stmt);
    return;
  case ast::NodeKind::BreakStmt:
    if (!loops_.empty()) {
      branchTo(loops_.back().end);
    }
    return;
  case ast::NodeKind::ContinueStmt:
    if (!loops_.empty()) {
      branchTo(loops_.back().condition);
    }
    return;
  case ast::NodeKind::EmptyStmt:
  case ast::NodeKind::TypeAliasDecl:
    // A name for a type is **not storage and not an instruction**: it is a word the
    // checker resolved to a `TypeId`, and by the time the lowering runs there is
    // nothing left of it but that id -- which is the whole of `type_alias.md`
    // decision 2, seen from here. The `-g` half of the declaration (its
    // `DW_TAG_typedef`) is built from the *positions* that wrote the name and not
    // from a walk of the declarations, so a declaration that no position used
    // produces no metadata at all, and one that did produces it where it was
    // needed.
    return;
  default:
    fatal(spanOf(stmt), IRDiagnosticCode::UnsupportedNode,
          "this statement is not lowered yet: " + std::string(parse::toString(kindOf(stmt))));
    return;
  }
}

void Lowering::lowerBinding(ast::AstId stmt) {
  const ast::AstId nameNode = childOf(stmt, ast::NodeKind::Name);
  const std::optional<resolve::DefId> def = defAtName(nameNode);
  if (!def.has_value()) {
    fatal(spanOf(stmt), IRDiagnosticCode::Internal,
          "a binding reached lowering with no declaration behind its name");
    return;
  }
  const sema::TypeId type = typeOf(stmt);
  if (!types_.known(type) || types_.isError(type)) {
    fatal(spanOf(stmt), IRDiagnosticCode::Internal,
          "a binding reached lowering with no type; the unit was not checked");
    return;
  }

  std::string_view name{};
  if (def->index < defs_.defs.size() && defs_.defs[def->index].name != support::kInvalidSym) {
    name = symbols_.lookup(defs_.defs[def->index].name);
  }
  // The annotation's own node travels with the binding: it is where `sema`
  // recorded the name this position was written with, and a debug record for `Rec`
  // that said `[8]u8` would be a debugger reporting a type the source never wrote
  // (`type_alias.md`, decision 8).
  llvm::AllocaInst* slot =
      declareLocal(*def, type, name, stmt, 0, aliasNameAt(childOf(stmt, ast::NodeKind::Type)));
  if (slot == nullptr) {
    // The binding's type could not be mapped, and the refusal is already
    // recorded. Nothing is stored: there is no object to store into.
    return;
  }

  // The initializer is the operand that is neither the name nor the type
  // annotation, which is the same rule the checker uses to find it.
  const ast::AstId typeNode = childOf(stmt, ast::NodeKind::Type);
  ast::AstId init;
  for (const ast::AstId operand : operandsOf(stmt)) {
    if (operand != nameNode && operand != typeNode) {
      init = operand;
    }
  }
  if (!init.valid()) {
    // `let x: i32;` -- a slot with no value. Nothing is stored, and the
    // definite-assignment pass is what proves it is never read before some
    // other statement writes it.
    return;
  }
  const Value value = lowerOperand(stmt, init);
  if (value.v == nullptr) {
    return;
  }
  storePlace(Place{slot, type}, value, ast::AstId{});
}

void Lowering::lowerReturn(ast::AstId stmt) {
  const std::vector<ast::AstId> operands = operandsOf(stmt);
  const ast::AstId expr = operands.empty() ? ast::AstId{} : operands.front();

  if (types_.isVoid(currentReturn_) || types_.isNever(currentReturn_) || !expr.valid()) {
    // A `return;`, a value returned from a `void` function -- which the checker
    // already refused -- and a `return boom();` from a function whose return type
    // is `!`, which is the *only* way such a function may end (`never.md`). The
    // three are one case here because they are one shape in the module: no value
    // travels back. A `!` return in particular may not store the call's result
    // anywhere, because the call has no result -- it is a `void` instruction, and
    // a `ret` fed one is a malformed module.
    //
    // The expression is still lowered for its effects, because a call inside a
    // mistake is still a call the reader wrote.
    if (expr.valid()) {
      (void)lowerExpr(expr);
    }
    builder_.CreateRetVoid();
    return;
  }

  const Value value = lowerOperand(stmt, expr);
  if (value.v == nullptr) {
    builder_.CreateUnreachable();
    return;
  }
  // An aggregate is returned by **writing the caller's object**: the destination
  // arrived as the first argument, so the value is one store and the `ret` is
  // `void` (`arrays.md` decision 13). Nothing is spilled into the callee's frame
  // first, which is the point -- a `[1 << 20]i32` return moves through the
  // caller's storage, not through a copy of it.
  if (byReference(currentReturn_)) {
    if (sretPointer_ == nullptr) {
      fatal(spanOf(stmt), IRDiagnosticCode::Internal,
            "a function returning an aggregate has no destination to write");
      builder_.CreateUnreachable();
      return;
    }
    storePlace(Place{sretPointer_, currentReturn_}, value, ast::AstId{});
    builder_.CreateRetVoid();
    return;
  }
  builder_.CreateRet(value.v);
}

void Lowering::lowerIf(ast::AstId stmt) {
  const ast::AstId thenBlock = childOf(stmt, ast::NodeKind::Block);
  const ast::AstId elseClause = childOf(stmt, ast::NodeKind::ElseClause);

  ast::AstId condition;
  for (const ast::AstId operand : operandsOf(stmt)) {
    if (operand != thenBlock && operand != elseClause) {
      condition = operand;
      break;
    }
  }
  if (!condition.valid() || !thenBlock.valid()) {
    return;
  }

  llvm::BasicBlock* thenBB = llvm::BasicBlock::Create(context_, "if.then", current_);
  llvm::BasicBlock* elseBB = nullptr;
  llvm::BasicBlock* endBB = llvm::BasicBlock::Create(context_, "if.end", current_);
  if (elseClause.valid()) {
    elseBB = llvm::BasicBlock::Create(context_, "if.else", current_);
  }

  const Value conditionValue = lowerExpr(condition);
  llvm::Value* test = asBool(builder_, conditionValue, boolType());
  if (test != nullptr) {
    builder_.CreateCondBr(test, thenBB, elseBB != nullptr ? elseBB : endBB);
  }

  builder_.SetInsertPoint(thenBB);
  lowerBlock(thenBlock);
  branchTo(endBB);

  if (elseBB != nullptr) {
    builder_.SetInsertPoint(elseBB);
    for (const ast::AstId arm : operandsOf(elseClause)) {
      if (kindOf(arm) == ast::NodeKind::Block) {
        lowerBlock(arm);
      } else {
        lowerStatement(arm);
      }
    }
    branchTo(endBB);
  }

  builder_.SetInsertPoint(endBB);
}

void Lowering::lowerWhile(ast::AstId stmt) {
  const ast::AstId body = childOf(stmt, ast::NodeKind::Block);
  ast::AstId condition;
  for (const ast::AstId operand : operandsOf(stmt)) {
    if (operand != body) {
      condition = operand;
      break;
    }
  }
  if (!condition.valid() || !body.valid()) {
    return;
  }

  llvm::BasicBlock* condBB = llvm::BasicBlock::Create(context_, "while.cond", current_);
  llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(context_, "while.body", current_);
  llvm::BasicBlock* endBB = llvm::BasicBlock::Create(context_, "while.end", current_);

  branchTo(condBB);
  builder_.SetInsertPoint(condBB);
  // The condition is evaluated on *every* iteration, which is why it lives in
  // its own block: it is the continue target as well as the test.
  loops_.push_back(Loop{condBB, endBB});
  const Value conditionValue = lowerExpr(condition);
  llvm::Value* test = asBool(builder_, conditionValue, boolType());
  if (test != nullptr) {
    builder_.CreateCondBr(test, bodyBB, endBB);
  }
  builder_.SetInsertPoint(bodyBB);
  lowerBlock(body);
  branchTo(condBB);
  loops_.pop_back();
  builder_.SetInsertPoint(endBB);
}

void Lowering::lowerFor(ast::AstId stmt) {
  const ast::AstId forCondition = childOf(stmt, ast::NodeKind::ForCondition);
  const ast::AstId forStep = childOf(stmt, ast::NodeKind::ForStep);
  const ast::AstId body = childOf(stmt, ast::NodeKind::Block);
  if (!body.valid()) {
    return;
  }

  // The `for` statement is a scope of its own: the binding its initializer
  // declares is visible to the condition, the step and the body, and to nothing
  // else -- and that "nothing else" is what the debugger reads, so the scope is
  // opened here and closed after the loop's end block.
  const bool scoped = debug_ != nullptr && debug_->inFunction();
  if (scoped) {
    debug_->openBlock(spanOf(stmt));
  }
  // The initializer runs once, before the loop, in the enclosing block: it is a
  // statement of the enclosing scope as far as control flow is concerned, and
  // the binding it declares is visible to the condition, the step and the body
  // because `sema`'s scopes say so -- this stage only has to put the store
  // somewhere a single time.
  for (const ast::AstId child : operandsOf(stmt)) {
    switch (kindOf(child)) {
    case ast::NodeKind::LetStmt:
    case ast::NodeKind::ConstStmt:
    case ast::NodeKind::ExprStmt:
    case ast::NodeKind::EmptyStmt:
      lowerStatement(child);
      break;
    default:
      break;
    }
  }
  if (failed_) {
    if (scoped) {
      debug_->closeBlock();
    }
    return;
  }

  llvm::BasicBlock* condBB = llvm::BasicBlock::Create(context_, "for.cond", current_);
  llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(context_, "for.body", current_);
  llvm::BasicBlock* stepBB = llvm::BasicBlock::Create(context_, "for.step", current_);
  llvm::BasicBlock* endBB = llvm::BasicBlock::Create(context_, "for.end", current_);

  branchTo(condBB);
  builder_.SetInsertPoint(condBB);
  const std::vector<ast::AstId> condition = operandsOf(forCondition);
  if (!condition.empty()) {
    const Value conditionValue = lowerExpr(condition.front());
    llvm::Value* test = asBool(builder_, conditionValue, boolType());
    if (test != nullptr) {
      builder_.CreateCondBr(test, bodyBB, endBB);
    }
  } else {
    // An omitted condition is `true`: `for ;; {}` is an infinite loop, and the
    // branch is what makes it one rather than a fall-through.
    builder_.CreateBr(bodyBB);
  }

  builder_.SetInsertPoint(bodyBB);
  // `continue` in a `for` runs the step and re-tests, which is what makes the
  // step the loop's continue target and not the condition.
  loops_.push_back(Loop{stepBB, endBB});
  lowerBlock(body);
  branchTo(stepBB);
  loops_.pop_back();

  builder_.SetInsertPoint(stepBB);
  const std::vector<ast::AstId> step = operandsOf(forStep);
  if (!step.empty()) {
    (void)lowerExpr(step.front());
  }
  branchTo(condBB);

  if (scoped) {
    debug_->closeBlock();
  }
  builder_.SetInsertPoint(endBB);
}

} // namespace minc::ir
