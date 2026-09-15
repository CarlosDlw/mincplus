// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The conversions, in one place.
//
// C's implicit conversions are the language's arithmetic, not an optimisation:
// they are specified (C17 6.3.1.8 for the usual arithmetic conversions, 6.3.1 for
// the promotions, 6.5.16 for simple assignment), and a compiler that gets them
// wrong computes a different program. So they live here, as pure functions over
// types, and every operator and every statement calls the same three.
//
// The two places this language deliberately departs from C are named where they
// happen, not spread through the checker:
//
//   * `bool` and `str` are **not** arithmetic (C promotes `bool` to `int`);
//   * a condition must be `bool` (C accepts any scalar).
#pragma once

#include <cstdint>

#include "sema/type.h"
#include "sema/type_store.h"
#include "support/consteval/const_int.h"

namespace minc::sema {

// Integer promotion: `bool`, `char`, and every integer narrower than `int` widen
// to `i32`, which can represent all of their values. A wider integer, a float, a
// deferred literal, and the poison are returned unchanged.
[[nodiscard]] TypeId promote(TypeStore& types, TypeId type);

// The common type of two arithmetic operands -- what `a + b` produces, what a
// comparison converts to before comparing, and what `?:` unifies its arms to.
//
// Deferred literals are handled here rather than at each operator: two of one
// class defer together, and one adopts the other side within its own class
// (promoted, so `1 + u8` is `i32`). An **integer and a float have no common
// type**: neither is converted to the other (`convertible`), so the answer is
// `Error` and the caller reports it. The poison spreads, and a non-arithmetic
// operand yields `Error` too -- the caller has already reported it, and this must
// not report a second time.
[[nodiscard]] TypeId usualArithmetic(TypeStore& types, TypeId left, TypeId right);

// The type an operator's operands are converted to before it runs, which is also
// the type an integer result is computed in.
//
// For a shift it is the **promoted left operand** and the count is promoted on
// its own (C 6.5.7 promotes each operand separately); for every other operator
// it is the common type. It is one function rather than two call sites doing it,
// because `x << n` and `x <<= n` have to agree about the width -- and they did
// not: the compound form accepted `x <<= 40` on an `i8`, which is an
// out-of-range shift at the promoted width, and therefore a poison value in the
// backend rather than a diagnostic here.
[[nodiscard]] TypeId operationType(TypeStore& types, bool shift, TypeId left, TypeId right);

// Whether the assignment conversion applies: initializer, assignment, argument,
// `return`. Arithmetic converts to arithmetic of its **own class** -- an integer
// to an integer, a float to a float, narrower to wider -- and the narrowing
// within a class is silent, which is why the diagnosis for it is the
// `-Wconversion` lint rather than an error. An integer and a float do not convert
// into each other in either direction: the class of a number is the class of its
// spelling, and crossing is a cast. `bool` converts only to `bool`, `str` only to
// `str`, pointers only as `memory.md` says, and the poison to everything.
//
// Pointers convert to pointers of the **same pointee**, and to and from `*void`
// -- the untyped pointer, and the only one that crosses. `i32` to `*i32` is not
// a conversion in either direction: a pointer is not an integer, and the two
// operations that join them are named and counted (`memory.md`, *Provenance*).
[[nodiscard]] bool convertible(const TypeStore& types, TypeId from, TypeId to);

// True when one type is an integer and the other is a float: the one pair of
// arithmetic types that does not convert, in either direction. Exported because
// two diagnostics ask it -- the refusal of `let x: f64 = 1;` and the refusal of
// `1 + 2.0` are one rule and deserve one explanation -- and because "are these
// two mixed numbers" is a question about the types and not about either checker.
[[nodiscard]] bool mixedNumberPair(const TypeStore& types, TypeId from, TypeId to);

// True when the conversion may lose information: a float to an integer, a wider
// integer to a narrower one, a signed to an unsigned of the same or smaller
// width. Only `-Wconversion` reads this.
[[nodiscard]] bool narrows(const TypeStore& types, TypeId from, TypeId to);

// Does `value` fit the type's range? Used when a deferred literal takes the type
// its context gave it -- `let x: u8 = 256;` is one diagnostic at the literal
// instead of C's silent truncation. Types the 64-bit core cannot bound (`i128`,
// `u128`) answer `true`, and the caller has already refused a literal that did
// not fit the core at all.
[[nodiscard]] bool fitsIn(const TypeStore& types, TypeId type, support::ConstInt value);

} // namespace minc::sema
