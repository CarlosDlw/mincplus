// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The debug information, in the one stage that still has the correspondence
// between an instruction and the node it came from.
//
// Four things here are worth the reading, and each one is a decision rather than
// an API detail.
//
// 1. **`DW_LANG_C99`.** Not because the language is C -- it is not -- but because
//    a debugger picks its *expression parser* from this field, and every debugger
//    in existence parses C expressions. A value it does not know makes `print
//    x * 2` fail with a message about the language; a value it knows makes the
//    expression parser that is right for this language's operators (which are
//    C's) work. It is the closest true statement DWARF can carry.
// 2. **Records, never intrinsics.** `insertDeclare` on the `InsertPosition`
//    overload produces a `#dbg_declare` record, which is what LLVM 19+ made the
//    default representation. `llvm.dbg.declare` calls are the legacy form and the
//    reference forbids the two in one module -- a module that mixes them verifies
//    and then produces a debugger that lies. `invariants.cc` scans for the
//    intrinsic and a test asserts its absence.
// 3. **Every type is read twice from `sema`.** The name from
//    `TypeStore::spelling`, the width from `TypeStore::sizeOf`. A debug type with
//    its own idea of a width shows a reader the wrong number in the debugger,
//    which is worse than showing them nothing, so there is no second table.
// 4. **`finalize()` is not optional.** `createCompileUnit`'s retained arrays and
//    the subprograms are temporaries until then; a module that skipped it prints
//    `<temporary>` in the interesting places, and the failure looks like a bug in
//    LLVM.
#include "debug.h"

#include <cstdint>
#include <string>
#include <utility>

#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include "sema/type.h"
#include "sema/type_store.h"
#include "support/line/line_col.h"
#include "support/source/source_file.h"

