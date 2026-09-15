// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// One function's body, and the block bookkeeping every construct shares.
//
// The shape of a function is fixed here and nowhere else: an entry block, the
// allocas in it, the parameters stored into those allocas, the body, and the
// terminator the language implies when control runs off the end. Everything
// below this file builds onto a block that already exists, which is what keeps a
// statement from having to know how a frame is made.
#include "lowering.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"

#include "ast/node.h"
#include "debug.h"
#include "sema/typed_ast.h"
#include "support/intern/sym_id.h"

namespace minc::ir {

void Lowering::defineFunction(const sema::FunctionInfo& info) {
  if (failed_ || !info.body.valid() || inError(info.body)) {
    return;
  }
  const ast::AstId nameNode = childOf(info.decl, ast::NodeKind::Name);
  const std::optional<resolve::DefId> def = defAtName(nameNode);
  if (!def.has_value()) {
    error(info.decl.valid() ? info.decl : file_.root(), IRDiagnosticCode::Internal,
          "a function body reached lowering with no declaration behind its name");
    failed_ = true;
    return;
  }
  const auto found = functions_.find(defKey(*def));
  if (found == functions_.end()) {
    fatal(spanOf(info.decl), IRDiagnosticCode::Internal,
          "a function body reached lowering before its signature was declared");
    return;
  }
  llvm::Function* function = found->second;
  if (!function->empty()) {
    // Two bodies for one def. `sema` already reported `sema-function-redefinition`,
    // so this is not a second diagnostic about the program: it is the
    // *unchecked* case -- a caller that ran this stage over a tree it was told
    // had an error -- and stopping is what keeps the module one body per symbol.
    return;
  }

  // The function's debug scope is opened before the entry block, so the block's
  // own instructions and the parameters stored into their slots all carry a line
  // number rather than the file scope's.
  if (debug_ != nullptr) {
    const std::string symbol = linkageName(*def);
    debug_->enterFunction(*function, symbol, symbol, types_, info.functionType,
                          nameNode.valid() ? spanOf(nameNode) : spanOf(info.decl));
  }

  llvm::BasicBlock* entry = llvm::BasicBlock::Create(context_, "entry", function);
  builder_.SetInsertPoint(entry);
  locate(info.decl);
  // Frame slots live in the entry block and never in a conditional one: an
  // `alloca` inside a loop grows the frame on every iteration, and every binding
  // in this language has a fixed size, so there is no reason to allow it.
  // `declareLocal` seats the alloca builder per slot; this records the block it
  // seats it in.
  entryBlock_ = entry;
  allocaBuilder_.SetInsertPoint(entry);

  current_ = function;
  currentReturn_ = info.returnType;
  // One flat map per function, cleared here: a `DefId` is unique across the
  // unit, so a key from a previous function cannot be found by accident, but
  // holding every frame of every function alive would keep an `alloca` of a
  // function that has already been finished.
  locals_.clear();

  // The parameters, stored into their own slots. This is what makes a parameter
  // a *place* like every other binding: `&p` takes the address of the slot, and
  // an assignment to `p` writes it, with no second rule for parameters.
  const std::span<const sema::TypeId> params = types_.paramsOf(info.functionType);
  const ast::AstId paramList = childOf(info.decl, ast::NodeKind::ParamList);
  // An aggregate return puts its destination *first* in the argument list, so
  // every parameter's index moves by one (`arrays.md` decision 13). The shift is
  // computed once, here, rather than being remembered at each `getArg` below.
  if (types_.isAggregate(info.returnType) && function->arg_size() > 0) {
    sretPointer_ = function->getArg(0);
  }
  std::size_t index = 0;
  if (paramList.valid()) {
    for (const ast::AstId param : file_.childrenOfKind(paramList, ast::NodeKind::Param)) {
      if (sretOffset() + index >= function->arg_size()) {
        // More parameters than the signature the function was declared with --
        // not reachable from `sema`'s own signature pass, so a bug here rather
        // than a statement about the program.
        fatal(spanOf(param), IRDiagnosticCode::Internal,
              "this function has more parameters than its declared signature");
        return;
      }
      const ast::AstId paramName = childOf(param, ast::NodeKind::Name);
      const std::optional<resolve::DefId> paramDef = defAtName(paramName);
      const sema::TypeId paramType = index < params.size() ? params[index] : sema::kTypeError;
      if (paramDef.has_value()) {
        std::string_view name{};
        if (paramDef->index < defs_.defs.size() &&
            defs_.defs[paramDef->index].name != support::kInvalidSym) {
          name = symbols_.lookup(defs_.defs[paramDef->index].name);
        }
        // The parameter's own node is the location, so a debugger stops on the
        // parameter the reader wrote and not on the function's first line.
        const ast::AstId paramAt = paramName.valid() ? paramName : param;
        llvm::Argument* argument = function->getArg(static_cast<unsigned>(sretOffset() + index));
        if (types_.isAggregate(paramType)) {
          // An aggregate arrives as a pointer to the caller's copy, and that
          // pointer **is** the parameter's storage: no second copy, and `&a`
          // names the object the callee owns for the call rather than a spill of
          // it (`arrays.md` decision 13).
          locals_.emplace(defKey(*paramDef), argument);
          if (debug_ != nullptr) {
            // The record goes at the front of the entry block: the storage is an
            // argument and not an instruction, so there is nothing to sit behind,
            // and "before the body runs" is where an `alloca`'s record sits too.
            debug_->declareParameterBinding(*argument, name, types_, paramType, spanOf(paramAt),
                                            entry->begin());
          }
        } else {
          llvm::AllocaInst* slot = declareLocal(*paramDef, paramType, name, paramAt);
          storePlace(Place{slot, paramType}, Value{argument, paramType}, ast::AstId{});
        }
      }
      ++index;
    }
  }

  lowerBlock(info.body);

  // The terminator for a function that runs off the end. A `void` function
  // returns; anything else is the case `sema` already refused (a non-void
  // function may not reach its end), so an `unreachable` is the honest shape and
  // the module is still well formed for a tree that got here some other way.
  llvm::BasicBlock* last = builder_.GetInsertBlock();
  if (last != nullptr && last->getTerminator() == nullptr) {
    if (types_.isVoid(info.returnType) || types_.isAggregate(info.returnType)) {
      // An aggregate return falls off the end the same way a `void` one does: the
      // destination is the caller's storage and it was written by the `return`s,
      // and a function with no `return` at all is already refused. `sema` is what
      // refuses it; this keeps the module well formed regardless.
      builder_.CreateRetVoid();
    } else {
      builder_.CreateUnreachable();
    }
  }
  terminateDangling(function);

  if (debug_ != nullptr) {
    debug_->leaveFunction();
  }
  current_ = nullptr;
  entryBlock_ = nullptr;
  sretPointer_ = nullptr;
  currentReturn_ = sema::kInvalidType;
  locals_.clear();
}

void Lowering::terminateDangling(llvm::Function* function) {
  // A construct that ends both of its arms in a `return` leaves its join block
  // with no predecessor and no terminator -- `if c { return 1; } else { return 2; }`
  // is the two-line version of it. `unreachable` is what the block is: nothing
  // reaches it, and the optimizer deletes it along with the branch it was the
  // target of. Without this pass the module would fail LLVM's own verifier, and
  // a verifier failure is the one kind of failure this stage must not have.
  llvm::IRBuilder<> fix(context_);
  for (llvm::BasicBlock& block : *function) {
    if (block.getTerminator() != nullptr) {
      continue;
    }
    fix.SetInsertPoint(&block);
    fix.CreateUnreachable();
  }
}

// --- blocks -------------------------------------------------------------------

void Lowering::lowerBlock(ast::AstId block) {
  if (!block.valid() || failed_) {
    return;
  }
  locate(block);
  for (const ast::AstId stmt : operandsOf(block)) {
    if (failed_) {
      return;
    }
    // After a `return`, `break` or `continue` the current block has a terminator
    // and cannot take another instruction. The statements that follow are
    // unreachable -- the checker warned about each one -- and they are still
    // *lowered*, into a fresh block, because a dropped statement is a name or a
    // call that never reaches the module, and that omission does not show up
    // until somebody adds a second effect to it.
    if (builder_.GetInsertBlock() != nullptr &&
        builder_.GetInsertBlock()->getTerminator() != nullptr) {
      builder_.SetInsertPoint(deadBlock());
    }
    lowerStatement(stmt);
  }
}

llvm::BasicBlock* Lowering::deadBlock() {
  return llvm::BasicBlock::Create(context_, "dead", current_);
}

void Lowering::branchTo(llvm::BasicBlock* next) {
  if (next == nullptr || builder_.GetInsertBlock() == nullptr) {
    return;
  }
  if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
    builder_.CreateBr(next);
  }
}

} // namespace minc::ir
