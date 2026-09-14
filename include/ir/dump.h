// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Printing a lowered module.
//
// Here and not in `ir.h` because printing is the one operation that needs LLVM's
// own text format, and a stage that only wants to *build* IR should not have to
// see it. The alternative -- a hand-written printer -- would be a second
// implementation of LLVM's assembly, and the one thing it would guarantee is
// that the two disagree about something.
#pragma once

#include <string>

#include "ir/ir.h"

namespace minc::ir {

// The module's textual form: what `mincc ir` prints, and what a test can assert
// a *shape* against (`no `nsw` anywhere`, `one global per distinct spelling`).
//
// Deliberately not a golden-file format (`ir.md`, *Tests*): the text changes
// when LLVM changes, when the target changes and when a comment in a pass
// changes, so freezing it would freeze a moving thing and the test would be
// regenerated until it stopped testing. Behaviour is what is frozen, and that
// lives in the `run` oracle.
//
// An unbuilt handle prints the empty string rather than throwing: a caller that
// asks to print nothing gets nothing, and the failure it cares about is
// `IRResult::failed()`, which it has already had to check.
[[nodiscard]] std::string dumpModule(const Module& module);

} // namespace minc::ir
