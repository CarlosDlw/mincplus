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

#include "support/constraint/constraint.h"
#include <string_view>

#include "support/intern/sym_id.h"
#include "support/span/file_id.h"

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
  // `[]T`: a **view** of `count`-unknown elements of the element type -- a
  // `{ptr, len}` descriptor, two words, aliasing storage somebody else owns
  // (`slices.md`). The element lives in the same `pointee` field an array's does,
  // and `count` stays 0, which is not a count this store ever holds: the two
  // aggregate kinds differ by exactly the thing that matters, `[4]i32` and
  // `[]i32` are different types, and which of the two a reader has is answered
  // by the kind rather than inferred from a length.
  Slice,
  // `(T, U, ...)`: a **product** of two or more types, in the order written
  // (`tuples.md`). It is the one kind whose arity is not fixed, and it holds its
  // members in the same sequence a function holds its parameters -- both are
  // "the types this type is made of", so identity, interning, the arity bound
  // and the dump shape are one mechanism and not two.
  //
  // Structural, with no name of its own: `(i32, bool)` is one id whatever
  // spelled it, which is what makes it usable as a return type, a parameter and
  // an element without a declaration anywhere. A product *with* names is a
  // `struct`, and that is a nominal kind when it lands -- it must not be this
  // one with a flag.
  Tuple,
  // A **type parameter**: the `T` of `fn T identity<T>(value: T)` and of
  // `type Pair<T, K> = (T, K);` (`generics.md`).
  //
  // It has identity of its own -- `(owner, binder)` and not its structure -- for
  // the one reason a structural store needs a nominal kind: `T` of one
  // declaration and `T` of another are *not* the same type, and a spelling is
  // not enough to tell them apart (two functions may each call their binder
  // `T`). The spelling is kept beside the identity because a diagnostic prints
  // it and it has no other source.
  //
  // A `Param` is what a signature holds *before* an instantiation. It is not a
  // value type: it has no width, so it cannot be stored, cannot be an array's
  // element, and reaching the lowering is an internal error -- the record's
  // invariant, because instantiation substitutes every one of them before a
  // module is built.
  Param,
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
  // Function: what it returns, and its parameters, which are the row of the
  // store's sequence table that `sequence` names. Tuple: the members, in the same
  // table and the same shape -- a tuple has no return type and a function has no
  // member list, so one pair of fields covers both and the identity rule
  // (`hashOf`/`equalParts`) never has to ask which kind it has.
  TypeId returnType;
  // The **row index** of that sequence, assigned by the store when the type is
  // appended and never written again. A row index and not an offset into one flat
  // array, and the reason is a caller's span: `paramsOf`/`membersOf` hand one out,
  // and a checker holds it while it reads what the span is made of -- which
  // interns types (the substituted signature of a generic call, a tuple built
  // while an argument is checked). Appending to the store must not move the row a
  // live span points at, and a row in a `deque` never moves.
  std::uint32_t sequence = 0;
  std::uint32_t paramCount = 0;
  // How many types this one is made of, children included: one for a type with
  // none, and one plus the children's for everything else. Derived, like
  // `sequence`, so it is neither hashed nor compared -- and **stored** rather
  // than computed, because computing it by walking is the very cost it exists to
  // bound (`support/limits.h`, `kMaxTypeNodes`): a caller that had to walk the
  // structure to ask whether the structure is too large would have already paid.
  std::uint32_t nodes = 1;
  // The object's layout, derived like `nodes` and **stored** for a sharper reason:
  // this store is a **DAG**, not a tree. `type P1 = (P0, P0);` interns one type
  // with two parents, so a recursive walk of the structure it spells out is
  // exponential in the depth while the store holds only a handful of types -- and
  // the layout is the question every stage asks per *use* (an array's element, a
  // variable's slot, a field's offset), so a walk per use is a walk per line.
  //
  // `size` is bytes, `align` is bytes and at least 1 wherever a layout exists, and
  // `unknownSize` is "the width is not decided yet" -- a type parameter, or an
  // aggregate built out of one -- which is why `[4]T` has no size (yet) and `void`
  // has none at all. The two zeros are different questions and `unknownSize` is
  // what tells them apart; `hasUnknownSize` is the one that asks.
  std::size_t size = 0;
  std::uint16_t align = 1;
  bool unknownSize = false;
  // Function: the parameter list ends in `...`, so a call may pass more
  // arguments than `paramCount`. Part of the *type* and not a flag beside it,
  // because `f(i32)` and `f(i32, ...)` are different functions -- the same
  // reason LLVM's `FunctionType` carries `isVarArg`: a distinction the type does
  // not make is one every consumer has to remember, and a function pointer is
  // where forgetting it would be silent.
  bool variadic = false;
  // Reserved: a named type (`struct S`, a typedef).
  support::SymId name = support::kInvalidSym;
  // Param: **the identity**, which is `(unit, owner, binder)` and not the
  // structure.
  //
  // `owner` is the declaring node's id in *its own* unit's tree, and a unit's
  // tree is numbered from zero (`LoweredFile::root()` is `AstId{0}`) -- so two
  // units both declare something at id 5, and "node 5" alone does not name a
  // declaration. `unit` is what makes the pair a name: one store serves the whole
  // compilation (`sema::Context` owns it and every input's `TypeId` indexes it),
  // while a tree is one file's, which is exactly the mismatch this field closes.
  //
  // Not the structure, and this is the whole point of a nominal kind here: two
  // declarations that both call their binder `T` get two types.
  support::FileId unit = support::kInvalidFile;
  std::uint32_t owner = 0;
  std::uint32_t binder = 0;
  // Param: what a diagnostic prints (`T`). Deliberately **not** part of the
  // identity: the identity is the pair above, and two binders with the same
  // spelling in two declarations are still two types. The view is owned by the
  // store's spelling pool, which never moves or shrinks an entry.
  std::string_view paramSpelling;
  // Param: the **constraint** the binder was declared with, which is what decides
  // which operations a body may perform on it and which type arguments may fill
  // it (`generics.md`, § 6, `support/constraint`).
  //
  // Kept beside the spelling and for the same reason, and like the spelling it is
  // **not** compared for identity (`equalFields` mixes the triple above): a class
  // is a fact *about* a binder, and two `Param`s differing only in their class
  // would be the same type declared twice. It is a pure function of
  // `(unit, owner, binder)` -- a binder list is read once, from one piece of
  // source -- so there is nothing to reconcile and nothing to keep in sync *as
  // long as the triple names one declaration*, which is the invariant the `unit`
  // field is there to hold.
  //
  // The default is `Any`, which is the class of a binder that wrote no constraint,
  // so `<T>` and `<T: Any>` are the same declaration and every generic body that
  // existed before constraints means exactly what it meant.
  support::ConstraintClass binderClass = support::ConstraintClass::Any;
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
