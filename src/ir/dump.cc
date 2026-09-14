// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Printing a lowered module, through LLVM's own writer.
//
// The alternative -- a hand-written printer -- would be a second implementation
// of LLVM's assembly, and the one thing it would guarantee is that the two
// disagree about something. This is why `ir/dump.h` is a header of its own: the
// stages that only *build* IR never include LLVM, and this file is where the
// text format is reached.
#include "ir/dump.h"

#include <string>
#include <utility>

#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include "ir/storage.h"

namespace minc::ir {

std::string dumpModule(const Module& module) {
  // An unbuilt handle prints nothing rather than throwing: the failure a caller
  // cares about is `IRResult::failed()`, which it has already had to check, and
  // a printer that refused would be a second place to get the same answer.
  if (!ModuleAccess::built(module)) {
    return {};
  }
  std::string text;
  llvm::raw_string_ostream stream(text);
  ModuleAccess::llvmModule(module).print(stream, /*AAW=*/nullptr);
  stream.flush();
  return text;
}

} // namespace minc::ir
