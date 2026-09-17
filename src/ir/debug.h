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
#include <vector>

#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include "alias_name.h"
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
  //
  // `templateParams` and `templateArgs` are the instance's two lists, positionally
  // paired: the declaration's binders (one `Param` per binder, in order) and the
  // arguments they were instantiated with. Both are empty for a function the source
  // wrote whole. A non-empty pair emits one `DW_TAG_template_type_parameter` per
  // binder, named by the *binder* (`T`) and typed by the **argument**, which is
  // what clang and rustc both emit and what lets a debugger answer "instantiated
  // with `i32`" without a dictionary (`generics.md`, § 8).
  void enterFunction(llvm::Function& function, std::string_view name, std::string_view linkageName,
                     const sema::TypeStore& types, sema::TypeId functionType, support::Span span,
                     std::span<const sema::TypeId> templateParams = {},
                     std::span<const sema::TypeId> templateArgs = {});
  // Closes it. Safe to call with no scope open.
  void leaveFunction();
  [[nodiscard]] bool inFunction() const {
    return subprogram_ != nullptr;
  }

  // A nested scope: every block inside a function body is one, so a name declared
  // in a block is *out of scope* outside it. That is not bookkeeping -- two
  // bindings may share a spelling (`for let i` inside a function that already has
  // an `i`), and without the block a debugger has two variables in scope on every
  // line and answers `print i` with whichever it found first. `span` is the
  // block's opening brace, which is where a debugger points when it says "this
  // scope".
  void openBlock(support::Span span);
  void closeBlock();

  // A binding, declared at its frame slot. A `#dbg_declare` record and not an
  // `llvm.dbg.declare` call: LLVM 19+ made records the default representation and
  // the reference forbids mixing the two in one module, so the module carries one
  // representation or the other and never both.
  //
  // `parameterNumber` is what turns the record into a *parameter*: the number a
  // formal parameter has in its signature, counting from one, and `0` for every
  // ordinary binding. It is the difference between `DW_TAG_formal_parameter` and
  // `DW_TAG_variable` in the object, and the difference is visible to a reader:
  // a debugger that is not told which children of a subprogram are its arguments
  // answers "No arguments" for every frame (see `checks.md` for the survey of
  // what a debugger asks for).
  void declareBinding(llvm::AllocaInst& alloca, std::string_view name, const sema::TypeStore& types,
                      sema::TypeId type, support::Span span, unsigned parameterNumber = 0,
                      const AliasName& alias = {});

  // The same, for a binding whose storage is the *value it arrived as*: an
  // aggregate parameter, whose storage is the pointer to the caller's copy
  // (`arrays.md` decision 13). An argument is not an instruction, so there is
  // nothing to sit behind -- `position` is where in the entry block the record
  // goes, which is "before the body runs", the same place an `alloca`'s record
  // sits.
  void declareParameterBinding(llvm::Argument& storage, std::string_view name,
                               const sema::TypeStore& types, sema::TypeId type, support::Span span,
                               unsigned parameterNumber, llvm::BasicBlock::iterator where,
                               const AliasName& alias = {});

  // A file-scope object. `DIGlobalVariableExpression` is the only form of global
  // debug information LLVM has: a `GlobalVariable` with no expression is a symbol
  // the debugger cannot name, so `-g` on a file that declares constants would
  // produce a `gdb` that answers "no such variable" about a name the source
  // writes. It is attached to the object here and not collected later, because
  // the object already exists by the time this runs.
  void declareGlobal(llvm::GlobalVariable& global, std::string_view name,
                     const sema::TypeStore& types, sema::TypeId type, support::Span span,
                     const AliasName& alias = {});

  // Resolves every temporary node and the compile unit's arrays. Must run before
  // the module is verified, printed or handed to `codegen`: a module with
  // unresolved temporaries verifies intermittently and prints as `<temporary>`.
  void finalize();

private:
  // One `DIType` per `sema::TypeId`, so a type is one node and a pointer's
  // pointee is *the same* node the pointee has on its own. Keyed on the store's
  // id, which is the only thing that makes two spellings one type.
  [[nodiscard]] llvm::DIType* debugType(const sema::TypeStore& types, sema::TypeId id);
  // One binding record: the storage, the name, the language's type, the position,
  // and whether it is a parameter. Shared so that the two spellings of "a binding"
  // above cannot drift -- a parameter and a local have to appear to a debugger the
  // same way -- and so that the one place a variable is created is the one place
  // the parameter question is asked.
  void declareAt(llvm::Value& storage, std::string_view name, const sema::TypeStore& types,
                 sema::TypeId type, support::Span span, unsigned parameterNumber,
                 llvm::BasicBlock::iterator where, const AliasName& alias);
  // The type a *binding* is described with: the `DW_TAG_typedef` for the name the
  // source wrote at the position, or the underlying type when the position wrote a
  // type. One DIE per declaration, cached, so a name used at ten positions is one
  // node -- which is what makes `ptype` answer the name and not a copy of it.
  [[nodiscard]] llvm::DIType* aliasType(const sema::TypeStore& types, sema::TypeId type,
                                        const AliasName& alias);
  // The element list of a `DISubroutineType`: element 0 is the return type, the
  // rest are the parameters, and a variadic function ends with a null entry --
  // DWARF's marker for `...`. One function because two call sites build it (a
  // function's own signature and a function *type* like a pointer's pointee) and
  // a second copy is a second chance to forget the marker.
  [[nodiscard]] llvm::SmallVector<llvm::Metadata*, 8>
  subroutineElements(const sema::TypeStore& types, sema::TypeId functionType);

  // Keyed on `TypeId::index` rather than on the id: the id is a struct with a
  // defaulted `operator==` and no `std::hash`, and adding one to `sema` so a
  // debug map can have it would put this stage's convenience into a type the
  // whole pipeline shares.
  std::unordered_map<std::uint32_t, llvm::DIType*> types_;
  // One typedef DIE per `type` declaration, keyed on the declaration's index in
  // `TypedFile::aliases()`. Keyed on the declaration and not on the spelling: two
  // declarations may not share a spelling (`resolve` refuses it), and a *block's*
  // name may hide a file's -- so the index is the only key that says which name a
  // position meant.
  std::unordered_map<std::uint32_t, llvm::DIType*> aliases_;

  llvm::Module& module_;
  const support::SourceFile& source_;
  llvm::DIBuilder builder_;
  llvm::DICompileUnit* unit_ = nullptr;
  llvm::DIFile* file_ = nullptr;
  llvm::DISubprogram* subprogram_ = nullptr;
  // The scope a binding and a location belong to: the subprogram at a function's
  // top level, or the innermost `DW_TAG_lexical_block` inside one. Null between
  // functions.
  [[nodiscard]] llvm::DIScope* currentScope() const;
  std::vector<llvm::DILexicalBlock*> blocks_;
  bool finalized_ = false;
};

} // namespace minc::ir
