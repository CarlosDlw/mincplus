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

// One element of a type position, in source order: a `*`, or a word.
//
// A type position is `* * ... * <words>` and nothing else, because that is what
// the grammar accepts (`parser.md`): the `*` is *before* what it points to, so a
// pointer is a prefix over the same run of words every other type is.
struct TypePart {
  bool isStar = false;
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
[[nodiscard]] TypeSpecResult readType(std::span<const TypePart> parts, TypeStore& types);

// Every spelling the reader accepts, for a "did you mean ...?" suggestion.
// Names only, not the valid *combinations*: a suggestion is about one word.
[[nodiscard]] std::span<const std::string_view> typeNames();

} // namespace minc::sema
