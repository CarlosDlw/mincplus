// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The invariant scan: the module this compiler built, checked against the rules
// it promised to obey.
//
// `llvm::verifyModule` proves a module is *well formed*. This proves it is
// **ours**: that the closed list of assumptions in `ir.md` § *The assumption
// list* is the list actually in the module, that every division went through a
// guard, and that every access states the alignment its type requires. The
// verifier cannot check any of those, because none of them is a rule about LLVM
// -- they are rules about this language, and the whole reason they are written
// down is that a violation of one of them is a *miscompile* rather than a
// diagnostic.
//
// It is a separate translation unit and a separate function because it is a
// different question asked of the same artifact, and because it is the thing a
// future contributor is supposed to *extend* when they add an assumption: one
// row in one table, plus the check that reads it.
#pragma once

#include <vector>

#include "ir/ir.h"

namespace minc::ir {

// Every violated invariant, in the order they were found. Empty means the module
// is one this compiler is allowed to have built.
//
// A non-empty result is *always* a bug in this compiler -- the program it came
// from was checked and accepted -- so a caller reports these the way it reports
// an internal error, with the module dumped beside them.
[[nodiscard]] std::vector<IRDiagnostic> scanModule(const Module& module);

} // namespace minc::ir
