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

#include <optional>

#include "llvm/IR/Value.h"

#include "sema/type.h"

namespace minc::ir {

// A computed value: a register, a constant, an address used as a value.
struct Value {
  llvm::Value* v = nullptr;
  sema::TypeId type = sema::kInvalidType;
};

// The two values a bounds guard compares, produced by the place that knows them.
//
// It is carried on the `Place` and not looked up again at the access because the
// access no longer has them: by then the address is one value, and the index and
// the extent are values the *place* computed on the way (`checks.md`).
struct BoundsGuard {
  // The index, already materialised at the pointer index width -- the same value
  // the `getelementptr` steps by, so a guard cannot disagree with the address it
  // guards.
  llvm::Value* index = nullptr;
  // What the index has to be below: a constant count for an array object, or the
  // `len` word of the descriptor a slice is.
  llvm::Value* extent = nullptr;
};

// An address, and what lives there. Never loaded implicitly: a consumer that
// wants the value asks for the load, which is what makes "the alignment comes
// from the access record" true at every read and write.
//
// The address is the object itself when the place names a binding; when it is an
// element of something, `bounds` carries the evidence. Whether a guard is
// *emitted* is not decided here -- it is read from the access record at the
// access, which is the one place both a read and a write pass through.
struct Place {
  // A constructor and not an aggregate, because the third member is *evidence*
  // rather than a field every construction site has: a place that names a binding
  // has an address and a type and nothing else to say, and an aggregate would make
  // each of those sites write the nothing out (`-Wmissing-field-initializers`, which
  // this project builds with, is the compiler saying the same thing).
  Place() = default;
  Place(llvm::Value* address, sema::TypeId what) : addr(address), type(what) {}

  llvm::Value* addr = nullptr;
  sema::TypeId type = sema::kInvalidType;
  // Set by the producer that knows them; read by the checked build's bounds guard
  // (`checks.cc`).
  std::optional<BoundsGuard> bounds;
};

} // namespace minc::ir
