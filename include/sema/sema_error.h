// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// A typing problem, as a value.
//
// Same shape as every other stage: this layer never reports. `sema_report.h`
// knows about severity and `DiagBag`, which is what keeps `minc_sema` free of
// the diagnostic machinery and lets the checker be tested with no `Session` and
// no terminal -- and what lets a later stage re-run it as often as it likes
// without a second diagnostic appearing anywhere.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "support/span/span.h"

namespace minc::sema {

// The closed set. A code is an enumerator with one table row rather than a
// string literal at each call site, so the stage cannot invent a code no test
// knows about and two sites cannot spell the same condition two ways.
//
// Every code here is reachable from an input the grammar accepts today; the
// suite pins one input per code. A rule whose syntax does not exist yet is not
// listed: a code that cannot fire is a code that cannot be tested.
enum class SemaErrorCode : std::uint8_t {
  // A word in a type position (or the empty run) that names no type.
  UnknownType,
  // Type specifiers that cannot combine (`unsigned float`, `i32 int`, `long long
  // long`) **and** a type the spelling names but the target does not have, which
  // is `f80` on a machine with no x87. One code for the two because the repair is
  // one shape -- write a type this language and this target have -- and because
  // `UnknownType`'s machinery is for a word nobody recognizes: telling a reader
  // who wrote `f80` that they may have meant `i8` is advice nobody can use.
  MalformedType,
  // A non-value type where a value is required (`void` as an object's type).
  TypeNotValue,
  // An integer literal does not fit the type its context gave it.
  LiteralOutOfRange,
  // A condition, or an operand of `!`/`&&`/`||`, that is not `bool`.
  ConditionNotBool,
  // An operator applied to types it does not accept.
  InvalidOperands,
  // The left side of an assignment is not a place a value can be stored.
  InvalidAssignment,
  // Assigning to, or incrementing, a `const` binding.
  AssignToConst,
  // `++`/`--` on something that is not an lvalue.
  IncDecNotLvalue,
  // An array literal whose element type the context was needed for, and was not
  // there: `[1, 2, 3]` on its own says how many elements it has and nothing about
  // what they are, and the count of an array is part of the *type* of an object
  // whose type this language writes down (`arrays.md` decision 8).
  LiteralTypeUnknown,
  // The shape of a list of elements does not match the type it is building: the
  // length is not exact, a `_` was written with nothing to count, a fill's two
  // numbers disagree, or the group is empty. One code and several sentences,
  // because the *repair* is one shape in every case -- an array's value is the
  // whole value, written out -- and the sentence is what says which number moved.
  InitializerShape,
  // A `return` that a function returning `!` can actually execute. The type
  // promises the call never gives control back, and a `return` is precisely
  // control coming back -- so this is the promise broken, reported at the
  // statement that breaks it. Unreachable `return`s (after a `while true`, say)
  // are not this.
  NeverReturns,
  // A function returning `!` whose body can complete normally. A `!` body has to
  // *end* in something that does not end -- a loop that cannot leave, or another
  // call that never comes back -- and this is the diagnostic for a body that
  // simply runs out.
  NeverBodyCompletes,
  // Calling a value whose type is not a function.
  NotAFunction,
  // Wrong number of arguments.
  ArgumentCount,
  // The returned expression does not convert to the function's return type.
  ReturnMismatch,
  // `return;` in a function that must return a value.
  ReturnMissingValue,
  // `return expr;` in a `void` function.
  ReturnVoidValue,
  // A non-`void` function can reach its end without returning a value.
  MissingReturn,
  // `main` is declared and is not `fn i32 main()`.
  MainSignature,
  // Two definitions of one function. The language gives a name one definition,
  // so the second body has nowhere to go -- which is why this is an error and
  // not "the last one wins".
  FunctionRedefinition,
  // Two declarations of one function whose signatures disagree: `extern fn i32
  // f(i32);` above `fn i32 f() { }`. The declaration is what every call is
  // checked against and what the symbol's type comes from, so the two have to
  // name the same function for the program to mean anything.
  SignatureMismatch,
  // An `extern` declaration with an array in its signature -- a parameter, or the
  // return type. An array crosses this language's functions by *value*, and that
  // shape is the compiler's own: no ABI promises it, so a definition compiled
  // somewhere else cannot be called with it. Refused at the declaration, because
  // the alternative is a symbol that links, runs, and reads the wrong bytes
  // (`arrays.md` decision 11).
  ExternAggregate,
  // `a[..]` where the base is neither an array, nor a slice, nor a pointer: an
  // integer has no elements to view, and there is no extent a bound could be
  // measured against (`slices.md`). Distinct from `DerefNotPointer` because the
  // *set* of bases is larger here -- a view can be taken of an object and of
  // another view -- so the fix the sentence gives is a different one.
  SliceNotViewable,
  // `p[..4]` on a pointer: a view of a pointer is a view of an object whose
  // length is **not in any type**, so both bounds have to be written, and this is
  // the code for the form that wrote only one (`slices.md` decision 8).
  SlicePointerNeedsBothBounds,
  // `a[3..1]`: a constant begin after a constant end. The result would be a view
  // of a negative number of elements, which is not an empty view -- it is a
  // length the descriptor cannot hold -- and both numbers were written where the
  // compiler can see them.
  SliceBoundsReversed,
  // `break` with no loop to break out of.
  BreakOutsideLoop,
  // `continue` with no loop to continue.
  ContinueOutsideLoop,
  // A constant division or remainder by zero.
  DivisionByZero,
  // A constant expression whose value does not fit the type it is computed in.
  // Distinct from `LiteralOutOfRange`, which is about one literal: here the
  // value came out of folding, and blaming a literal the program never wrote
  // would point the reader at the wrong token.
  ConstantOutOfRange,
  // A shift whose count is negative or at or past the width of the value moved.
  // C leaves it undefined and the backend inherits a poison value.
  ShiftCountOutOfRange,
  // A name read on a path that never assigned it: `let x: i32;` with no
  // assignment reaching the read. Never a warning -- an unwritten object has no
  // value to read -- and never a guess: the analysis names the paths it proved.
  UseBeforeAssignment,
  // `*x` where `x` is not a pointer.
  DerefNotPointer,
  // `*p` or `p[i]` where the pointee is `void`: `void` has no size, so there is
  // nothing there to access. Distinct from `DerefNotPointer` because the pointer
  // is fine and it is the *type* that has to be named.
  PointerVoidAccess,
  // `p + n`, `n + p` or `p - q` where the pointee is `void`: stepping a pointer
  // scales by the pointee's size, and `void` has none.
  PointerVoidArithmetic,
  // `&e` where `e` has no address (an arithmetic value, a literal, a call).
  AddressOfNonLvalue,
  // `&c` where `c` is a `const` binding. `memory.md` states the operator takes
  // the address of a **modifiable** lvalue: a pointer to a `const` binding would
  // be a way to write it, and `const` protects the name. Distinct from
  // `AddressOfNonLvalue` because the operand *is* a place -- what is missing is
  // permission, and the fix is different (drop the `const`, or copy the value).
  AddressOfConst,
  // `p[i]` with an index that is not an integer.
  IndexNotInteger,
  // A **constant** index outside an array's own count: `a[4]` on a `[4]i32`.
  // The count is in the type, so this is arithmetic on two numbers the compiler
  // already has, and it is the one bounds check C's type system cannot express --
  // there the array is a pointer by the time anyone could look. A runtime index
  // is not this code: it is the access's extent, which the checked build guards
  // (`arrays.md` decisions 7 and 26).
  IndexOutOfRange,
  // Two pointer types that do not meet: a comparison of `*i32` with `*u8`, a
  // subtraction of unrelated pointees, a `?:` with no common pointer type, or an
  // initializer/argument of one pointee type where the other is required. The
  // last group is why the message states the rule rather than the mismatch:
  // `*void` is the only crossing point that is implicit, and the rest is a
  // reinterpretation the source has to write (`memory.md`, *The surface*).
  PointerMismatch,
  // A pointer and an integer on one side of a conversion. The language has no
  // implicit conversion between them in either direction: the two named
  // operations that join them are not in the grammar yet, and until they are the
  // refusal is the whole rule (`memory.md`, *Provenance*).
  PointerInteger,
  // A file-scope initializer that is not an initializer *constant expression*:
  // a call, a read of a `let`, a dereference, a local. The language has no
  // dynamic initialization, so the value of a file-scope object has to be known
  // before the program exists (`globals.md`, decision 2), and this is the
  // sentence for the expression that stopped being one.
  // A builtin used where a *value* is wanted: stored, passed, taken the address
  // of. A row need not have an address at all -- one whose lowering is an
  // instruction has no symbol behind it -- so the name denotes the operation and
  // never a function value, and this is the refusal that keeps that honest.
  BuiltinNotAValue,
  // A builtin called on an integer width the operation does not exist for. It is
  // a diagnostic and not a verifier error one stage later: `llvm.bswap` is
  // *invalid* -- not undefined -- for an odd number of bytes, so a `bswap` of a
  // `u8` would be a program the checker let pass and a module LLVM refuses.
  BuiltinWidth,
  GlobalNotConstant,
  // The file-scope bindings of this unit need each other's values in a cycle
  // (`const a = b; const b = a;`). Each has a value only if the other does, and
  // there is no order that gives both one -- so the language refuses the cycle
  // rather than picking an arbitrary place to start, which would make the value
  // of a constant depend on the order the compiler happened to visit in.
  GlobalCycle,
  // The type budget was reached. A hazard bound, not a language rule.
  LimitTypes,
  // A statement after a `return` in the same block (warning).
  UnreachableCode,
  // An implicit narrowing conversion (`-Wconversion`, warning).
  ImplicitConversion,
  // A cast the language does not permit, and the sentence names what to write
  // instead. The catalogue of refusals is `casts.md`, *What is refused*: an
  // array that would have to decay, a view, a float into a `bool`, `void`,
  // a function type, and a suffix that names a type the literal cannot take.
  CastInvalid,
  // A cast that *may* lose something (`-Wcast`, warning). Distinct from
  // `ImplicitConversion` on purpose: the implicit lint fires on the mask idiom
  // (`(u8)x`) a cast is written for, so the two rules are two flags.
  CastLoses,
  // A cast between a pointer and an integer (`-Wprovenance`, warning): the two
  // named operations of `memory.md`, `expose` and `with_exposed_provenance`. It is
  // a warning about *where*, not about *how much* -- a 64-bit pointer into a
  // `usize` loses nothing and is still a site the model wants counted.
  ProvenanceCast,
  // An `int -> ptr` conversion **from a constant that is not zero**: the program
  // never obtained that address. `with_exposed_provenance` is a named operation,
  // and naming an address is an assertion about where the value came from -- which
  // a constant cannot make (`casts.md`, decision 11b).
  AddressFromConstant,
};

struct SemaErrorCodeInfo {
  SemaErrorCode code;
  const char* name;
  bool warning;
};

[[nodiscard]] std::span<const SemaErrorCodeInfo> semaErrorCodeInfos();

// Every code, derived from the table, so a code added to the enum without a row
// is caught by the tests rather than shipped dead.
[[nodiscard]] std::span<const SemaErrorCode> allSemaErrorCodes();

// The stable short name (`sema-unknown-type`), never localized.
[[nodiscard]] std::string_view toString(SemaErrorCode code);

[[nodiscard]] bool isWarning(SemaErrorCode code);

struct SemaError {
  // Where the bytes were written -- the header a type came from, the macro
  // argument a literal came from. What a diagnostic points at.
  support::Span span;
  std::string message;
  SemaErrorCode code = SemaErrorCode::UnknownType;
  // A secondary diagnostic worth printing, when there is one. Used for "the
  // type name is unknown, but this one is one edit away": one error with a
  // pointer, not two errors for one mistake.
  std::string note;
  support::Span noteSpan;
};

} // namespace minc::sema
