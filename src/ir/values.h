// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// `Value` and `Place`: the two things an expression can evaluate to.
//
// The distinction is Clang's `LValue`/`RValue`, and it is made on the first day
// rather than when it becomes necessary, because the cost of not making it is
// paid by every construct added later. A `Value` has no address; a `Place` has no
// value until it is loaded; and neither is a substitute for the other, which is
// what stops "is this an assignment target" from being answered by looking at
// what the expression *is*.
//
// Both carry a `sema::TypeId` and not an `llvm::Type*`, and that is the second
// decision worth stating: the type is a *language* fact, and the LLVM type is
// derived from it on demand. Carrying the LLVM type instead would make the pair
// `(llvm value, llvm type)` look like it had all the information, and it does
// not -- whether a value is `i32` or `u32` is invisible in LLVM, and the
// signedness of the next instruction depends on it.
#pragma once

#include "llvm/IR/Value.h"

#include "sema/type.h"

namespace minc::ir {

// A computed value: a register, a constant, an address used as a value.
struct Value {
  llvm::Value* v = nullptr;
  sema::TypeId type = sema::kInvalidType;
};

// An address, and what lives there. Never loaded implicitly: a consumer that
// wants the value asks for the load, which is what makes "the alignment comes
// from the access record" true at every read and write.
struct Place {
  llvm::Value* addr = nullptr;
  sema::TypeId type = sema::kInvalidType;
};

} // namespace minc::ir
