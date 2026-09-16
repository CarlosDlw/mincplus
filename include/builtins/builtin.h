// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The builtin table: one row per operation the *program* may name, and nothing
// else in the compiler knows a builtin's name.
//
// ### Why a module of its own
//
// Five readers, and no two of them want the same thing from a row: `resolve`
// binds the name, `sema` types the call, `ir` lowers it, `mincc builtins`
// renders it, and the reference page is generated from it. The table therefore
// depends on **`support` alone**:
//
//   - not on `sema`, so a row cannot carry a `TypeId` -- ids are store-local and
//     target-dependent, which is also why `any-int` is a *shape* in a row and a
//     concrete type only once the checker has an argument to look at;
//   - not on `ir`, so a row cannot carry an `llvm::Intrinsic::ID` (`ir.md` makes
//     `ir` the first stage that may include `llvm/*`, and a docs generator must
//     not have to link LLVM to print a name) -- the row carries the intrinsic's
//     *name* and `ir` is where the name becomes an id;
//   - not on `resolve`, because the data has four other readers and none of them
//     is the resolver.
//
// ### What a row is, and what it may not be
//
// A row is a promise with five answers: what it is called and who owns the name,
// what its arguments and result are, what it does to the world, what it becomes
// in the module, and whether the language promises it. Every field below varies
// across the rows in the table -- a field with one possible value is a second
// statement of a fact that already lives in the lowering, which is how a
// front end's table starts disagreeing with itself.
//
// Three things a row deliberately is **not**:
//
//   - **not code.** The lowering is data (`Lowering`), never a function pointer:
//     a row that is a closure cannot be printed, diffed, rendered as a page, or
//     compared against LLVM's own attributes by a test. Go's `intrinsicBuilder`
//     is the shape this avoids.
//   - **not a compiler-emitted operation.** The `memcpy` of an object copy and
//     the `llvm.trap` of a checked division are things the compiler needs and the
//     program cannot spell; they belong to the construct that lowers to them and
//     get no row, because a row would make them writable and therefore a promise
//     about their semantics.
//   - **not a macro, and not a library symbol.** A symbol in the runtime is a
//     declaration and a header (`extern fn`, already shipped); a macro is the
//     preprocessor's business (`assert`). Both are cheaper to change than a
//     compiler release, and the test for a row is: could a library do it with the
//     language as it stands? If yes, it is a function.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "builtins/builtin_id.h"

namespace minc::builtins {

// Who owns the spelling. This is the field the two-spelling design turns on, and
// the two answers are not two levels of the same thing -- they are two ways of
// getting a name:
//
//   - `Reserved` -- `__builtin_*`. The program may not declare it, may not
//     `#define` it, and may not shadow it: the guarantee is enforced where the
//     name is *taken*, in `sema` (a declaration) and in the preprocessor (a
//     macro), which is stronger than C -- where taking the prefix is undefined
//     behavior no compiler diagnoses -- and cheaper than Zig's `@`, which pays a
//     token class and a grammar production for the same guarantee.
//
//   - `Prelude` -- a plain name, bound in the file scope before the unit is read,
//     exactly as `true` and `null` are. Ordinary lookup finds it; an ordinary
//     declaration shadows it and `-Wshadow` says so. This is why the language can
//     have a `clz` that is *not* it: nothing here matches a user function by
//     name and replaces the call (GCC's `-fbuiltin`, and the reason `-fno-builtin`
//     exists), because the name being called is a declaration of ours, resolved
//     like any other, and a program that writes its own `clz` calls its own.
enum class SpellingClass : std::uint8_t { Reserved, Prelude };

[[nodiscard]] std::string_view toString(SpellingClass value);

// Whether the language promises the name. `Stable` is earned, not granted: a row
// is stable when every input has a defined answer, which for a `Prelude` row is
// the same statement as "there is nothing to warn about". `Internal` is the raw
// layer -- Rust's rule, in its own words: intrinsics are unlikely to ever be
// stabilized, and a program is meant to reach them through the wrappers the
// runtime builds on top.
enum class Status : std::uint8_t { Stable, Internal };

[[nodiscard]] std::string_view toString(Status value);

// What one argument, or the result, is.
//
// A closed C++ type and not a string of letters (Clang's `"z"`, `"LLi"`): a typo
// in a type *string* parses and is only caught by a test, and the same file's
// comment admits the alphabet "must be kept in sync" with the predicates that
// read it. A `switch` over this enum with no `default:` is a compile error for a
// missing case, and the checker resolves each one against the type store it
// already owns -- which is what makes `usize` mean 32 bits on one target and 64
// on another *by the same row*.
enum class BuiltinType : std::uint8_t {
  Void,
  Bool,
  Char,
  Str,
  I8,
  I16,
  I32,
  I64,
  I128,
  Isize,
  U8,
  U16,
  U32,
  U64,
  U128,
  Usize,
  F32,
  F64,
  // The untyped pointer, `*void`.
  VoidPtr,
  // The bottom type, `!`: the result of an operation that never comes back, and
  // the reason `fn i32 fail() -> i32 { __builtin_trap(); }` is not a missing
  // return. `sema` already asks the *type* for "does control come back", so a row
  // answers the flow pass by being typed and not by being special-cased.
  Never,

