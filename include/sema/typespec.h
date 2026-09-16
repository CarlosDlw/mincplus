// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Reading a `Type` node: the identifier run a type position holds.
//
// A type is a *position* in this grammar, not a token kind (`parser.md`), so
// `i32`, `int`, and `unsigned long long int` all arrive as a run of identifiers
// and only this stage can say which of them is a type. The reader is a small
// state machine over the words, and the point of writing it down is that every
// rejection has a sentence: `unsigned float` is not "malformed type" but
// "`unsigned` cannot apply to `float`".
//
// Widths come from the **target** (`sema/target.h`), never from the compiler's
// own machine, which is what makes `long` mean the right thing in a cross build
// and what keeps this file free of any `#ifdef`.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sema/target.h"
#include "sema/type.h"
#include "sema/type_store.h"

namespace minc::sema {

struct TypeSpecResult {
  // `kInvalidType` when `ok` is false.
  TypeId type;
  bool ok = false;
  // A complete sentence, because it is what a diagnostic prints.
  std::string message;
  // The word that could not be understood, when the failure was an unknown one:
  // the caller suggests a near miss. Empty for every other failure, whose fix is
  // not a different spelling.
  std::string_view unknownWord;
};

// Reads the words of a type run, in source order. An empty run is an error
// rather than a guess: a missing type is the parser's finding, and a default
// here would hide it.
//
// This is the *base* reader: the words of a type, with no pointer prefix. It
// stays public because it is the whole of the C-specifier grammar and a caller
// that has only words (a test, a future `#if` type query) should not have to
// build a `TypePart` array to ask about one.
[[nodiscard]] TypeSpecResult readTypeSpec(std::span<const std::string_view> words,
                                          TypeStore& types);

// One element of a type position, in source order: a `*`, an `[N]`, or a word.
//
// A type position is `(* | [N])* <words>` and nothing else, because that is what
// the grammar accepts (`parser.md`): both constructors are *prefixes* over what
// they build, so a pointer and an array are two more parts of the same run every
// other type is -- and `*[4]i32` and `[4]*i32` are the same two parts in two
// orders, with no second grammar to remember (`arrays.md`, *The surface*).
struct TypePart {
  bool isStar = false;
  // `[N]`: the array constructor. `count` is the N, already folded.
  bool isArray = false;
  // Whether a number was written between the brackets at all. `[]` and `[0]`
  // both arrive with `count == 0` and they are two different types with two
  // different rules -- the slice, and an object of no elements -- so the reader
  // has to be able to tell them apart (`arrays.md` decision 5,
  // `slices.md`).
  bool hasCount = false;
  // The count of an `[N]` part, when one was written.
  //
  // It is a *value* and not a spelling on purpose: the store compares counts to
  // decide whether two types are one, so `[0x10]i32` and `[16]i32` have to be the
  // same number by the time they get there (`arrays.md` decision 19). The reader
  // that builds this part owns the fold, and it is the same
  // `support::parseIntegerLiteral` the `#if` evaluator and the literal checker
  // use, with the same base rule: `[010]i32` is the leading-zero error it is
  // everywhere else in `.mx`.
  std::uint64_t count = 0;
  // The count was written and could not be read as a 64-bit number -- an
  // `[18446744073709551616]i32`, and nothing smaller. Implies `hasCount`.
  bool countOverflow = false;
  // `[_]`: the count the *initializer* supplies (`arrays.md`). Legal only as the
  // outermost constructor of a typed initializer, where there is a list to count,
  // which is why it is a part of the type position and not a spelling the reader
  // rewrites before anyone sees it: `let a: [_]i32` has no list, and the refusal
  // has to name the position it was written in.
  bool countInferred = false;
  // A `!`, which is a type only on its own: `!` and nothing else, in a return
  // position. It is a part of the run rather than a word because it is a
  // punctuator -- it cannot be spelled by an identifier, which is the whole
  // reason the bottom type is `!` and not a reserved word (`never.md`).
  bool isBang = false;
  // Empty for a `*` and for a `!`.
  std::string_view word;
};

// The whole type position. A `*` after the words is refused by name -- the one
// spelling the language has is `*T`, and a reader who wrote `i32*` has one
// character to move, which is exactly what the message says.
//
// `!` is accepted here and only here as the *whole* run. Whether it is legal in
// the position it was written in is the caller's question, not this reader's:
// the same `Type` node appears after `fn` and after `:`, and only the caller
// knows which one it is holding (`never.md`, *Written where*).
// The constructors are applied from the *innermost* out: the part nearest the
// words is the one closest to the element, so the run is walked in reverse and
// `[2][3]i32` is two arrays of three (`arrays.md` decision 18).
//
// `inferredCount` is the count of an outermost `[_]`, and the caller is the only
// one that can supply it: it comes from the elements of the initializer the type
// belongs to. A `_` with nothing here is refused, which is why the default is the
// *safe* one -- a call site that knows nothing about initializers gets the
// sentence, not a count of zero.
[[nodiscard]] TypeSpecResult readType(std::span<const TypePart> parts, TypeStore& types,
                                      std::optional<std::uint64_t> inferredCount = std::nullopt);

// Every spelling the reader accepts, for a "did you mean ...?" suggestion.
// Names only, not the valid *combinations*: a suggestion is about one word.
[[nodiscard]] std::span<const std::string_view> typeNames();

// Whether this one word in the table is a type **on this target**. The table is
// the reader's vocabulary and the target decides one entry of it: `f80` names the
// x87 format, so it is a type where there is x87 and a refusal where there is not
// (`sema.md` decision 26). A caller that offers a name back to a reader -- the
// suggestion search, whose whole promise is that the spelling it proposes then
// works -- asks here rather than repeating the rule.
[[nodiscard]] bool typeNameOnTarget(std::string_view name, const TargetInfo& target);

} // namespace minc::sema
