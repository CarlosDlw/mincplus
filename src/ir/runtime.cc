// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The operations the language defines and the hardware does not.
//
// `sema`'s integer table says a division by zero, `INT_MIN / -1`, and a shift
// count outside `[0, width)` are *traps* -- not undefined behaviour, and not a
// wrapped value. LLVM's `sdiv`/`srem`/`shl` say the opposite: an undefined
// result, which the optimizer is free to turn into anything, including a wrong
// answer for a program that never reached it. So every one of them is emitted
// through here, with an explicit test and an `llvm.trap` on the failing edge.
//
// Two reasons this is one file and not a branch at each call site. The mapping
// in `expr.cc` is the *only* place these opcodes appear, so the guard cannot be
// forgotten by a construct added later; and the shape of the guard is one shape,
// which is what makes it checkable (`invariants.cc` says a division's block is
// entered through a conditional branch that tests the divisor).
#include "lowering.h"

#include <cstdint>

#include "llvm/ADT/APInt.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"

namespace minc::ir {
namespace {

[[nodiscard]] llvm::Constant* allOnes(llvm::IntegerType* type) {
  return llvm::ConstantInt::get(type, llvm::APInt::getAllOnes(type->getBitWidth()));
}

} // namespace

void Lowering::trapBlock() {
  // `llvm.trap` and not a call to `abort`: it is the language's trap, it is what
  // the checked build reaches, and it is declared by the intrinsic machinery
  // rather than by a header the module would have to link.
  llvm::Function* trap = llvm::Intrinsic::getOrInsertDeclaration(&module_, llvm::Intrinsic::trap);
  builder_.CreateCall(trap);
  builder_.CreateUnreachable();
}

Value Lowering::checkedDiv(const Value& lhs, const Value& rhs, sema::TypeId opType,
                           bool isRemainder) {
  if (lhs.v == nullptr || rhs.v == nullptr) {
    return {};
  }
  auto* type = llvm::dyn_cast<llvm::IntegerType>(llvmType(opType));
  if (type == nullptr) {
    fatal(support::Span{}, IRDiagnosticCode::Internal,
          "a division reached lowering with a non-integer operation type");
    return {};
  }
  const bool signedOp = isSigned(opType);

  llvm::BasicBlock* trapBB = llvm::BasicBlock::Create(context_, "div.trap", current_);
  llvm::BasicBlock* okBB = llvm::BasicBlock::Create(context_, "div.ok", current_);

  // The divisor. A `null` here is the zero test, and it is what the invariant
  // scan recognises: the block the division lives in is entered only through a
  // branch that tests the divisor, so a bare `sdiv` is impossible to emit by
  // accident.
  llvm::Value* bad = builder_.CreateICmpEQ(rhs.v, llvm::ConstantInt::get(type, 0), "div.zero");
  if (signedOp) {
    // `INT_MIN / -1` overflows in two's complement, and the hardware traps on
    // x86 while LLVM calls it poison. It is one more comparison, and it makes
    // the operation total.
    llvm::Value* isNegativeOne = builder_.CreateICmpEQ(rhs.v, allOnes(type), "div.negone");
    llvm::Value* isMinimum = builder_.CreateICmpEQ(
        lhs.v, llvm::ConstantInt::get(type, llvm::APInt::getSignedMinValue(type->getBitWidth())),
        "div.min");
    llvm::Value* overflow = builder_.CreateAnd(isNegativeOne, isMinimum, "div.overflow");
    bad = builder_.CreateOr(bad, overflow, "div.bad");
  }
  builder_.CreateCondBr(bad, trapBB, okBB);

  builder_.SetInsertPoint(trapBB);
  trapBlock();

  builder_.SetInsertPoint(okBB);
  llvm::Value* result = nullptr;
  if (isRemainder) {
    result = signedOp ? static_cast<llvm::Value*>(builder_.CreateSRem(lhs.v, rhs.v, "rem"))
                      : static_cast<llvm::Value*>(builder_.CreateURem(lhs.v, rhs.v, "urem"));
  } else {
    result = signedOp ? static_cast<llvm::Value*>(builder_.CreateSDiv(lhs.v, rhs.v, "div"))
                      : static_cast<llvm::Value*>(builder_.CreateUDiv(lhs.v, rhs.v, "udiv"));
  }
  return Value{result, opType};
}

Value Lowering::checkedShift(const Value& value, const Value& count, sema::TypeId opType,
                             bool left) {
  if (value.v == nullptr || count.v == nullptr) {
    return {};
  }
  auto* type = llvm::dyn_cast<llvm::IntegerType>(llvmType(opType));
  if (type == nullptr) {
    fatal(support::Span{}, IRDiagnosticCode::Internal,
          "a shift reached lowering with a non-integer operation type");
    return {};
  }
  const bool signedOp = isSigned(opType);
  const std::uint16_t bits = bitsOf(opType);

  llvm::BasicBlock* trapBB = llvm::BasicBlock::Create(context_, "shift.trap", current_);
  llvm::BasicBlock* okBB = llvm::BasicBlock::Create(context_, "shift.ok", current_);

  // The count's range is the *value's* width, not the count's own: `x << 32` on
  // an `i32` is what the language refuses, and a count of `33` on a `u8`-backed
  // `i32` operation is legal because the operation is at `i32`.
  llvm::Value* bad =
      builder_.CreateICmpUGE(count.v, llvm::ConstantInt::get(type, bits), "shift.too.big");
  if (signedOp) {
    llvm::Value* negative =
        builder_.CreateICmpSLT(count.v, llvm::ConstantInt::get(type, 0), "shift.negative");
    bad = builder_.CreateOr(negative, bad, "shift.bad");
  }
  builder_.CreateCondBr(bad, trapBB, okBB);

  builder_.SetInsertPoint(trapBB);
  trapBlock();

  builder_.SetInsertPoint(okBB);
  llvm::Value* result = nullptr;
  if (left) {
    result = builder_.CreateShl(value.v, count.v, "shl");
  } else {
    // An arithmetic shift for a signed value and a logical one for an unsigned
    // one. LLVM spells the distinction `ashr`/`lshr` because its types are
    // signless, which is why `sema`'s `Type` is the only thing that can answer
    // this question and why it is answered here from the store.
    result = signedOp ? static_cast<llvm::Value*>(builder_.CreateAShr(value.v, count.v, "ashr"))
                      : static_cast<llvm::Value*>(builder_.CreateLShr(value.v, count.v, "lshr"));
  }
  return Value{result, opType};
}

} // namespace minc::ir
