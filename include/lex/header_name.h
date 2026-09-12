// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Header-names: `<h-char-sequence>` and `"q-char-sequence"` (C 6.4.7).
//
// A header-name is part of the lexical grammar, but it is not something the
// plain lexer can produce, because whether `<stdio.h>` is a header-name or a
// less-than followed by identifiers depends entirely on what precedes it: only
// `#include`, `#include_next`, and `__has_include` introduce one. Clang reaches
// the same place from the other side -- its preprocessor re-lexes the operand
// with `LexHeaderName` -- and this is that, spelled as a second entry point
// rather than a mode: `lexOne` stays a pure, restartable function of
// `(text, offset)` that knows nothing about directives.
//
// Why the operand cannot be recovered from tokens instead, which is what this
// code exists to replace:
//
//   * inside a header-name `//` and `/*` are ordinary characters -- `h-char` and
//     `q-char` exclude only the newline and the closing delimiter -- so the
//     plain lexer turns `#include <a//b.h>` into a line comment that swallows
//     the `>`, and the directive ends up reported as unterminated;
//   * escapes are not processed either, so lexing `#include "c:\dir\x.h"` as a
//     string literal reports an invalid escape on valid code, which is the worst
//     kind of diagnostic;
//   * `>` is legal inside `"..."` and `"` is legal inside `<...>`, so no amount
//     of joining tokens can express both.
//
// Reading the raw bytes answers all three, and the bytes are still there to
// read: this is why trivia is kept in the token stream.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace minc::lex {

// A header-name as written, with the delimiters counted as part of it so a caret
// can underline the whole thing the way a reader sees it.
struct HeaderName {
  // At the opening delimiter.
  std::uint32_t offset = 0;
  // Including both delimiters.
  std::uint32_t length = 0;
  // `<...>` rather than `"..."`. The include search order depends on it, so it
  // is part of the value rather than something the caller re-derives by looking
  // at the first byte again.
  bool angle = false;
  // The name between the delimiters, as a view into the text that was scanned.
  std::string_view text;
};

// Scans a header-name whose opening delimiter is at `offset`.
//
// Returns `std::nullopt` when the delimiter never closes before a newline or the
// end of the text. That is the truthful answer and the caller reports it; a
// newline ends the attempt because `.mx` has no line splicing ([`lexer.md`],
// decision 4), so a header-name cannot span one.
[[nodiscard]] std::optional<HeaderName> scanHeaderName(std::string_view text, std::uint32_t offset);

} // namespace minc::lex
