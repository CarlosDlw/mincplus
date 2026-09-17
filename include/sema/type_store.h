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
#include <deque>
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
  // `(T, U, ...)`: a **product** of two or more types, in the order written
  // (`tuples.md`).
  //
  // Three refusals, all asked *before* the intern, and each is the rule the
  // caller's sentence is about:
  //
  //  - **arity < 2** (`kInvalidType`): a product of one member *is* its member,
  //    so admitting it would hand two ids to one type -- which is the rule this
  //    whole store is built on. A product of zero is not a type at all: `()` is
  //    an empty parameter list, and "no value" is `void`.
  //  - **a member that is not an object** (`isObject`): a member is stored, so a
  //    member has to have a size. Same rule as an array element, for the same
  //    reason.
  //  - **a product whose size does not fit `size_t`**: the bound the array has,
  //    checked at the same place, so no later stage meets a type whose size is
  //    not a number.
  //
  // Interned: `(i32, bool)` is one id whatever spelled it, and the *order* is
  // part of the identity -- `(i32, bool)` and `(bool, i32)` are two types, which
  // is why the members are a sequence and not a set.
  [[nodiscard]] TypeId tupleOf(std::span<const TypeId> members);
  // The size of a product *before* it is built, so a caller can say *why* it is
  // refusing -- the peer of `arraySize`, for the same reason: a sentence needs to
  // name the number it is about, and the store is the one place that knows the
  // layout rule. `nullopt` when a member has no object representation or when the
  // padded total does not fit `std::size_t`.
  [[nodiscard]] std::optional<std::size_t> tupleSize(std::span<const TypeId> members) const;
  // How many types a structure built from `parts` would be made of, `children`
  // (a pointee, a return type) included -- the peer of `tupleSize` for the other
  // reason a build is refused. Asking it **before** the build is how a caller
  // says which of the two happened, and it is also what keeps the refusal cheap:
  // the count comes from what the store already recorded per type, so answering
  // costs one step per *member* and never a walk of the structure itself.
  //
  // Saturates one past `kMaxTypeNodes`, because the only question asked of it is
  // whether it is over the bound.
  [[nodiscard]] std::uint32_t nodesOf(std::span<const TypeId> parts,
                                      std::span<const TypeId> children = {}) const;
  // The parameters are copied into the store; the caller's span need not
  // outlive the call.
  // `variadic` is required rather than defaulted: every caller is a signature,
  // and a default would let one of them build the non-variadic type for a
  // variadic function without the compiler saying anything.
  [[nodiscard]] TypeId function(TypeId returnType, std::span<const TypeId> params, bool variadic);
  // A **type parameter**: `T` of `fn T identity<T>(value: T)` (`generics.md`).
  //
  // Its identity is `(owner, binder)` and not its structure, which is the whole
  // reason it is a kind: two declarations may each call their binder `T`, and they
  // are two types. `owner` is the declaring node's id in the unit's tree, `binder`
  // is the position in that declaration's binder list, and `spelling` is what a
  // diagnostic prints -- the *third* fact, kept beside the identity and not part
  // of it. The store copies the spelling, so the caller's view need not outlive
  // the call.
  //
  // `klass` is the *fourth* fact and travels the same way: the constraint the
  // binder wrote, which decides what its body may do with it and which type
  // arguments may fill it. It is not part of the identity for the same reason the
  // spelling is not -- both are properties of the bound name, and the identity is
  // which binder it is.
  [[nodiscard]] TypeId param(std::uint32_t owner, std::uint32_t binder, std::string_view spelling,
                             support::ConstraintClass klass = support::ConstraintClass::Any);
  // The constraint a binder was declared with, or `Any` for a type that is not a
  // binder at all. One question, asked where an operation is about to be allowed
  // or a type argument accepted.
  [[nodiscard]] support::ConstraintClass binderClass(TypeId id) const;

  // Does this type belong to the class?
  //
  // Each predicate is the **operation rule's own** -- `isArithmetic` for `+`,
  // `isScalar` for `==`, `isInteger` for `%` -- and that is the property that keeps
  // the two halves of a constraint from drifting: the body check asks "does the
  // class grant this operation", the satisfaction check asks "is the argument in the
  // class", and both are answering about the same rules.
  //
  // Here and not in the checker, because the predicates are this class's and both
  // stages that ask are asking about a *type*: the instantiation of a `fn`, and the
  // use of a generic `type` name.
  //
  // A binder is accepted rather than judged, and it is the one case that *cannot* be
  // judged here: a call inside a generic body is written in terms of the enclosing
  // binders (`fn T outer<T>(x: T) { return id::<T>(x); }`), and the honest answer for
  // an abstract argument is "ask again when it is concrete". It is asked again --
  // the worklist substitutes the enclosing instance before it interns anything, so
  // every argument reaching this from the expansion path is a real type.
  [[nodiscard]] bool satisfies(support::ConstraintClass klass, TypeId type) const;
  // Is this the parameter of *that* declaration? One question, because a binder
  // is only ever substituted by the declaration that owns it: a body referring to
  // an enclosing binder keeps it (decision 6), so "is this mine" is what every
  // substitution asks.
  [[nodiscard]] bool isParamOf(TypeId id, std::uint32_t owner) const;
  [[nodiscard]] bool isParam(TypeId id) const;
  // **Substitution.** Every `Param` of `owner` in `subject` is replaced by the
  // corresponding entry of `args`, and the result is interned like any other type
  // -- so `Pair<i32, bool>` *is* `(i32, bool)`, the check is an id equality, and
  // the store did not grow a second kind of type (decision 8).
  //
  // Total for the declaration it belongs to and partial for nothing: a type that
  // mentions no `Param` of `owner` comes back unchanged, which is what makes it
  // safe to call on a type read anywhere. `args` must be at least as long as the
  // binder list the params came from; a shorter span leaves the tail unsubstituted
  // rather than reading past it.
  //
  // `kInvalidType` when an argument has no object and one is required (the array
  // and tuple rules, applied to the *result*) or when the budget is reached -- the
  // invalid answer is a diagnostic the caller reports, never an allocation that
  // already happened.
  [[nodiscard]] TypeId substitute(TypeId subject, std::span<const TypeId> args,
                                  std::uint32_t owner);

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
  // A function's parameters, or `{}` for everything else -- including a tuple,
  // whose members are asked for by `membersOf`. The two are separate accessors on
  // purpose: "what may I call this with" and "what is this made of" are different
  // questions, and an accessor that answered both would let one be passed where
  // the other is meant.
  [[nodiscard]] std::span<const TypeId> paramsOf(TypeId id) const;
  // A tuple's members, in the order written, or `{}` for everything else.
  [[nodiscard]] std::span<const TypeId> membersOf(TypeId id) const;
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
  // `(T, U, ...)`: a product. Not a view and not storage of one element type:
  // its members may have different types, so there is no *element* type for an
  // index to produce (`membersOf` is the way in).
  [[nodiscard]] bool isTuple(TypeId id) const;
  // An aggregate: `[N]T` and `[]T` today, a `struct` when that lands. The types a
  // load, a store or a copy moves as one *object* rather than as one value, which
  // is the distinction the lowering needs and the reason this is not `isScalar`.
  [[nodiscard]] bool isAggregate(TypeId id) const;
  // Does this type have no size *yet*? True for a type parameter and for an
  // aggregate built out of one -- a product with a binder member, or an array of
  // one -- and false for everything else, which is every type that can reach a
  // module (`generics.md`: a `Param` is the one type the lowering never sees).
  //
  // It is a separate question from "is it an object": `(T, K)` *is* an object --
  // it can be returned, bound and passed -- and its width is the argument's, so
  // the two rules that *do* arithmetic on widths (an array's element product, a
  // product's layout) ask this first and skip the arithmetic instead of believing
  // a zero.
  [[nodiscard]] bool hasUnknownSize(TypeId id) const;
  // A type with an object representation: an integer, a float, a `bool`, a
  // `char`, a `str`, a pointer, an aggregate, or a type parameter -- a `Param`
  // answers yes, because a type argument *is* an object and the body of a generic
  // is checked once against the binder (`generics.md`). What a `Param` does not
  // have is a width, so nothing may take its size: see `arrayOf`, which is the one
  // place a wrong zero could be believed. What can be a binding, a
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
  // The byte offset of a tuple's member `index`, by the one layout rule the type
  // has: members in the order written, each at its own alignment, the whole
  // object padded to the largest member's (`tuples.md`, decision 4). 0 for
  // anything that is not a tuple, or for an index past the end -- the callers
  // that care (`sizeOf`, the debug record) walk the members they were given.
  //
  // It lives here and not in `ir` because it is a *property of the type*, and the
  // lowering must not be the second place that knows how a product is laid out.
  [[nodiscard]] std::size_t memberOffset(TypeId id, std::uint32_t index) const;
  // What a deferred literal becomes when nothing decided it: `i32`, `f64`. Not
  // `const`: answering it interns the default, because the answer is a type like
  // any other and must be the *same* `TypeId` everywhere it is asked.
  [[nodiscard]] TypeId defaultOf(TypeId id);

