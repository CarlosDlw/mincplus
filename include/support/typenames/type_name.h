// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The words a type position is *spelled* with, in one place.
//
// A type name is not a keyword: `i32` lexes as an `Identifier`, and the type
// reader recognizes it by spelling (`parser.md`, `typespec.h`). Two stages above
// the lexer then have to answer "is this word a type name", and they have to
// answer it with the *same* list:
//
//   * the **parser**, to decide whether `(i32)x` is a cast or a parenthesised
//     expression -- and that decision is only decidable without a symbol table
//     because a word that is a type name can never be a variable either
//     (`casts.md`, decision 4);
//   * **resolution**, which refuses a *declaration* of one of these names
//     (`let i32 = 5;`), and which needs the same list for the same reason.
//
// It lives here, in `support`, because the parser may not link the checker and
// the checker may not link the parser. `typeNames()` in `sema` is the *reader's*
// table -- the same words plus their widths and signedness -- and a test asserts
// the two agree, so a type added to the language fails a test until it is added
// here, which is the property that keeps `(T)x` parsing correctly the day a name
// is added.
#pragma once

#include <span>
#include <string_view>

namespace minc::support {

// Every word that can take part in a type position: the language's own names
// (`i8`…`usize`, `bool`, `char`, `str`, `void`), the C specifier words
// (`signed`, `unsigned`, `short`, `long`, `int`, `float`, `double`), and the
// shorthands of the C spellings (`uint`, `__int128`).
[[nodiscard]] std::span<const std::string_view> typeNameWords();

// True when `word` is one of them. Linear over a table of thirty, which is what
// the callers are: a lookahead check on `(`, and a declaration's name.
[[nodiscard]] bool isTypeNameWord(std::string_view word);

} // namespace minc::support
