// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// What this compiler was built on, asked of LLVM rather than derived here.
//
// Two facts, and a reason to put them behind a function instead of reading the
// macros at the call site: `--version -v` prints them, and they are exactly what
// a bug report is missing when they are absent. The host triple in particular is
// **LLVM's** answer (`getDefaultTargetTriple`) and not a `#if` chain of this
// project's, because LLVM's answer is the one its own backend will use, and a
// second spelling of the host is a second thing to be wrong.
//
// This is also why the driver cannot do it itself: `src/driver` may not include
// `llvm/*` (a test greps the tree), so the one place that may — `src/backend` —
// answers and the driver prints.
#include "backend/codegen.h"

#include <string>

#include "llvm/Config/llvm-config.h"
// LLVM 22 moved the host queries out of `Support` and into `TargetParser`.
#include "llvm/TargetParser/Host.h"

namespace minc::backend {

std::string hostTriple() {
  return llvm::sys::getDefaultTargetTriple();
}

std::string llvmVersion() {
  return LLVM_VERSION_STRING;
}

} // namespace minc::backend
