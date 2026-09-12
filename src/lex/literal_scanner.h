// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Literal scanning: numbers, strings, and character literals.
//
// Internal to the lex module. Split out of `lexer.cc` because these are the
// only scanners that need character sets and back-tracking; the dispatch in
// `lexer.cc` stays a readable table of "which scanner owns this byte".
//
// Every function follows the raw-lexer contract: it consumes at least the byte
// at `offset` (so a caller looping from 0 always progresses), copies nothing,
// and reports anything malformed as flags on the returned token instead of
// producing a diagnostic.
#pragma once

#include <cstdint>
#include <string_view>

#include "lex/token.h"

namespace minc::lex::detail {

// Precondition: `offset < text.size()` and the byte at `offset` is an ASCII
// digit, or a '.' immediately followed by one.
[[nodiscard]] Token scanNumber(std::string_view text, std::uint32_t offset);

// Precondition: `text[offset] == '"'`.
[[nodiscard]] Token scanString(std::string_view text, std::uint32_t offset);

// Precondition: `text[offset] == '\''`.
[[nodiscard]] Token scanChar(std::string_view text, std::uint32_t offset);

} // namespace minc::lex::detail
