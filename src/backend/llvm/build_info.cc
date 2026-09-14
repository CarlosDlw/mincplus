// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Which LLVM this compiler was built against, asked of LLVM rather than written
// down here.
//
// A fact `-vV` prints and nothing decides with, and exactly what a bug report is
// missing when it is absent: an LLVM codegen failure is frequently a fact about
// the LLVM version, and the version is not otherwise visible from the compiler's
// own version number.
//
// This is also why the driver cannot do it itself: `src/driver` may not include
// `llvm/*` (a test greps the tree), so the one place that may -- `src/backend` --
// answers and the driver prints.
#include "backend/codegen.h"

#include <string>

#include "llvm/Config/llvm-config.h"

namespace minc::backend {

std::string llvmVersion() {
  return LLVM_VERSION_STRING;
}

} // namespace minc::backend
