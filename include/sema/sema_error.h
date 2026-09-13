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
