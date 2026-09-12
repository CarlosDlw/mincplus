// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Human-readable rendering of a token stream, for `mincc lex`.
//
// Pure formatting, no I/O: it returns a string and never writes, so a test can
// assert on the exact table and the driver decides where it goes. The columns
// are computed from the data, so the table stays aligned whatever the file
// contains and however long the kind names grow.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "lex/token_stream.h"
#include "support/term/terminal.h"

namespace minc::lex {

struct DumpOptions {
  // Ansi adds SGR colors. Alignment is computed on the uncolored text, so
  // turning color on never shifts a column.
  support::ColorMode color = support::ColorMode::Plain;
  // Longest lexeme shown before `...` (in bytes of the input, not of the
  // escaped output), so an escape sequence is never cut in half.
  std::size_t maxSpellingBytes = 48;
  // Print the per-kind tally after the table.
  bool showHistogram = true;
};

// `path` is only used in the header line: the stream knows its FileId, and the
// path lives in the SourceFile it came from.
[[nodiscard]] std::string dumpTokens(const TokenStream& stream, std::string_view path,
                                     DumpOptions options = {});

} // namespace minc::lex