  // The argument's own integer type, signed and unsigned alike: the operation is
  // on the bit pattern, so `clz` of an `i32` is the same operation as `clz` of a
  // `u32` with no conversion between them -- which is the only honest answer in a
  // language that converts nothing implicitly.
  AnyInteger,
  // The type this row established for another argument -- LLVM's `LLVMMatchType`.
  // It is what makes a rotate's result the width of its value, and what makes an
  // out-parameter's pointee the type of the value being stored.
  MatchArg,
  MatchArgPtr, // `*MatchArg`
};

[[nodiscard]] std::string_view toString(BuiltinType value);

// What the operation does to the rest of the program.
//
// The checker may not ask LLVM (it cannot include it), so the front end's
// decisions need their own statement: `Diverges` is what makes a call an
// expression of type `!`'s family in the flow pass, and it is the field the
// differential test cross-checks against LLVM's `IntrNoReturn`.
enum class Effect : std::uint8_t {
  // Reads and writes nothing, and returns.
  None,
  // Does not return; every edge after a call to it is unreachable.
  Diverges,
};

[[nodiscard]] std::string_view toString(Effect value);

// The constant trailing operand an intrinsic requires after the arguments the
// program wrote.
//
// This is LLVM's contract showing through, and the place where the language's
// promise -- *if the checker lets it pass, it must run* -- is kept for the bit
// operations: `llvm.ctlz(x, is_zero_poison)` is poison for `x == 0` when the flag
// is true and **the bit width** when it is false, and the language's answer for
// `clz(0)` is the width. So the row passes `false`, and the difference between
// our answer and UB is one operand of data rather than a paragraph of intent.
enum class TailOperand : std::uint8_t { None, I1False, I1True };

[[nodiscard]] std::string_view toString(TailOperand value);

// What a row becomes, as data.
//
// `Intrinsic` is the ordinary case: a name to look up in `Intrinsics.td`, plus
// whatever constant trailing operand that intrinsic's contract requires.
//
// The rest are operations whose contract the language has to satisfy *before*
// the intrinsic: `llvm.fshl`/`llvm.fshr` are poison when the count is at or past
// the width, while this language defines a rotate as "`n` modulo the width", so
// the count is reduced first and the intrinsic sees an argument it is defined
// for. That reduction is code, and it is expressed as a *kind* rather than as a
// flag beside one, because a second way of saying "and then emit this" is a
// second thing that can disagree with the first.
struct Lowering {
  enum class Kind : std::uint8_t {
    Intrinsic,
    // `urem` the count by the width, then `llvm.fshl(x, x, count)`.
    RotateLeft,
    // ... and `llvm.fshr` for the other direction.
    RotateRight,
  };

