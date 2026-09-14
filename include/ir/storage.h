// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// What `ir::Module` actually owns, and the only way to reach it.
//
// `ir.h` is included by stages that must not link LLVM, so the handle there is
// one pointer and nothing else. This header is the *reason* that works: the
// context, the data layout and the module live together behind that pointer,
// and **this file includes `llvm/*`** -- so a translation unit that includes it
// is one that has crossed the boundary on purpose.
//
// It lives under `include/` rather than beside the lowering because exactly one
// consumer outside `src/ir` needs it: `src/backend`, which has to hand the
// `llvm::Module` to LLVM's code generator. Inside `src/ir` the printer
// (`dump.cc`) and the scanner (`invariants.cc`) use it, which is the pair
// `ir.md` names.
//
// What it is **not** is a general accessor. `Module` has no public
// `llvm::Module&` member function, so the boundary cannot be crossed by
// including a header a stage above already includes; reaching the module takes
// this file, whose name says what it does.
#pragma once

#include <memory>
#include <string>

#include "llvm/IR/DataLayout.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include "ir/ir.h"
#include "support/span/file_id.h"

namespace minc::ir {

// One unit's LLVM state, owned as a whole.
//
// The context is a member and not a temporary because an LLVM type is owned by
// the context that made it: a module outliving its context is a use-after-free,
// and the ownership chain here (`ModuleStorage` -> `LLVMContext` before the
// types that point into it) is what makes that impossible rather than merely
// unlikely. One handle per unit is also what makes two units safe to lower on
// two threads.
struct ModuleStorage {
  // Declared before the module, deliberately: a `Type*` is owned by the context,
  // so the context has to outlive every reference into it, and member order is
  // what states that.
  llvm::LLVMContext context;
  // The default data layout until the lowering installs the target's. A
  // default-constructed `DataLayout` is valid (little-endian, 64-bit pointers),
  // which keeps a reference total even on the path that refuses the target and
  // never emits anything.
  llvm::DataLayout layout;
  std::unique_ptr<llvm::Module> module;
  support::FileId file = support::kInvalidFile;
};

// The internal view of a `Module`. A friend of `Module` (declared in `ir.h`), so
// the printer and the scanner can reach the LLVM module without `Module` growing
// a public `llvm::Module&` accessor that the whole tree could then include.
struct ModuleAccess {
  [[nodiscard]] static bool built(const Module& module) {
    return module.impl_ != nullptr && module.impl_->module != nullptr;
  }
  // Precondition: `built(module)`.
  [[nodiscard]] static llvm::Module& llvmModule(const Module& module) {
    return *module.impl_->module;
  }
  [[nodiscard]] static const llvm::DataLayout& layout(const Module& module) {
    return module.impl_->layout;
  }
  [[nodiscard]] static llvm::LLVMContext& context(const Module& module) {
    return module.impl_->context;
  }
};

} // namespace minc::ir
