// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// `mincc lex <files...>`: show what the lexer sees.
//
// This is the first subcommand with real behaviour, and it is deliberately the
// narrowest one: it runs no preprocessing and no parsing, so what it prints is
// exactly the module it names. That makes it useful for reviewing a lexer
// change, for writing a regression test from real output, and for the language
// server later -- all of which need the raw token stream, not a parse.
#pragma once

#include "driver/cli.h"

namespace minc::driver {

// Token dump on stdout, lexical diagnostics on stderr. Returns
// ExitCode::Failure when any input could not be read or lexed.
[[nodiscard]] int runLex(const CliOptions& options);

} // namespace minc::driver
