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
  Array,
};

[[nodiscard]] std::string_view toString(TypeKind kind);

struct Type {
  TypeKind kind = TypeKind::Error;
  // Int: signed or not. Char/Float: unused (`char` is unsigned by decision).
  bool isSigned = false;
  // Int/Float: width in bits. `f80` is 80. 0 for everything else.
  std::uint16_t bits = 0;
  // Reserved: the pointee of a `Pointer`, the element of an `Array`.
  TypeId pointee;
  // Function: what it returns, and its parameters, which live in the store's
  // parameter array at `[firstParam, firstParam + paramCount)`.
  TypeId returnType;
  std::uint32_t firstParam = 0;
  std::uint32_t paramCount = 0;
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

// `isize`/`usize` are deliberately **not** here. They are pointer-sized, and on
// LP64 the pointer-sized signed type *is* `i64`, so a distinct id would give two
// ids to one type and break the identity this file is built on. The specifier
// reader maps the names onto `signedInt(target.pointerBits)`, exactly as `long`
// maps onto the target's `long` width -- a spelling that resolves to a type, not
// a type of its own. The same applies to `size_t`/`ssize_t`/`ptrdiff_t`.

// How many ids the store pre-registers. Everything past this was interned while
// checking a unit.
inline constexpr std::uint32_t kFirstInternedType = 20;

} // namespace minc::sema
