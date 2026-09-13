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
[[nodiscard]] TypeSpecResult readTypeSpec(std::span<const std::string_view> words,
                                          TypeStore& types);

// Every spelling the reader accepts, for a "did you mean ...?" suggestion.
// Names only, not the valid *combinations*: a suggestion is about one word.
[[nodiscard]] std::span<const std::string_view> typeNames();

} // namespace minc::sema