namespace minc::ir {
namespace {

// The DWARF encoding for an integer. `DW_ATE_boolean` and `DW_ATE_unsigned_char`
// are their own values because a debugger prints and interprets them differently
// from a plain integer of the same width; that difference is the whole reason
// `bool` and `char` are distinct types here and not aliases.
[[nodiscard]] unsigned integerEncoding(const sema::Type& type) {
  return type.isSigned ? llvm::dwarf::DW_ATE_signed : llvm::dwarf::DW_ATE_unsigned;
}

// A line and a column, or 1:1. `lookup` clamps, so an offset at end of file
// resolves to the position just past the last byte rather than wrapping -- and a
// debug line of 0 is invalid DWARF, which is why the floor is 1 and not 0.
[[nodiscard]] support::LineCol positionOf(const support::SourceFile& source, std::uint32_t offset) {
  const support::LineCol position = source.lookup(offset);
  support::LineCol safe = position;
  if (safe.line == 0) {
    safe.line = 1;
  }
  if (safe.col == 0) {
    safe.col = 1;
  }
  return safe;
}

} // namespace

DebugInfo::DebugInfo(llvm::Module& module, const support::SourceFile& source,
                     std::string_view producer)
    : module_(module), source_(source), builder_(module) {
  // The path is split the way DWARF splits it, with `llvm::sys::path` and not with
  // a search for `/`: on Windows a path has both separators and the platform
  // layer is the only thing that knows which is which, and a filename with a
  // backslash in it is a filename a debugger cannot open.
  const llvm::StringRef path(source_.path);
  llvm::StringRef directory = llvm::sys::path::parent_path(path);
  llvm::StringRef filename = llvm::sys::path::filename(path);
  file_ = builder_.createFile(filename.empty() ? llvm::StringRef("<unit>") : filename, directory);

  unit_ = builder_.createCompileUnit(llvm::dwarf::DW_LANG_C99, file_, std::string(producer),
                                     /*isOptimized=*/false, /*Flags=*/"", /*RV=*/0);

  // The two module flags. `createCompileUnit` writes `!llvm.dbg.cu` itself, and
  // these are the pair that a *consumer* checks before it will read that named
  // node at all: a missing `"Debug Info Version"` is a module the verifier's
  // upgrade pass strips, and the message a reader gets is about metadata rather
  // than about the flag. Guarded, because a second `DebugInfo` for one module (a
  // test, a future linker step) must not add a duplicate flag.
  if (module_.getModuleFlag("Debug Info Version") == nullptr) {
    module_.addModuleFlag(llvm::Module::Error, "Debug Info Version",
                          static_cast<std::uint32_t>(llvm::DEBUG_METADATA_VERSION));
  }
  if (module_.getModuleFlag("Dwarf Version") == nullptr) {
    module_.addModuleFlag(llvm::Module::Error, "Dwarf Version",
                          static_cast<std::uint32_t>(llvm::dwarf::DWARF_VERSION));
  }
}

DebugInfo::~DebugInfo() = default;

llvm::DebugLoc DebugInfo::locationAt(support::Span span) const {
  // A span from an included file, a malformed one, or one from before the unit's
  // copy of its file was created: all of them are "no location", and an empty
  // `DebugLoc` is how LLVM spells that.
  if (!span.valid() || span.file != source_.id) {
    return {};
  }
  const support::LineCol position = positionOf(source_, span.begin);
  llvm::DIScope* const scope = subprogram_ != nullptr ? static_cast<llvm::DIScope*>(subprogram_)
                                                      : static_cast<llvm::DIScope*>(file_);
  return llvm::DebugLoc(
      llvm::DILocation::get(module_.getContext(), position.line, position.col, scope));
}

void DebugInfo::enterFunction(llvm::Function& function, std::string_view name,
                              std::string_view linkageName, const sema::TypeStore& types,
                              sema::TypeId functionType, support::Span span) {
  // Leaving a scope that was never closed would silently nest every subprogram
  // inside the previous one, which is a debugger showing the wrong call frames
  // rather than an error anywhere.
  leaveFunction();

  const support::LineCol position = span.valid() && span.file == source_.id
                                        ? positionOf(source_, span.begin)
                                        : support::LineCol{1, 1};

  // The function's own type: element 0 of a subroutine type is its return type and
  // the rest are the parameters, which is DWARF's convention and not a choice.
  llvm::SmallVector<llvm::Metadata*, 8> elements = subroutineElements(types, functionType);
  llvm::DISubroutineType* const signature =
      builder_.createSubroutineType(builder_.getOrCreateTypeArray(elements));

  // `DISubprogram::SPFlagDefinition` and not `SPFlagLocalToUnit`: every function
  // this stage defines has external linkage, and marking one local would tell the
  // debugger it is invisible outside the unit.
  subprogram_ = builder_.createFunction(
      file_, std::string(name), std::string(linkageName), file_, position.line, signature,
      position.line, llvm::DINode::FlagPrototyped, llvm::DISubprogram::SPFlagDefinition);
  function.setSubprogram(subprogram_);
}

llvm::SmallVector<llvm::Metadata*, 8> DebugInfo::subroutineElements(const sema::TypeStore& types,
                                                                    sema::TypeId functionType) {
  llvm::SmallVector<llvm::Metadata*, 8> elements;
  // Element 0 is the return type and the rest are the parameters, which is
  // DWARF's convention and not a choice.
  elements.push_back(
      types.known(functionType) ? debugType(types, types.get(functionType).returnType) : nullptr);
  if (!types.known(functionType)) {
    return elements;
  }
  for (const sema::TypeId param : types.paramsOf(functionType)) {
    elements.push_back(debugType(types, param));
  }
  if (types.isVariadic(functionType)) {
    // DWARF's marker for `...`: a null entry after the parameters. Without it a
    // debugger reads a variadic function as a fixed-arity one and shows the
    // wrong arguments for a frame inside `printf`.
    elements.push_back(nullptr);
  }
  return elements;
}

void DebugInfo::leaveFunction() {
  subprogram_ = nullptr;
}

llvm::DIType* DebugInfo::debugType(const sema::TypeStore& types, sema::TypeId id) {
  if (!types.known(id)) {
    return nullptr;
  }
  const auto existing = types_.find(id.index);
  if (existing != types_.end()) {
    return existing->second;
  }

  const sema::Type& type = types.get(id);
  llvm::DIType* node = nullptr;
  switch (type.kind) {
  case sema::TypeKind::Void:
  case sema::TypeKind::Never:
  case sema::TypeKind::Error:
    // Nothing to say. `Error` cannot reach here on a checked tree -- `run()`
    // refuses it first -- and neither `void` nor `!` has a type in DWARF. `!` is
    // the same answer for the same reason: it has no value for a debugger to
    // describe, and a function returning it is `void` on the ABI side.
    return nullptr;
  case sema::TypeKind::Bool:
    // The object is a byte (`memory.md`, *Objects*), and saying `i1` here would
    // tell the debugger the object is one bit wide.
    node = builder_.createBasicType("bool", 8, llvm::dwarf::DW_ATE_boolean);
    break;
  case sema::TypeKind::Char:
    node = builder_.createBasicType("char", 8, llvm::dwarf::DW_ATE_unsigned_char);
    break;
  case sema::TypeKind::Int:
    node = builder_.createBasicType(std::string(types.spelling(id)),
                                    static_cast<std::uint64_t>(types.sizeOf(id)) * 8,
                                    integerEncoding(type));
    break;
  case sema::TypeKind::Float:
    node = builder_.createBasicType(std::string(types.spelling(id)),
                                    static_cast<std::uint64_t>(types.sizeOf(id)) * 8,
                                    llvm::dwarf::DW_ATE_float);
    break;
  case sema::TypeKind::Str:
    // `str` is a NUL-terminated pointer in the language and a pointer in the
    // debugger too; calling it anything else would show a reader a struct-like
    // object where the program has an address.
    node = builder_.createPointerType(
        builder_.createBasicType("char", 8, llvm::dwarf::DW_ATE_unsigned_char),
        static_cast<std::uint64_t>(types.target().pointerBits), 0, std::nullopt, "str");
    break;
  case sema::TypeKind::Pointer:
    node = builder_.createPointerType(debugType(types, types.pointeeOf(id)),
                                      static_cast<std::uint64_t>(types.target().pointerBits), 0,
                                      std::nullopt, std::string(types.spelling(id)));
    break;
  case sema::TypeKind::Function: {
    llvm::SmallVector<llvm::Metadata*, 8> params = subroutineElements(types, id);
    node = builder_.createSubroutineType(builder_.getOrCreateTypeArray(params));
    // Reached through the `DISubprogram` that names it, so it needs no retain of
    // its own -- the subprogram is the anchor.
    types_.emplace(id.index, node);
    return node;
  }
  case sema::TypeKind::IntLiteral:
  case sema::TypeKind::FloatLiteral:
    // A deferred literal cannot reach here: `run()` refuses it, because a
    // deferred type has no width to show anybody.
    return nullptr;
  case sema::TypeKind::Array:
    // Reserved until the syntax that builds one lands, and an unspecified type
    // is the honest answer in the meantime: it has the language's spelling and no
    // claim about a layout the language has not decided.
    node = builder_.createUnspecifiedType(std::string(types.spelling(id)));
    break;
  }

  if (node == nullptr) {
    return nullptr;
  }
  // Retained, because none of these is reachable from the compile unit by itself:
  // DWARF keeps only the types that are anchored or retained, and a local
  // variable's type would otherwise be dropped and read as `<unknown type>`.
  builder_.retainType(node);
  types_.emplace(id.index, node);
  return node;
}

void DebugInfo::declareBinding(llvm::AllocaInst& alloca, std::string_view name,
                               const sema::TypeStore& types, sema::TypeId type,
                               support::Span span) {
  if (subprogram_ == nullptr || name.empty()) {
    return;
  }
  llvm::DIType* const debugTypeNode = debugType(types, type);
  if (debugTypeNode == nullptr) {
    return;
  }
  const support::LineCol position = span.valid() && span.file == source_.id
                                        ? positionOf(source_, span.begin)
                                        : support::LineCol{1, 1};

  // AlwaysPreserve false: a binding no code reads is a variable the debugger has
  // no reason to keep, and preserving every one would pin temporaries the
  // optimizer is entitled to delete.
  llvm::DILocalVariable* const variable = builder_.createAutoVariable(
      subprogram_, std::string(name), file_, position.line, debugTypeNode);

  // The location is the scope and nothing else -- line 0, column 0 -- because
  // `#dbg_declare` is a *declaration*: pointing it at the initializing expression
  // tells a stepping debugger the variable came into being there, which is the
  // one thing it is not.
  llvm::DebugLoc location(llvm::DILocation::get(module_.getContext(), 0, 0, subprogram_));

  // Right after the slot. `getIterator()` is `end()` when the alloca is the last
  // instruction in the entry block, and `InsertPosition` accepts that: it is the
  // position that means "trailing records of this block", which is a well-defined
  // place in LLVM's new debug format and is where the record belongs.
  const llvm::BasicBlock::iterator after = std::next(alloca.getIterator());
  (void)builder_.insertDeclare(&alloca, variable, builder_.createExpression(), location,
                               llvm::InsertPosition(after));
}

void DebugInfo::finalize() {
  if (finalized_) {
    return;
  }
  finalized_ = true;
  builder_.finalize();
}

} // namespace minc::ir
