// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The compilation's interned types.
//
// One store per compilation, not per file: a function in one header returning
// `i32` has to produce the *same* `TypeId` as an `i32` in the `.mx` file that
// includes it, or nothing downstream can compare them. When translation units
// multiply this moves next to the session-level context, and nothing here
// changes.
//
// Interning is by structure, with a hash that finds candidates and `equal` that
// confirms -- the project's rule for every cache (`resolve/store.h` says why a
// hash alone would be a correctness bug), and this store's answer is handed to
// every later stage, so it is not the place for a probabilistic one.
//
// The built-ins are pre-registered in a fixed order so their ids are constants
// (`sema/type.h`), which is what makes a dump byte-stable and lets a test name
// `kTypeI32` instead of looking it up.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "sema/target.h"
#include "sema/type.h"
#include "support/limits.h"

namespace minc::sema {

class TypeStore {
public:
  explicit TypeStore(TargetInfo target = defaultTarget(),
                     std::size_t maxTypes = support::kMaxTypesPerUnit);
  TypeStore(const TypeStore&) = delete;
  TypeStore& operator=(const TypeStore&) = delete;

  [[nodiscard]] const TargetInfo& target() const {
    return target_;
  }

  // The budget, enforced *where the allocation happens*: interning is checked
  // against it before the `push_back`, so reaching it is a diagnostic a caller
  // can report rather than an allocation that already happened. It is settable
  // because it can be lowered (a constrained environment, a test proving the
  // bound is a bound) and never disabled.
  [[nodiscard]] std::size_t maxTypes() const {
    return maxTypes_;
  }
  void setMaxTypes(std::size_t value) {
    maxTypes_ = value;
  }

  // --- builders --------------------------------------------------------------
  //
  // Each interns, so the same shape always yields the same id. `bits` is a
  // width, never a spelling: the C spelling reader has already turned `long`
  // into the target's width by the time it calls this.

  [[nodiscard]] TypeId signedInt(std::uint16_t bits);
  [[nodiscard]] TypeId unsignedInt(std::uint16_t bits);
  [[nodiscard]] TypeId floatOf(std::uint16_t bits);
  // A pointer to `pointee`. Interned, so `*i32` is one id whatever spelled it,
  // which is what makes two `*i32` parameters the same type at a call site.
  //
  // `pointee` may be `void`, and that is not a defect: `*void` is the untyped
  // pointer of the model (`memory.md`), the only pointer type that converts to
  // and from another pointer type implicitly, and never one that may be
  // dereferenced or stepped.
  [[nodiscard]] TypeId pointerTo(TypeId pointee);
  // `[count]element`. Interned, so `[4]i32` is one id whose count is in its
  // structure: the identity is the *value* of the count and never its spelling
  // (`arrays.md` decision 19), which is why a `[0x10]i32` and a `[16]i32` are
  // the same call here.
  //
  // `kInvalidType` for a count of zero, for an element that is not an object
  // (`isObject`), and for a product that would not fit `size_t` -- the same
  // budget discipline as the rest of the store: the invalid answer is a
  // diagnostic the caller reports, not an allocation that already happened.
  // `arraySize` is the arithmetic on its own, so a caller that wants to say
  // *why* it is refusing can ask before it builds.
  [[nodiscard]] TypeId arrayOf(TypeId element, std::uint64_t count);
  // `[]element`. Interned like everything else, so `[]i32` is one id whatever
  // spelled it, and `[]i32` is *not* `[]u8`.
  //
  // One refusal, and it is the array's first one: the element must be an object
  // (`isObject`), because a slice of `void` or of a deferred literal is a view of
  // something with no representation. Nothing else can be refused -- there is no
  // count to check and no product that could overflow -- which is exactly why the
  // *view* needs no arithmetic and the array does.
  [[nodiscard]] TypeId sliceOf(TypeId element);
  // The parameters are copied into the store; the caller's span need not
  // outlive the call.
  // `variadic` is required rather than defaulted: every caller is a signature,
  // and a default would let one of them build the non-variadic type for a
  // variadic function without the compiler saying anything.
  [[nodiscard]] TypeId function(TypeId returnType, std::span<const TypeId> params, bool variadic);

  // --- access ---------------------------------------------------------------

  // Is this an id this store knows? Every predicate below answers "no" for one
  // that is not, because `kInvalidType` is how a caller says "there is no type
  // here" -- an absent annotation, an optional context -- and asking a question
  // about it must answer, not crash.
  [[nodiscard]] bool known(TypeId id) const {
    return id.valid() && id.index < types_.size();
  }

  // Precondition: `known(id)`.
  [[nodiscard]] const Type& get(TypeId id) const {
    return types_[id.index];
  }
  [[nodiscard]] std::span<const TypeId> paramsOf(TypeId id) const;
  // True for a function type whose parameter list ends in `...`. False for
  // everything that is not a function, so a caller never has to ask the kind
  // first -- the question "may this call pass more arguments" has one answer.
  [[nodiscard]] bool isVariadic(TypeId id) const;
  [[nodiscard]] std::size_t count() const {
    return types_.size();
  }
  [[nodiscard]] std::span<const Type> all() const {
    return types_;
  }

  // --- questions every checker asks ------------------------------------------

