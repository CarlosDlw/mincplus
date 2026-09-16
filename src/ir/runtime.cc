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

Value Lowering::checkedFloatToInt(const Value& value, sema::TypeId to, support::Span at) {
  if (value.v == nullptr) {
    return {};
  }
  auto* destination = llvm::dyn_cast<llvm::IntegerType>(llvmType(to));
  if (destination == nullptr || !value.v->getType()->isFloatingPointTy()) {
    fatal(support::Span{}, IRDiagnosticCode::Internal,
          "a float-to-integer conversion reached lowering with the wrong pair of types");
    return {};
  }

  // **A constant operand is decided, never guarded** (`casts.md`, *Float →
  // integer*). The compiler knows the value, so a value the destination cannot
  // hold is a refusal about the program rather than a trap inside it, and a value
  // it can hold becomes the `ConstantInt` the run-time path would have produced.
  // Emitting the guard for one would also be the wrong shape: an `fcmp` of two
  // constants folds, which would leave an `fptosi` in a block no test can reach
  // -- and the scan would be right to call that unguarded.
  if (auto* constant = llvm::dyn_cast<llvm::ConstantFP>(value.v)) {
    if (!floatFitsInteger(constant->getValueAPF(), to)) {
      refuseCastOutOfRange(value.type, to, at);
      return {};
    }
    return Value{foldedInteger(constant->getValueAPF(), to), to};
  }

  // The bounds are *exact* powers of two, and the same two the constant check
  // above reads (`casts.md`). The comparison is on the **value** and not on its
  // truncation, so `-0.5 as u8` traps: a value the destination cannot hold is a
  // value the destination cannot hold, whatever its truncation would be.
  const FloatRange range = floatRangeOf(to);
  const llvm::fltSemantics& semantics = value.v->getType()->getFltSemantics();

  // `2^128` is past what an `f32` can name, and `fptosi` from an `f32` to an
  // `i128` is a real conversion: the operand then moves to `f64` **once**, and the
  // moved value is both what is tested and what converts -- so the scan's "the
  // block tests the operand" holds by pointer and not by a second name for it. An
  // `fpext` from a narrower format loses nothing, and `f64` holds every bound in
  // the store, so one step is enough for any destination the language has.
  llvm::Value* subject = value.v;
  const llvm::fltSemantics* compareIn = &semantics;
  if (!holdsBound(range.low, semantics) || !holdsBound(range.high, semantics)) {
    compareIn = &llvm::APFloat::IEEEdouble();
    subject = builder_.CreateFPExt(subject, llvm::Type::getDoubleTy(context_), "f2i.wide");
  }
  // The one step above is enough only if the wider format holds both bounds, and
  // that is proved rather than assumed: a destination past it (an `i512`) would
  // reach a `ConstantFP` of an infinity and a test that lets everything through.
  if (!holdsBound(range.low, *compareIn) || !holdsBound(range.high, *compareIn)) {
    fatal(at, IRDiagnosticCode::Internal,
          "the bounds of this float-to-integer conversion are past `f64`, which is the "
          "width this guard promotes to; a wider destination needs a wider comparison and "
          "not a bigger constant");
    return {};
  }
  const llvm::APFloat low = boundIn(*compareIn, range.low);
  const llvm::APFloat high = boundIn(*compareIn, range.high);

  llvm::BasicBlock* trapBB = llvm::BasicBlock::Create(context_, "f2i.trap", current_);
  llvm::BasicBlock* okBB = llvm::BasicBlock::Create(context_, "f2i.ok", current_);
  // **Unordered** comparisons, so a NaN is caught by the same two tests: it is
  // neither `>= low` nor `< high`, and an ordered pair would call it both and
  // take the trapping edge only by accident.
  llvm::Value* below =
      builder_.CreateFCmpULT(subject, llvm::ConstantFP::get(subject->getType(), low), "f2i.below");
  llvm::Value* above =
      builder_.CreateFCmpUGE(subject, llvm::ConstantFP::get(subject->getType(), high), "f2i.above");
  llvm::Value* bad = builder_.CreateOr(below, above, "f2i.bad");
  builder_.CreateCondBr(bad, trapBB, okBB);

  builder_.SetInsertPoint(trapBB);
  trapBlock();

  builder_.SetInsertPoint(okBB);
  // Reached only when the value is inside the range, so neither arm can produce
  // poison -- which is the whole point of the guard. The operand is `subject` and
  // not `value.v`, so a promoted conversion converts what it tested.
  llvm::Value* result =
      isSigned(to)
          ? static_cast<llvm::Value*>(builder_.CreateFPToSI(subject, destination, "fptosi"))
          : static_cast<llvm::Value*>(builder_.CreateFPToUI(subject, destination, "fptoui"));
  return Value{result, to};
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