private:
  // The type's members, read out of the sequence table for a type that is
  // *already* interned. The span the table yields and the span a caller hands to
  // `tupleOf`/`function` are the same thing by construction; this is the one that
  // reads it where it lives.
  [[nodiscard]] std::span<const TypeId> partsOf(const Type& type) const;
  // One child's count: what `nodesOf` adds for a single `TypeId`. An id this
  // store does not know counts one, which is what makes a structure built on an
  // unknown type answer *small* rather than refuse -- an unknown type is the
  // poison `kInvalidType`, and the caller that built on one has already said so.
  [[nodiscard]] std::uint32_t childNodes(TypeId id) const;
  // A layout before it is stored: what `internSequence` copies into the `Type`.
  // Its own type rather than three parameters, because the three travel together
  // and a caller that mixed up two of them would be storing a wrong size.
  struct Layout {
    std::size_t size = 0;
    std::uint16_t align = 1;
    bool unknownSize = false;
  };
  // The layout of the type being built, from the layouts of its children -- each
  // already stored, so this is O(children) and never a walk (`type_store.cc`).
  [[nodiscard]] Layout layoutOf(const Type& type, std::span<const TypeId> parts) const;
  // The one hash, over the fields *and* the member sequence. It reads the
  // sequence from a span rather than from the arena because the type being hashed
  // may not be in the arena yet -- that is the whole case `function` and
  // `tupleOf` are, and answering it with a second hash function is how two ids
  // end up meaning one type.
  [[nodiscard]] std::uint64_t hashOf(const Type& type, std::span<const TypeId> parts) const;
  // The fields two types must agree on -- the same list the hash mixes, minus the
  // sequence. Split from the walk below for the reason above.
  [[nodiscard]] bool equalFields(const Type& a, const Type& b) const;
  [[nodiscard]] bool equalParts(const Type& type, std::span<const TypeId> parts) const;
  // A simple kind: no member sequence, so the sequence is whatever the type's own
  // fields say (empty for every kind that has none).
  [[nodiscard]] TypeId intern(const Type& type);
  // A kind built from a member sequence that is not in the arena yet: hash it,
  // confirm the candidates, and only then append the sequence. The order is the
  // rule -- a repeated signature must not grow the arena.
  [[nodiscard]] TypeId internSequence(const Type& type, std::span<const TypeId> parts);

  TargetInfo target_;
  std::size_t maxTypes_ = support::kMaxTypesPerUnit;
  std::vector<Type> types_;
  // The member sequences: one row per function or tuple type, written once when
  // the type is appended and never touched again.
  //
  // A `deque` of rows rather than one flat array, and the reason is stability, not
  // layout. `paramsOf`/`membersOf` hand out a `span` over a row, and a caller
  // holds one across an interning: checking a call's arguments interns the
  // instance's signature, and a checker of an array initializer holds the members
  // of the array while it interns the types of the elements. Appending to a
  // `deque` does not move what is already in it, so the span keeps pointing at the
  // row it was about; appending to a flat array moves every row at once, and every
  // such span becomes a read of freed memory with no diagnostic anywhere near it.
  //
  // The rows are never erased, which is what makes a row index (`Type::sequence`)
  // an index that stays valid for the life of the store.
  std::deque<std::vector<TypeId>> sequences_;
  std::unordered_multimap<std::uint64_t, TypeId> index_;
  // The spellings of the type parameters, owned by the store because a `Type`
  // holds a *view* of them: a `deque` and not a `vector`, because a vector's move
  // would leave every view dangling for a name short enough to live inside the
  // string (`T`). The pool never shrinks and nothing removes an entry: a spelling
  // is part of what a type's name is, and a type outlives the declaration that
  // introduced it.
  std::deque<std::string> paramSpellings_;
};

} // namespace minc::sema
