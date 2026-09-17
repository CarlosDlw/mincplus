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

// The predefined id of the unsigned integer of the pointer's width, which is what
// `usize` is: a spelling that resolves to a registered type and not a type of its
// own (`sema/type.h`), so the length word of a slice descriptor is one of these
// ids and not a name to invent. Asked of the target rather than tabulated, the
// same way `Lowering::pointerIntType` asks for the signed twin -- and read
// through `debugType`, which is keyed by the id, so a `usize` binding and a
// descriptor's length are the *same* type to a reader.
[[nodiscard]] sema::TypeId pointerUnsignedInt(const sema::TypeStore& types) {
  switch (types.target().pointerBits) {
  case 16:
    return sema::kTypeU16;
  case 32:
    return sema::kTypeU32;
  default:
    return sema::kTypeU64;
  }
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

llvm::DIScope* DebugInfo::currentScope() const {
  if (!blocks_.empty()) {
    return blocks_.back();
  }
  if (subprogram_ != nullptr) {
    return subprogram_;
  }
  return file_;
}

llvm::DebugLoc DebugInfo::locationAt(support::Span span) const {
  // A span from an included file, a malformed one, or one from before the unit's
  // copy of its file was created: all of them are "no location", and an empty
  // `DebugLoc` is how LLVM spells that.
  if (!span.valid() || span.file != source_.id) {
    return {};
  }
  const support::LineCol position = positionOf(source_, span.begin);
  return llvm::DebugLoc(
      llvm::DILocation::get(module_.getContext(), position.line, position.col, currentScope()));
}

void DebugInfo::openBlock(support::Span span) {
  if (subprogram_ == nullptr) {
    return;
  }
  // The scope's own line is the block's opening brace. A block inside a block
  // nests in the one around it -- which is what makes the vector a stack and not
  // a single slot: `{ { let a = 1; } }` is two lexical blocks, and a walk that can
  // nest them is the only walk that can lower one.
  const support::LineCol position = span.valid() && span.file == source_.id
                                        ? positionOf(source_, span.begin)
                                        : support::LineCol{1, 1};
  blocks_.push_back(
      builder_.createLexicalBlock(currentScope(), file_, position.line, position.col));
}

void DebugInfo::closeBlock() {
  // Balanced by construction: the lowering opens a block around the statements it
  // lowers and closes it after them, so a pop on an empty stack would be a bug in
  // the walk -- and one that must not *silently* narrow the scope of everything
  // that follows. The guard is the cheap half of that statement.
  if (!blocks_.empty()) {
    blocks_.pop_back();
  }
}

void DebugInfo::enterFunction(llvm::Function& function, std::string_view name,
                              std::string_view linkageName, const sema::TypeStore& types,
                              sema::TypeId functionType, support::Span span,
                              std::span<const sema::TypeId> templateParams,
                              std::span<const sema::TypeId> templateArgs) {
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

  // `SPFlagLocalToUnit` exactly when the symbol is not visible outside the unit,
  // which is what `static` means to a debugger. It is read from the `Function`
  // itself, the same way a global's record reads its own object below, so the
  // debug info and the module cannot disagree about whether this unit owns the
  // name -- and a `static` function is not offered to the debugger as callable
  // from another unit.
  //
  // Built by LLVM's own helper rather than by ORing the bitmask here, and the
  // difference is not style: `SPFlagDefinition | SPFlagLocalToUnit` is `12`,
  // which is a valid bitmask and **not** a named enumerator, so the analyzer
  // reads the combination as a cast out of the enum's range and fails the tidy
  // gate (`clang-analyzer-optin.core.EnumCastOutOfRange`). The helper is the API
  // for this question, so the gate and the correct answer agree.
  const llvm::DISubprogram::DISPFlags flags = llvm::DISubprogram::toSPFlags(
      /*IsLocalToUnit=*/function.hasLocalLinkage(), /*IsDefinition=*/true,
      /*IsOptimized=*/false);
  // The template parameters, for an instance. The name is the binder's -- the word
  // the reader wrote in `<...>` -- and the type is the **argument**, so a debugger
  // reading the DIE can print `identity<i32>` and say which `i32` belongs to which
  // binder without a dictionary passed at run time (`generics.md`, § 8). Empty for
  // every function the source wrote whole, which is why the array is built
  // unconditionally and passed as a null array otherwise: one shape of the call.
  llvm::SmallVector<llvm::Metadata*, 4> templates; // DITemplateTypeParameter, one per binder
  for (std::size_t i = 0; i < templateParams.size(); ++i) {
    const sema::TypeId binder = templateParams[i];
    const sema::TypeId argument = i < templateArgs.size() ? templateArgs[i] : sema::kInvalidType;
    const std::string_view spelling = types.known(binder) && types.isParam(binder)
                                          ? types.get(binder).paramSpelling
                                          : std::string_view("T");
    // The **argument**'s type and the **binder**'s name, which is the pair the two
    // leaders emit: `DW_AT_name` is `T` and `DW_AT_type` is `int`.
    templates.push_back(builder_.createTemplateTypeParameter(
        file_, std::string(spelling), debugType(types, argument), /*IsDefault=*/false));
  }
  // The DIE list is built through the *typed* array constructor, which is what
  // makes it a `DITemplateParameterArray` rather than a generic metadata array:
  // `DISubprogram` takes the parameterized type, and LLVM's assertions check it.
  llvm::DITemplateParameterArray parameters(llvm::MDTuple::get(module_.getContext(), templates));
  subprogram_ = builder_.createFunction(file_, std::string(name), std::string(linkageName), file_,
                                        position.line, signature, position.line,
                                        llvm::DINode::FlagPrototyped, flags, parameters);
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
  // The scopes belong to the function that opened them. Clearing them here and
  // not in `closeBlock` is what keeps a lowering bug from putting the *next*
  // function's first binding inside the previous function's last block.
  blocks_.clear();
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
  case sema::TypeKind::Param:
    // Nothing to say, and the four say it for the same reason: there is no
    // object to describe. `Error` cannot reach here on a checked tree -- `run()`
    // refuses it first; neither `void` nor `!` has a DWARF type (`!` has no value
    // for a debugger to look at, and a function returning it is `void` on the ABI
    // side); and a `Param` has no width at all, which the *mapper* refuses first
    // and states (`generics.md`, decision 20).
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
  case sema::TypeKind::Array: {
    // `DW_TAG_array_type` with one `DISubrange`: the element, the count, and the
    // size and alignment the language's own layout rule gives, read from the
    // store like every other number in this file (`arrays.md` decision 6). A
    // debugger that showed a different element count or a different size than the
    // checker's would be showing a number the compiler does not use.
    llvm::DIType* element = debugType(types, types.elementOf(id));
    if (element == nullptr) {
      return nullptr;
    }
    llvm::Metadata* subrange =
        builder_.getOrCreateSubrange(0, static_cast<std::int64_t>(type.count));
    node = builder_.createArrayType(static_cast<std::uint64_t>(types.sizeOf(id)) * 8,
                                    static_cast<std::uint32_t>(types.alignOf(id)) * 8, element,
                                    builder_.getOrCreateArray({subrange}));
    break;
  }
  case sema::TypeKind::Slice: {
    // `DW_TAG_structure_type` with the descriptor's two members, `ptr` and `len`,
    // named as `slices.md` names them. A debugger that showed only a pointer (the
    // habit from C's `char *`) would leave a reader with no way to see the length
    // the program is walking by, which is the one number a slice adds.
    //
    // The two members are `DW_TAG_member` nodes and not the types they have, and
    // that is not a formality: a struct whose element list holds bare types has
    // no members to emit, so the descriptor reaches a debugger as an opaque
    // sixteen-byte object and `print s.len` is answered "no member named len".
    // A `createMemberType` carries the three things a member has and a type does
    // not -- the name, the offset, and the tag -- so the two are built here with
    // the offsets the descriptor's layout gives them (zero, and the pointer's
    // own width).
    const std::uint64_t pointerBits = static_cast<std::uint64_t>(types.target().pointerBits);
    const std::uint64_t size = static_cast<std::uint64_t>(types.sizeOf(id)) * 8;
    const std::uint32_t align = static_cast<std::uint32_t>(types.alignOf(id)) * 8;
    // The member's alignment in a two-word descriptor is the pointer's, which in
    // bits is the same number as its size: a descriptor is aligned like the word
    // it starts with, and so is each of its two words.
    const std::uint32_t memberAlign = static_cast<std::uint32_t>(pointerBits);
    llvm::DIType* const pointerType = builder_.createPointerType(
        debugType(types, types.elementOf(id)), pointerBits, memberAlign, std::nullopt);
    // The length is the language's `usize` and not a type of this function's own:
    // a basic type named here answers `ptype` with `len len` for a member whose
    // type is `len`, and gives one number two types in one module.
    llvm::DIType* const lengthType = debugType(types, pointerUnsignedInt(types));
    llvm::DIType* const pointerMember =
        builder_.createMemberType(file_, "ptr", file_, 0, pointerBits, memberAlign,
                                  /*OffsetInBits=*/0, llvm::DINode::FlagPublic, pointerType);
    llvm::DIType* const lengthMember = builder_.createMemberType(
        file_, "len", file_, 0, pointerBits, memberAlign, /*OffsetInBits=*/pointerBits,
        llvm::DINode::FlagPublic, lengthType);
    node = builder_.createStructType(currentScope(), std::string(types.spelling(id)), file_, 0,
                                     size, align, llvm::DINode::FlagPublic, nullptr,
                                     builder_.getOrCreateArray({pointerMember, lengthMember}));
    break;
  }
  case sema::TypeKind::Tuple: {
    // `DW_TAG_structure_type`, **with no name**, and one `DW_TAG_member` per
    // position named `__0`, `__1`, ... (`tuples.md`, decision 14).
    //
    // The name is absent because the type has none: a product is *structural* --
    // `(i32, bool)` written in two files is one type -- and inventing a spelling
    // here would put a name in the debug record that the language cannot write and
    // that a reader would reasonably try. A nameless `DW_TAG_structure_type` is
    // exactly what an anonymous C `struct` is, and that is the shape the compiler
    // this language is measured against emits: `ptype` on one prints
    // `struct { int __0; char __1; }` and `print` prints
    // `{__0 = 7, __1 = 1 '\001'}` -- a record with position-named members and no
    // name is readable, which is the whole requirement.
    //
    // `__0` and not `0` is the same measurement: rustc's tuples reach DWARF with
    // `DW_AT_name ("__0")`, because an identifier in DWARF is a name and `0` is a
    // *number* -- and a leading double underscore is the spelling this language
    // already reserves for names no program can write (`tuples.md`, decision 14).
    //
    // The offsets are the store's, read one member at a time and never computed
    // here: the layout is a property of the type, and a debugger that disagreed
    // with `sizeOf` about where member one starts would be describing an object the
    // compiler does not generate (`arrays.md` decision 6, the same rule).
    const std::span<const sema::TypeId> declared = types.membersOf(id);
    llvm::SmallVector<llvm::Metadata*, 8> members;
    members.reserve(declared.size());
    for (std::size_t i = 0; i < declared.size(); ++i) {
      llvm::DIType* member = debugType(types, declared[i]);
      if (member == nullptr) {
        return nullptr;
      }
      const std::uint64_t size = static_cast<std::uint64_t>(types.sizeOf(declared[i])) * 8;
      const std::uint32_t align = static_cast<std::uint32_t>(types.alignOf(declared[i])) * 8;
      const std::uint64_t offset =
          static_cast<std::uint64_t>(types.memberOffset(id, static_cast<std::uint32_t>(i))) * 8;
      members.push_back(builder_.createMemberType(file_, "__" + std::to_string(i), file_, 0, size,
                                                  align, /*OffsetInBits=*/offset,
                                                  llvm::DINode::FlagPublic, member));
    }
    node = builder_.createStructType(currentScope(), /*Name=*/llvm::StringRef{}, file_, 0,
                                     static_cast<std::uint64_t>(types.sizeOf(id)) * 8,
                                     static_cast<std::uint32_t>(types.alignOf(id)) * 8,
                                     llvm::DINode::FlagPublic, nullptr,
                                     builder_.getOrCreateArray(members));
    break;
  }
  case sema::TypeKind::IntLiteral:
  case sema::TypeKind::FloatLiteral:
    // A deferred literal cannot reach here: `run()` refuses it, because a
    // deferred type has no width to show anybody.
    return nullptr;
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
                               const sema::TypeStore& types, sema::TypeId type, support::Span span,
                               unsigned parameterNumber, const AliasName& alias) {
  // Right after the slot. `getIterator()` is `end()` when the alloca is the last
  // instruction in the entry block, and `InsertPosition` accepts that: it is the
  // position that means "trailing records of this block", which is a well-defined
  // place in LLVM's new debug format and is where the record belongs.
  declareAt(alloca, name, types, type, span, parameterNumber, std::next(alloca.getIterator()),
            alias);
}

void DebugInfo::declareParameterBinding(llvm::Argument& storage, std::string_view name,
                                        const sema::TypeStore& types, sema::TypeId type,
                                        support::Span span, unsigned parameterNumber,
                                        llvm::BasicBlock::iterator where, const AliasName& alias) {
  declareAt(storage, name, types, type, span, parameterNumber, where, alias);
}

llvm::DIType* DebugInfo::aliasType(const sema::TypeStore& types, sema::TypeId type,
                                   const AliasName& alias) {
  llvm::DIType* const underlying = debugType(types, type);
  if (underlying == nullptr || alias.index == sema::TypedFile::kNoAlias || alias.spelling.empty()) {
    return underlying;
  }
  if (const auto found = aliases_.find(alias.index); found != aliases_.end()) {
    return found->second;
  }
  const support::LineCol position = alias.span.valid() && alias.span.file == source_.id
                                        ? positionOf(source_, alias.span.begin)
                                        : support::LineCol{1, 1};
  // The scope is the compile unit and not the current block: a name for a type is
  // visible over a region of *text*, not over a region of instructions, and a
  // typedef DIE nested in whichever block happened to declare a variable first
  // would appear and disappear as the lowering walked. `DW_AT_decl_file` and
  // `DW_AT_decl_line` are what a debugger reports, and they are the declaration's.
  llvm::DIType* const node =
      builder_.createTypedef(underlying, std::string(alias.spelling), file_, position.line, unit_);
  aliases_.emplace(alias.index, node);
  return node;
}

void DebugInfo::declareAt(llvm::Value& storage, std::string_view name, const sema::TypeStore& types,
                          sema::TypeId type, support::Span span, unsigned parameterNumber,
                          llvm::BasicBlock::iterator where, const AliasName& alias) {
  if (subprogram_ == nullptr || name.empty()) {
    return;
  }
  // The name the source wrote, when it wrote one (`aliasType`), and the type it
  // stands for otherwise. Every binding of the unit goes through here, which is
  // what makes `let a: Arr` *and* `let b: Arr` point at one typedef DIE.
  llvm::DIType* const debugTypeNode = aliasType(types, type, alias);
  if (debugTypeNode == nullptr) {
    return;
  }
  const support::LineCol position = span.valid() && span.file == source_.id
                                        ? positionOf(source_, span.begin)
                                        : support::LineCol{1, 1};
  // The scope is the innermost block, so a name declared inside a block is out of
  // scope outside it. `declareAt` is the one place a variable is created, which
  // is what makes that a rule rather than a check at each call site.
  llvm::DIScope* const scope = currentScope();

  // **A parameter is a parameter.** `DW_TAG_formal_parameter` and
  // `DW_TAG_variable` are the two children a subprogram can have, and a debugger
  // reads the first kind as "the frame's arguments" -- `gdb`'s `info args`, its
  // call-frame printing, and its `bt` all come from it. A parameter lowered as a
  // variable is a frame whose arguments the debugger reports as "No arguments"
  // and whose `argv`-style printing is missing, which is what this compiler did
  // until the number below was passed through.
  //
  // The number is the parameter's own -- one for the first, counting the `sret`
  // destination out, which is not a parameter anybody wrote.
  //
  // AlwaysPreserve false: a binding no code reads is a variable the debugger has
  // no reason to keep, and preserving every one would pin temporaries the
  // optimizer is entitled to delete.
  llvm::DILocalVariable* const variable =
      parameterNumber == 0
          ? builder_.createAutoVariable(scope, std::string(name), file_, position.line,
                                        debugTypeNode)
          : builder_.createParameterVariable(scope, std::string(name), parameterNumber, file_,
                                             position.line, debugTypeNode);

  // The location is the scope and nothing else -- line 0, column 0 -- because
  // `#dbg_declare` is a *declaration*: pointing it at the initializing expression
  // tells a stepping debugger the variable came into being there, which is the
  // one thing it is not.
  llvm::DebugLoc location(llvm::DILocation::get(module_.getContext(), 0, 0, scope));

  (void)builder_.insertDeclare(&storage, variable, builder_.createExpression(), location,
                               llvm::InsertPosition(where));
}

void DebugInfo::declareGlobal(llvm::GlobalVariable& global, std::string_view name,
                              const sema::TypeStore& types, sema::TypeId type, support::Span span,
                              const AliasName& alias) {
  if (unit_ == nullptr || name.empty()) {
    return;
  }
  llvm::DIType* const debugTypeNode = aliasType(types, type, alias);
  if (debugTypeNode == nullptr) {
    return;
  }
  const support::LineCol position = span.valid() && span.file == source_.id
                                        ? positionOf(source_, span.begin)
                                        : support::LineCol{1, 1};

  // `LinkageName` is the symbol, which for this language is the name the
  // declaration wrote -- there is no mangling to undo, and a debugger that has to
  // guess between them is a debugger that prints the wrong object.
  //
  // `IsLocalToUnit` mirrors the linkage the object actually has, so `static`
  // reaches the debugger as what it is: a symbol this unit owns. `IsDefinition`
  // is true because a file-scope binding in this language is always defined here
  // -- `extern` on a binding is refused by the parser, and the value is written
  // by this compiler into this module (`globals.md`, decision 3).
  llvm::DIGlobalVariableExpression* expression = builder_.createGlobalVariableExpression(
      unit_, std::string(name), std::string(name), file_, position.line, debugTypeNode,
      /*isLocalToUnit=*/global.hasInternalLinkage(),
      /*isDefinition=*/true);
  global.addDebugInfo(expression);
}

void DebugInfo::finalize() {
  if (finalized_) {
    return;
  }
  finalized_ = true;
  builder_.finalize();
}

} // namespace minc::ir
