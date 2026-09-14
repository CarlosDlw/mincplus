// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Debug information, built in `ir` and nowhere else.
//
// `codegen.md` § *Debug information* states the decision and this file is it:
// **`ir` builds the metadata, `codegen` only refuses to lose it.** The reason is
// a matter of what each stage still has. Correlating an instruction with a source
// span needs the `ast::AstId` that produced it, and that correspondence is gone
// the moment the lowering returns -- so a stage that wanted to add `!dbg`
// afterwards would have to rebuild it from the module, which is exactly the
// second walk `ir.md` refuses everywhere else.
//
// Two rules shape the whole file.
//
// **The lowering decides nothing, and neither does this.** Every line number
// comes from `SourceFile`'s line table and every type comes from `sema`'s
// `TypeStore` -- spelled with `TypeStore::spelling`, sized with
// `TypeStore::sizeOf`. There is no second type system here; a debug type that
// disagreed with the checker about a width would be a debugger that shows the
// wrong number, and the way to prevent that is to read one number twice.
//
// **A permit-list, not a deny-list.** `!dbg` says *where the code came from* and
// licenses no transformation, which is why the assumption scan names it as the
// one permitted attachment; nothing else may appear.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include "sema/type.h"
#include "sema/type_store.h"
#include "support/source/source_file.h"
#include "support/span/span.h"

namespace minc::ir {

// One unit's debug information, alive for one `Lowering`.
//
// It owns a `DIBuilder` and the two anchors LLVM wants (`llvm.dbg.cu`, the
// `"Debug Info Version"` flag), maps a `Span` to a `DILocation`, and hands out a
// `DISubprogram` per function. Nothing here is optional at the call site: the
// lowering asks `enabled()` once and every other method is a no-op on a
// `DebugInfo` that does not exist, because the alternative -- an `if` on every
// instruction -- is a rule with a hole in it.
class DebugInfo {
public:
  // Requires a module already carrying the target triple and data layout, and a
  // source file that is the unit's main file. `producer` becomes
  // `DW_AT_producer`, which is what `readelf --debug-dump=info` prints.
  DebugInfo(llvm::Module& module, const support::SourceFile& source, std::string_view producer);
  ~DebugInfo();
  DebugInfo(const DebugInfo&) = delete;
  DebugInfo& operator=(const DebugInfo&) = delete;

  // The location a span maps to, in the current function's scope when one is
  // open and in the compile unit otherwise. Empty for a span from another file
  // or one that does not resolve: an empty `DebugLoc` attaches nothing, which is
  // the honest answer for "this came from a macro in another file".
  [[nodiscard]] llvm::DebugLoc locationAt(support::Span span) const;

  // Opens a function scope for `function` and attaches the `DISubprogram` to it.
  // `name` is the source spelling, `linkageName` the symbol the linker sees,
  // `functionType` is `sema`'s type of the signature (so the parameter types in
  // the debugger are the checker's, not a second reading of the syntax), and
  // `span` is the declaration, so the line number is the one the reader wrote.
  void enterFunction(llvm::Function& function, std::string_view name, std::string_view linkageName,
                     const sema::TypeStore& types, sema::TypeId functionType, support::Span span);
  // Closes it. Safe to call with no scope open.
  void leaveFunction();
  [[nodiscard]] bool inFunction() const {
    return subprogram_ != nullptr;
  }

  // A binding, declared at its frame slot. A `#dbg_declare` record and not an
  // `llvm.dbg.declare` call: LLVM 19+ made records the default representation and
  // the reference forbids mixing the two in one module, so the module carries one
  // representation or the other and never both.
  void declareBinding(llvm::AllocaInst& alloca, std::string_view name, const sema::TypeStore& types,
                      sema::TypeId type, support::Span span);

  // Resolves every temporary node and the compile unit's arrays. Must run before
  // the module is verified, printed or handed to `codegen`: a module with
  // unresolved temporaries verifies intermittently and prints as `<temporary>`.
  void finalize();

private:
  // One `DIType` per `sema::TypeId`, so a type is one node and a pointer's
  // pointee is *the same* node the pointee has on its own. Keyed on the store's
  // id, which is the only thing that makes two spellings one type.
  [[nodiscard]] llvm::DIType* debugType(const sema::TypeStore& types, sema::TypeId id);

  // Keyed on `TypeId::index` rather than on the id: the id is a struct with a
  // defaulted `operator==` and no `std::hash`, and adding one to `sema` so a
  // debug map can have it would put this stage's convenience into a type the
  // whole pipeline shares.
  std::unordered_map<std::uint32_t, llvm::DIType*> types_;

  llvm::Module& module_;
  const support::SourceFile& source_;
  llvm::DIBuilder builder_;
  llvm::DICompileUnit* unit_ = nullptr;
  llvm::DIFile* file_ = nullptr;
  llvm::DISubprogram* subprogram_ = nullptr;
  bool finalized_ = false;
};

} // namespace minc::ir
