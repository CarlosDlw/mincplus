// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The **constraint vocabulary**: the classes a binder may be constrained to, what
// each one admits, and what each one grants (`generics.md`, § 6).
//
// A constraint says which operations a generic body may perform on the hole it
// declared. `<T: Number>` is the declaration saying "for this body, `T` is a
// number" -- and a number in this language means the arithmetic operators, because
// that is what the operation rules say a number is.
//
// The two facts per class, and they are deliberately **two**:
//
//   * its **members**, a predicate over types -- the predicate the operation rules
//     already use (`isArithmetic`, `isInteger`, `isFloat`, `isScalar`,
//     `isPointer`). It lives in `sema` with the type store, because that is what it
//     is a predicate over.
//   * its **grants**, the operations a body may perform on a binder of this class.
//     That is what this file holds.
//
// ## The invariant, which is a test and not an argument
//
// > **Every operation a class grants is legal for every type in it.**
//
// This is the property that keeps the grants from being decoration. A class is a
// promise about a set of types, and a promise that the operation rules would refuse
// for one of those types is a lie the compiler tells the reader -- the exact failure
// C++ has, where the constraint is inferred from whatever the body did and surfaces
// at a call site two layers away. So the grants are **checked against the
// predicates**, per class, by a test over representative types: `Float` cannot grant
// `%` (no float admits it), `Ordered` must grant `<` for every arithmetic type, and
// `Number` may grant `+` for all of them.
//
// It is *not* derived at run time, and the reason is a decision rather than an
// optimisation: grants and members are **independent facts**, so two classes may
// admit the same types and promise different operations. `Ordered` and `Number` are
// exactly that pair -- same members, and `Ordered` grants comparison only, because
// `<T: Ordered>` is the declaration saying "all I do with it is compare". A
// derivation would collapse the two into one class and take the reader's ability to
// say which capability they meant with it.
//
// ## No class for `bool`
//
// `!`, `&&`, `||` and the condition of `if`/`while`/`for` are `bool`-only, and no
// class grants them **on purpose**: a class whose members are one type is not a
// constraint, it is the type. A binder in one of those positions is refused with a
// sentence that says to write `bool`, which is the honest fix -- `<T: Bool>` would
// make `bool` a variable.
//
// ## What `Pointer` grants, and the two operations it cannot
//
// `Pointer` grants the **comparisons** -- `p < q` is an address comparison this
// language defines, and it is the only thing expressible on a pointer whose pointee
// nobody named. `*p` and `p[i]` are the pointee's type, and `p + i` scales by the
// pointee's size: all three need a type an abstract pointer does not name, so no
// class grants them, and `refuseOperation` says so instead of talking about numbers.
// They arrive with a way to write "the pointee of `T`", which is the associated-type
// half of a user-declared constraint and not this stage.
//
// ## Composition, and why it is not here yet
//
// A total order needs no combination: every pair of these classes is either
// redundant or contradictory (`Number` already grants `Ordered`'s operations;
// `Float` and `Pointer` share no type). So `<T: A + B>` has no use today, and the
// syntax that will need it is the **second independent class**, which is the one a
// user-declared `interface` brings. The slot is `parseGenericParams`, which already
// reads a class name and can read a `+` beside it.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace minc::support {

// An operation a constraint can grant -- and the three `bool`-only ones and the
// `bool` positions, which no class grants, so that "nothing grants this" is an
// answer the table gives rather than a case a caller has to remember it is missing.
enum class Operation : std::uint8_t {
  Add,
  Sub,
  Mul,
  Div,
  Remainder, // `%`
  Negate,    // unary `-` and `+`
  Increment, // `++` and `--`
  BitAnd,
  BitOr,
  BitXor,
  BitNot, // `~`
  ShiftLeft,
  ShiftRight,
  Less,
  LessEqual,
  Greater,
  GreaterEqual,
  Equal,
  NotEqual,
  // Not operators: the positions that require a `bool` -- `!`, `&&`, `||`, and the
  // condition of `if`/`while`/`for`. They are here so that "no class grants it" is
  // one answer and one sentence, which is what the table is for.
  LogicalNot,
  LogicalAnd,
  LogicalOr,
  Condition,
};

// A class a binder may be constrained to. `Any` is the default and is a class rather
// than the absence of one: a binder with no written constraint has a class, and it
// is this one.
enum class ConstraintClass : std::uint8_t {
  Any,
  Eq,
  Ordered,
  Number,
  Integer,
  Float,
  Pointer,
};

// The kind of literal a class admits, for the one rule that needs to know: a
// **deferred** literal in a binder's position. `let x: T = 1;` is not decidable from
// `T` alone -- `1` is an integer literal -- so it is decided from the class, and
// only a class whose members are *all* integers can take it.
enum class LiteralClass : std::uint8_t {
  None,    // no class admits a deferred literal: the value would mean two things
  Integer, // every member is an integer, so `1` is `1` for all of them
  Float,   // every member is a float, so `1.0` is `1.0` for all of them
};

// Every class name, in declaration order. The names are capitalized words that
// match no type-name word (`support::isTypeNameWord`) and no keyword, so a class and
// a type can never be the same spelling and `T: i32` is a class that does not exist
// rather than a type.
[[nodiscard]] std::span<const std::string_view> constraintClassNames();

// The class a written name denotes, or `nullopt` when there is no such class.
// Case-sensitive, like every other word of the language: `number` is not `Number`.
[[nodiscard]] std::optional<ConstraintClass> constraintClassFromName(std::string_view name);

// What a class prints as (`Number`), which is the spelling a diagnostic names.
[[nodiscard]] std::string_view constraintClassName(ConstraintClass klass);

// True when the class grants the operation, by the table and the invariant above.
[[nodiscard]] bool constraintGrants(ConstraintClass klass, Operation op);

// The class a refusal should tell the reader to write for this operation, or `Any`
// when **no** class grants it.
//
// It is the least powerful class that grants the operation, because that is the
// smallest promise that makes the body legal: `Number` already grants `Ordered`'s
// comparisons, so a body that only compares is told `Ordered` -- naming `Number`
// would make the declaration promise arithmetic it never uses.
//
// `Any` is the caller's cue to say something else, and the family it answers for is
// `!`, `&&`, `||` and the `bool` positions (see the header).
[[nodiscard]] ConstraintClass constraintForOperation(Operation op);

// The kind of deferred literal a class admits. `Integer` admits an integer literal
// and `Float` a float one; every other class admits neither, because its members are
// not all of one class of number -- and a body where `1` means `1i32` for one
// instantiation and `1.0` for another is a body that means two things.
[[nodiscard]] LiteralClass constraintLiteralClass(ConstraintClass klass);

// Whether a literal of the class it was written in may be the value of a binder of
// this class -- the *decision* half of the literal rule, and here rather than in
// `sema` because it is one comparison between two cells of the table above.
//
// Three sites ask it (`decideAt` when a literal is decided, `checkAssignable` when a
// value is stored in a binder, and the binary case where a literal sits beside one),
// and a rule asked three times is a rule that drifts: this is that rule written once.
// The sentence that reports the refusal is `sema`'s, because a diagnostic is.
[[nodiscard]] bool literalAdmittedBy(LiteralClass admitted, bool isFloatLiteral);

} // namespace minc::support
