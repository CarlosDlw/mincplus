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
  explicit TypeStore(TargetInfo target = targetInfo(kDefaultTarget),
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
  // The parameters are copied into the store; the caller's span need not
  // outlive the call.
  [[nodiscard]] TypeId function(TypeId returnType, std::span<const TypeId> params);

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
  // Arithmetic, `bool` or `str`: the types that can be stored and passed.
  [[nodiscard]] bool isScalar(TypeId id) const;
  [[nodiscard]] bool isVoid(TypeId id) const;
  [[nodiscard]] bool isError(TypeId id) const;

  // --- rendering and layout --------------------------------------------------

  // The canonical spelling. A built-in prints its `.mx` primitive name (`i32`,
  // `u8`, `f64`), whatever spelling the source used to reach it -- a type's name
  // is a property of the type, not of the declaration that named it.
  [[nodiscard]] std::string spelling(TypeId id) const;
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
