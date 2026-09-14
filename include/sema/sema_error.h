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
  // Type specifiers that cannot combine: `unsigned float`, `i32 int`,
  // `long long long`.
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
  // The type budget was reached. A hazard bound, not a language rule.
  LimitTypes,
  // A statement after a `return` in the same block (warning).
  UnreachableCode,
  // An implicit narrowing conversion (`-Wconversion`, warning).
  ImplicitConversion,
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
