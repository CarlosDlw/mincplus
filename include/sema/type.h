// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// A type, and the identity by which everything else refers to one.
//
// A type is an **interned value**: two types with the same structure are the
// same `TypeId`. That is not an optimisation, it is the answer to "are these the
// same type?", and getting it wrong is how a C compiler ends up treating `int`
// and `i32` as different types and refusing to link a header against a body.
// `i32`, `int` and `signed int` are one `TypeId` here; the spelling is a
// property of how a name was written, never of what it means.
//
// The shape is deliberately a small kind plus data, and not one enumerator per C
// spelling, because `long` is not a kind: it is `Int{Signed, 64}` on System V
// AMD64 and `Int{Signed, 32}` on LLP64. Which width a spelling means belongs to
// the *target*, and the target is a table -- `sema/target.h` -- not a branch.
//
// Reserving the kinds this language has not written yet (`Pointer`, `Array`,
// `Struct`, ...) is the same decision `resolve` made for its name spaces:
// adding one later would reshape every switch, and paying for it now is an
// enumerator.
#pragma once

#include <cstdint>
#include <string_view>

#include "support/intern/sym_id.h"

namespace minc::sema {

// Index into a compilation's `TypeStore`.
struct TypeId {
  std::uint32_t index = 0xFFFFFFFFu;

  [[nodiscard]] constexpr bool valid() const {
    return index != 0xFFFFFFFFu;
  }
  friend constexpr bool operator==(TypeId, TypeId) = default;
};

inline constexpr TypeId kInvalidType{};

// What a type *is*. `IntLiteral` and `FloatLiteral` are not kinds of value; they
// are the state of a literal whose type its context has not decided yet -- see
// `sema.md`, *Literals*. They are real types so that everything between the
// literal and the context is ordinary typing instead of a special case.
enum class TypeKind : std::uint8_t {
  // The poison: what a failed expression is typed as. It converts to and from
  // everything, silently, and never produces a second diagnostic.
  Error,
  Void,
  // The bottom type, written `!`. It is the type of an expression that never
  // produces a value -- a call to a function that never returns -- and so it has
  // no values at all: it *converts* to every other type, vacuously, because
  // every path that would have produced one is a path that never gets there.
  //
  // Deliberately not `Void`. `void` is "produces nothing and comes back"; this is
  // "never comes back", and the two differ at every consumer: a `void` value is
  // an error in a value position, while `!` in a value position is a program
  // that cannot reach it. Rust states the same distinction with the same
  // spelling, and refuses to write `!` outside a return type on stable
  // (`primitive.never`), which is the rule `typespec.cc` enforces here.
  Never,
  Bool,
  // Distinct from `i8`/`u8`, and always unsigned (README, decided).
  Char,
  Int,
  Float,
  // A NUL-terminated string; scalar, and *not* arithmetic.
  Str,
  Function,
  // Deferred literal types; defaulted to `i32`/`f64` when nothing decides them.
  IntLiteral,
  FloatLiteral,
  // Reserved. The kind exists so a switch written today keeps compiling when the
  // syntax that builds one lands.
  Pointer,
  // `[N]T`: an object of `count` elements, each of the element type. The count is
  // part of the *identity*, so `[4]i32` and `[8]i32` are two types -- which is
  // the whole decision and not a detail (`arrays.md` decision 1).
  Array,
};

[[nodiscard]] std::string_view toString(TypeKind kind);

struct Type {
  TypeKind kind = TypeKind::Error;
  // Int: signed or not. Char/Float: unused (`char` is unsigned by decision).
  bool isSigned = false;
  // Int/Float: width in bits. `f80` is 80. 0 for everything else.
  std::uint16_t bits = 0;
  // Pointer: the pointee. Array: the element type. The same field because the
  // two are never both meaningful -- a type is one or the other -- and a second
  // field would be one more thing every switch has to remember is dead.
  TypeId pointee;
  // Array: the element count, always >= 1. `std::uint64_t` because the count is
  // a folded value that must be comparable without narrowing first: it is read
  // once, by the type spec reader, from a literal the source wrote
  // (`arrays.md` decisions 19 and 20).
  std::uint64_t count = 0;
  // Function: what it returns, and its parameters, which live in the store's
  // parameter array at `[firstParam, firstParam + paramCount)`.
  TypeId returnType;
  std::uint32_t firstParam = 0;
  std::uint32_t paramCount = 0;
  // Function: the parameter list ends in `...`, so a call may pass more
  // arguments than `paramCount`. Part of the *type* and not a flag beside it,
  // because `f(i32)` and `f(i32, ...)` are different functions -- the same
  // reason LLVM's `FunctionType` carries `isVarArg`: a distinction the type does
  // not make is one every consumer has to remember, and a function pointer is
  // where forgetting it would be silent.
  bool variadic = false;
  // Reserved: a named type (`struct S`, a typedef).
  support::SymId name = support::kInvalidSym;
};

// --- the well-known types ----------------------------------------------------
//
// Fixed ids, assigned by every `TypeStore` in the same order, so a test can name
// one and a dump is byte-stable across runs. `TypeStore`'s constructor registers
// exactly this list; a `static_assert` in the store keeps the two in step.

inline constexpr TypeId kTypeError{0};
inline constexpr TypeId kTypeVoid{1};
inline constexpr TypeId kTypeBool{2};
inline constexpr TypeId kTypeChar{3};
inline constexpr TypeId kTypeIntLiteral{4};
inline constexpr TypeId kTypeFloatLiteral{5};
inline constexpr TypeId kTypeStr{6};

inline constexpr TypeId kTypeI8{7};
inline constexpr TypeId kTypeI16{8};
inline constexpr TypeId kTypeI32{9};
inline constexpr TypeId kTypeI64{10};
inline constexpr TypeId kTypeI128{11};

inline constexpr TypeId kTypeU8{12};
inline constexpr TypeId kTypeU16{13};
inline constexpr TypeId kTypeU32{14};
inline constexpr TypeId kTypeU64{15};
inline constexpr TypeId kTypeU128{16};

inline constexpr TypeId kTypeF32{17};
inline constexpr TypeId kTypeF64{18};
inline constexpr TypeId kTypeF80{19};

// `!`. Last in the registration order rather than next to `void`, which is where
// its kind sits: the id list is append-only, because every constant above is a
// promise to the dumps and the tests, and inserting in the middle would renumber
// `i32` to make a new type tidier.
inline constexpr TypeId kTypeNever{20};

// `isize`/`usize` are deliberately **not** here. They are pointer-sized, and on
// LP64 the pointer-sized signed type *is* `i64`, so a distinct id would give two
// ids to one type and break the identity this file is built on. The specifier
// reader maps the names onto `signedInt(target.pointerBits)`, exactly as `long`
// maps onto the target's `long` width -- a spelling that resolves to a type, not
// a type of its own. The same applies to `size_t`/`ssize_t`/`ptrdiff_t`.

// How many ids the store pre-registers. Everything past this was interned while
// checking a unit.
inline constexpr std::uint32_t kFirstInternedType = 21;

} // namespace minc::sema
