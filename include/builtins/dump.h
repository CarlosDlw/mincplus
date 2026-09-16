// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The table, as a terminal sees it.
//
// Here rather than in the driver for the same reason the table is here and not in
// `resolve`: the *page* and the CLI have to agree, and the way to make them agree
// is one renderer over `all()` -- a builtin the table does not have cannot appear
// in the list, and one it does have cannot be missing from it.
//
// The columns are computed from the data and never written down, so a longer
// spelling or a new status widens a column instead of misaligning the table, which
// is the rule `dumpTokens` already follows for the lexer's.
//
// No color, and no `support` dependency to get it: a module whose whole argument
// is that four stages may include it without dragging anything else in does not
// grow a terminal's concern to print a list of facts. A color mode would be the
// first include, and it would be needed by nobody (`builtins` has no diagnostics).
#pragma once

#include <string>

namespace minc::builtins {

// Every row, one per line, with its signature, its spelling class, its status, what
// it lowers to, and the sentence that says what it means.
[[nodiscard]] std::string dumpBuiltins();

} // namespace minc::builtins