  // Which of the intrinsic's overloaded type parameters the call supplies.
  //
  // One row in `Intrinsics.td` is *every* integer width (`llvm_anyint_ty`), so a
  // declaration has to be asked for at a width, and which width is a fact about
  // the operation and not about one call. It is data here for the same reason the
  // name is: `ir` is where the abstract becomes concrete, and a second rule about
  // how many types an intrinsic takes is a second place for LLVM and this table
  // to disagree.
  enum class Overload : std::uint8_t {
    // The intrinsic has no overloaded type (`llvm.trap`).
    None,
    // The first argument's own type (`llvm.ctlz.i32` from an `i32` argument).
    FirstArgument,
  };

  Kind kind = Kind::Intrinsic;
  // The `llvm.*` name, for `Kind::Intrinsic`. For the other kinds, the name the
  // *stage* uses, so the table still renders as text.
  std::string_view name;
  TailOperand tail = TailOperand::None;
  Overload overload = Overload::FirstArgument;

  [[nodiscard]] constexpr bool isIntrinsic() const {
    return kind == Kind::Intrinsic;
  }
};

// The widths the row's matched integer argument may have.
//
// It exists because LLVM's *verifier* has rules the language cannot inherit, and
// the difference matters: `llvm.bswap.i8` is not undefined, it is **invalid** --
// "bswap must be an even number of bytes" -- so a `bswap` row that accepted a
// `u8` would be a program the checker let pass and a module LLVM refuses to
// verify, which is precisely the promise this project is built to keep. The
// refusal is a sentence at the call site instead.
enum class MatchedWidths : std::uint8_t {
  // Every integer width the language has.
  Any,
  // A whole number of bytes, and at least two: 16, 32, 64, 128.
  EvenBytes,
};

[[nodiscard]] std::string_view toString(MatchedWidths value);

// The argument shapes, and the result.
//
// The parameters are a span into a `constexpr` array that lives in the table, so
// a row costs one pointer and one count.
struct Signature {
  std::span<const BuiltinType> params;
  BuiltinType result = BuiltinType::Void;
  MatchedWidths matchedWidths = MatchedWidths::Any;
};

// One builtin.
struct BuiltinInfo {
  BuiltinId id = BuiltinId::None;
  std::string_view spelling;
  SpellingClass spellingClass = SpellingClass::Reserved;
  Signature signature;
  Effect effect = Effect::None;
  Lowering lowering;
  Status status = Status::Internal;
  // One sentence, and a row may not ship without one: it is what the reference
  // page and `mincc builtins` show, and a builtin nobody can read about is a
  // builtin nobody can use. A test asserts every row's line is non-empty.
  std::string_view doc;

  [[nodiscard]] constexpr bool isPrelude() const {
    return spellingClass == SpellingClass::Prelude;
  }
  [[nodiscard]] constexpr bool isReserved() const {
    return spellingClass == SpellingClass::Reserved;
  }
};

// --- the table, read ----------------------------------------------------------

// Every row, in table order. The enumeration *is* the table: there is no second
// registry to keep in step and no build-time accumulation.
[[nodiscard]] std::span<const BuiltinInfo> all();

// The row this row is, by the exact spelling (`__builtin_clz`, `clz`), or null.
// Exact and case-sensitive, because the spelling is a program's identifier and
// identifiers are exact.
[[nodiscard]] const BuiltinInfo* lookup(std::string_view spelling);

// The row this id is, or null for `None` and for an id no row has.
[[nodiscard]] const BuiltinInfo* lookup(BuiltinId id);

// The names the compiler keeps for itself, as a rule and not as a list: anything
// beginning with `__builtin_`. This is what `sema` refuses in a declaration and
// what the preprocessor refuses in a `#define`, and it is one function so the two
// cannot disagree about which names are taken.
//
// Deliberately *not* "any name beginning with `__`": C reserves those, and this
// language reserves what it actually keeps, which is a promise a user can read
// off the compiler rather than off a standard.
[[nodiscard]] bool isReservedPrefix(std::string_view spelling);

// The abstract signature, for a page or a terminal: `(any-int) -> same`. The
// concrete widths are the checker's answer for a given call, and printing them
// here would be a promise about a target the *row* does not make.
[[nodiscard]] std::string signatureText(const BuiltinInfo& row);

} // namespace minc::builtins