  // Int or Char (Char is an integer type; it is not *arithmetic*).
  [[nodiscard]] bool isInteger(TypeId id) const;
  [[nodiscard]] bool isFloat(TypeId id) const;
  // Integer promotion applies to these.
  [[nodiscard]] bool isSmallInteger(TypeId id) const;
  // A literal whose type its context has not decided.
  [[nodiscard]] bool isDeferred(TypeId id) const;
  // Int/Float/Char and the deferred literals: the types the arithmetic operators
  // accept. Deliberately excludes `bool` and `str`, which C would promote and
  // this language does not (README, *Conversions and literal typing*).
  [[nodiscard]] bool isArithmetic(TypeId id) const;
  // Arithmetic, `bool`, `str` or a pointer: the types that can be stored and
  // passed.
  [[nodiscard]] bool isScalar(TypeId id) const;
  [[nodiscard]] bool isVoid(TypeId id) const;
  // The bottom type, `!`: the type of an expression that never produces a value.
  // Every consumer that treats "produces nothing" as an error in a value position
  // has to ask this separately, because `!` in a value position is not a mistake
  // but a program that cannot reach it.
  [[nodiscard]] bool isNever(TypeId id) const;
  [[nodiscard]] bool isError(TypeId id) const;
  // A pointer to anything, `*void` included.
  [[nodiscard]] bool isPointer(TypeId id) const;
  [[nodiscard]] bool isArray(TypeId id) const;
  // `[]T`: a view, so it is an aggregate *and* it is not an object that owns its
  // elements. The distinction from `isArray` is the one every consumer of a view
  // asks about: an array is the storage, a slice names storage.
  [[nodiscard]] bool isSlice(TypeId id) const;
  // An aggregate: `[N]T` and `[]T` today, a `struct` when that lands. The types a
  // load, a store or a copy moves as one *object* rather than as one value, which
  // is the distinction the lowering needs and the reason this is not `isScalar`.
  [[nodiscard]] bool isAggregate(TypeId id) const;
  // A type with an object representation: an integer, a float, a `bool`, a
  // `char`, a `str`, a pointer, or an aggregate. What can be a binding, a
  // parameter, an element, a field, or the source of a copy -- the one question
  // those four have in common.
  //
  // Deliberately narrower than `isScalar`, which is why it is a second
  // predicate and not a rename (`arrays.md` decision 21): a *deferred* literal
  // is scalar-shaped and has no width, so `[3]<integer literal>` is not an
  // object and there is no array of one.
  [[nodiscard]] bool isObject(TypeId id) const;
  // The element of an array or of a slice, or `kInvalidType` for anything else.
  // One answer for both because the question is the same -- what is an element of
  // this -- and the two kinds differ in whether the length is known, not in what
  // they are made of.
  [[nodiscard]] TypeId elementOf(TypeId id) const;
  // The count of an array, or 0 for anything else -- a slice included, because a
  // slice has no count to give and the descriptor's length is a *value*, not a
  // type. 0 is not a count the store ever holds, so a caller can test it without
  // asking the kind first.
  [[nodiscard]] std::uint64_t countOf(TypeId id) const;
  // `*void`: the type that converts to and from any other pointer type, and the
  // one that may not be dereferenced or stepped (`memory.md`, *Access*).
  [[nodiscard]] bool isVoidPointer(TypeId id) const;
  // What a pointer points at, or `kInvalidType` for anything else. The answer is
  // also what an access through the pointer will be typed as -- its size and its
  // alignment -- so a caller never reaches into the `Type` for it.
  [[nodiscard]] TypeId pointeeOf(TypeId id) const;

  // --- rendering and layout --------------------------------------------------

  // The canonical spelling. A built-in prints its `.mx` primitive name (`i32`,
  // `u8`, `f64`), whatever spelling the source used to reach it -- a type's name
  // is a property of the type, not of the declaration that named it.
  [[nodiscard]] std::string spelling(TypeId id) const;
  // `count * sizeOf(element)`, computed with the one checked multiply. `nullopt`
  // when the count is zero, the element is not an object, or the product does
  // not fit `std::size_t` -- so a type whose size is not a number is refused
  // where the count is read (`arrays.md` decision 20), and no later stage has to
  // defend against one.
  [[nodiscard]] std::optional<std::size_t> arraySize(TypeId element, std::uint64_t count) const;
  // Bytes. 0 for `void`, a function, or the poison: they have no object
  // representation, and returning a plausible 1 would let a future `sizeof`
  // silently believe it.
  [[nodiscard]] std::size_t sizeOf(TypeId id) const;
  [[nodiscard]] std::size_t alignOf(TypeId id) const;
  // What a deferred literal becomes when nothing decided it: `i32`, `f64`. Not
  // `const`: answering it interns the default, because the answer is a type like
  // any other and must be the *same* `TypeId` everywhere it is asked.
  [[nodiscard]] TypeId defaultOf(TypeId id);

private:
  [[nodiscard]] TypeId intern(const Type& type);
  // A member rather than a free function: a function type's hash includes its
  // parameters, which live in this store's arena.
  [[nodiscard]] std::uint64_t hashOf(const Type& type) const;
  [[nodiscard]] bool equal(const Type& a, const Type& b) const;

  TargetInfo target_;
  std::size_t maxTypes_ = support::kMaxTypesPerUnit;
  std::vector<Type> types_;
  std::vector<TypeId> params_;
  std::unordered_multimap<std::uint64_t, TypeId> index_;
};

} // namespace minc::sema
