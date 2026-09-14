// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The lowering, internal to `src/ir`.
//
// One object per unit, one pass, everything about it in one place -- the same
// shape `sema::Checker` has, and for the same reason: an expression cannot be
// lowered without the function it is inside (a `return` coerces to that
// function's type, `break` needs that loop's blocks), and splitting the state
// would mean passing the same dozen things down every call. It is split across
// `types.cc` (the type mapper and the conversion materialiser), `declarations.cc`
// (the function signatures), `function.cc` (one function), `stmt.cc`
// (statements), `expr.cc` (expressions) and `runtime.cc` (the operations the
// hardware does not define), but it is *one* class.
//
// The rule the whole file is arranged around: **the lowering decides nothing.**
// It reads `sema`'s answers -- `typeOf`, `infoOf`, the coercion record, the
// access record -- and materialises them. A question it cannot answer from those
// is a refusal, and the two refusals are named apart: `ir-unsupported-*` is a
// construct this stage has not built yet, `ir-internal`/`ir-missing-obligation`
// are bugs in this compiler. Opposite fixes, so never the same message.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include "ast/ast.h"
#include "ir/ir.h"
#include "lex/token_kind.h"
#include "parse/syntax_kind.h"
#include "resolve/map.h"
#include "sema/type_store.h"
#include "sema/typed_ast.h"
#include "support/intern/interner.h"
#include "support/span/file_id.h"
#include "tokens.h"
#include "values.h"

namespace minc::ir {

// A `DefId` packed into the one key every map here is keyed on.
//
// The *declaration* and not the name, because two bindings may share a spelling
// and a `PathExpr` that resolves to one of them must not find the other's
// storage -- and the file half is part of it, because a unit spans several files
// whose offsets restart.
[[nodiscard]] constexpr std::uint64_t defKey(resolve::DefId def) {
  return (static_cast<std::uint64_t>(def.file) << 32U) | def.index;
}
[[nodiscard]] constexpr std::uint64_t offsetKey(support::FileId file, std::uint32_t begin) {
  return (static_cast<std::uint64_t>(file) << 32U) | begin;
}

class DebugInfo;

class Lowering {
public:
  Lowering(const ast::LoweredFile& file, const resolve::DefMap& defs, const sema::TypedFile& typed,
           const sema::TypeStore& types, const support::Interner& symbols,
           const LoweringOptions& options);
  ~Lowering();
  Lowering(const Lowering&) = delete;
  Lowering& operator=(const Lowering&) = delete;
  Lowering(Lowering&&) = delete;
  Lowering& operator=(Lowering&&) = delete;

  // False when a diagnostic was produced. The module is then *not* handed out:
  // "no module" is part of the contract, not an implementation detail, and it is
  // what stops a half-built module from reaching a linker.
  bool run();

  // The finished module. Valid only after `run()` returned true.
  [[nodiscard]] Module takeModule();

  [[nodiscard]] std::vector<IRDiagnostic> takeDiagnostics() {
    return std::move(diagnostics_);
  }

private:
  // --- the tree, read the way the checker reads it -----------------------------
  //
  // The same accessors `sema::Checker` has, deliberately: two stages walking one
  // tree should not disagree about what a node's operands are, and the way to
  // guarantee that is to spell the rule the same way. `operandsOf` is "the
  // children that are not tokens, in source order", which is the operands for
  // every node in this grammar.
  [[nodiscard]] bool inError(ast::AstId id) const {
    return file_.inErrorRegion(id);
  }
  [[nodiscard]] ast::NodeKind kindOf(ast::AstId id) const {
    return file_.at(id).kind;
  }
  [[nodiscard]] ast::AstId childOf(ast::AstId id, ast::NodeKind kind) const {
    return file_.childOfKind(id, kind);
  }
  [[nodiscard]] std::vector<ast::AstId> operandsOf(ast::AstId id) const;
  [[nodiscard]] ast::AstId tokenOf(ast::AstId id) const;
  [[nodiscard]] std::string_view spelling(ast::AstId id) const {
    return file_.spellingOf(id);
  }
  [[nodiscard]] support::Span spanOf(ast::AstId id) const {
    return file_.at(id).origin;
  }
  // The token's kind for a leaf, as the checker's `tagOf` reads it.
  [[nodiscard]] lex::TokenKind tokenKindOf(ast::AstId id) const;

  // --- the answers `sema` published -------------------------------------------
  [[nodiscard]] sema::TypeId typeOf(ast::AstId id) const {
    return typed_.typeOf(id);
  }
  [[nodiscard]] const sema::ExprInfo& infoOf(ast::AstId id) const {
    return typed_.infoOf(id);
  }
  // The conversion `consumer` applies to `child`, or nullptr when it applies
  // none. Matched on the **operand node** rather than on an operand index: the
  // record keeps `node` precisely so the pair can be checked against the tree,
  // and matching on it means this stage never re-derives the numbering rule
  // (`BinaryExpr`'s left operand is 0, a call's first argument is 1 because the
  // callee is 0) -- a second copy of a rule is exactly what the record exists to
  // remove.
  [[nodiscard]] const sema::Coercion* coercionFor(ast::AstId consumer, ast::AstId child) const;
  // The access obligation at a place-expression that reaches memory through a
  // pointer. `nullptr` means the place denotes a binding, which needs no
  // obligation because the language already proved it is there.
  [[nodiscard]] const sema::AccessObligation* obligationFor(ast::AstId place) const;
  [[nodiscard]] bool isAccessNode(ast::AstId id) const;

  // --- debug information --------------------------------------------------------
  //
  // The `-g` half of the lowering, and a no-op without it. `locate` is called at
  // the top of every statement and expression rather than beside each instruction:
  // the builder carries the current location into every instruction it creates, so
  // one call per node is total coverage and a new instruction added below cannot
  // silently lose its line number.
  void locate(ast::AstId id);
  void locate(support::Span span);
  [[nodiscard]] bool debugEnabled() const {
    return debug_ != nullptr;
  }

  // --- diagnostics ------------------------------------------------------------
  void error(ast::AstId at, IRDiagnosticCode code, std::string message);
  void errorAt(support::Span span, IRDiagnosticCode code, std::string message);
  // Stops the walk at the first refusal. A lowering that carried on after a
  // refusal would build a module out of a tree it already said it could not
  // lower, and the module is discarded either way.
  void fatal(support::Span span, IRDiagnosticCode code, std::string message);

  // --- types ------------------------------------------------------------------
  // One mapper, total over `TypeKind` with no `default:` -- so a new kind is a
  // compile error here rather than a silent gap.
  [[nodiscard]] llvm::Type* llvmType(sema::TypeId id);
  // How a type is *stored*. `i1` is not a byte, so a `bool` object is `i8` with a
  // normalising store and a truncating load (`memory.md`, *Objects*). Everything
  // else is its own type.
  [[nodiscard]] llvm::Type* storageType(sema::TypeId id) {
    // `llvmType` once, and a null is propagated: a type this stage cannot map
    // has already been refused (`ir-unsupported-type`), and asking a second time
    // would record the same refusal twice. It also keeps null out of
    // `CreateAlloca`, where it is a crash rather than a diagnostic.
    llvm::Type* mapped = llvmType(id);
    if (mapped == nullptr) {
      return nullptr;
    }
    return mapped == boolType() ? byteType() : mapped;
  }
  [[nodiscard]] llvm::Type* llvmFunctionType(sema::TypeId id);
  [[nodiscard]] llvm::IntegerType* boolType() {
    return llvm::Type::getInt1Ty(context_);
  }
  [[nodiscard]] llvm::IntegerType* byteType() {
    return llvm::Type::getInt8Ty(context_);
  }
  [[nodiscard]] llvm::PointerType* pointerType() {
    return llvm::PointerType::get(context_, 0);
  }
  // The pointer index type: `isize`'s LLVM shape, taken from the *data layout*
  // and not from a constant, so a 32-bit triple gets an `i32` index.
  [[nodiscard]] llvm::IntegerType* indexType() {
    return llvm::IntegerType::get(context_, layout_.getPointerSizeInBits(0));
  }
  // The `sema` type of the pointer index: `isize`, which is `signedInt(pointer
  // width)` and is one of the pre-registered ids for every target this compiler
  // states. Read from the target rather than interned, because the type store is
  // shared and read-only here and `signedInt` interns.
  [[nodiscard]] sema::TypeId pointerIntType() const {
    switch (types_.target().pointerBits) {
    case 16:
      return sema::kTypeI16;
    case 32:
      return sema::kTypeI32;
    default:
      return sema::kTypeI64;
    }
  }

  // --- conversions ------------------------------------------------------------
  // Applies the conversion the record states, so the lowering is a
  // *materialiser*: `sext`/`zext`/`trunc`/`sitofp`/`uitofp`/`fptosi`/`fptoui`/
  // `fpext`/`fptrunc`, chosen from the pair and never from a rule of its own.
  [[nodiscard]] Value convert(const Value& value, sema::TypeId to);
  // Lowers an operand and applies its recorded conversion, if any. Every
  // consumer of a value goes through here, which is the single place a
  // conversion can be missed.
  [[nodiscard]] Value lowerOperand(ast::AstId consumer, ast::AstId child);
  // `i1` <-> its `i8` object representation. The two directions exist once each,
  // because a `bool` object is a byte and *only* a load and a store care.
  [[nodiscard]] Value toStorage(const Value& value);
  [[nodiscard]] Value fromStorage(const Value& value);

  // --- storage ----------------------------------------------------------------
  // The local an expression names, or nullptr when it names something that is not
  // a binding in this function.
  [[nodiscard]] llvm::AllocaInst* localOf(ast::AstId pathExpr) const;
  // `at` is the declaration the slot is for, so the frame slot carries the
  // location of the binding the reader wrote rather than of whatever instruction
  // happened to be last.
  [[nodiscard]] llvm::AllocaInst* declareLocal(resolve::DefId def, sema::TypeId type,
                                               std::string_view name, ast::AstId at);
  [[nodiscard]] std::optional<resolve::DefId> defOfPath(ast::AstId pathExpr) const;
  [[nodiscard]] std::optional<resolve::DefId> defAtName(ast::AstId nameNode) const;
  [[nodiscard]] std::optional<resolve::DefId> defOfPlace(ast::AstId expr) const;

  // --- places and accesses ----------------------------------------------------
  [[nodiscard]] Place lowerPlace(ast::AstId expr);
  // The value of a place. `placeNode` is the node the lowering is standing on,
  // which is what the access record is keyed on when the place came through a
  // pointer.
  [[nodiscard]] Value loadPlace(const Place& place, ast::AstId placeNode);
  void storePlace(const Place& place, const Value& value, ast::AstId placeNode);
  // The alignment an access of this type states. One function, so a read and a
  // write of one type cannot disagree -- and so the scan has a single rule to
  // check against.
  [[nodiscard]] std::uint64_t alignmentOf(sema::TypeId type) const;

  // --- items ------------------------------------------------------------------
  void declareFunctions();
  void defineFunction(const sema::FunctionInfo& info);
  [[nodiscard]] std::string linkageName(resolve::DefId def) const;
  // Gives every block that has no terminator one, so a construct that leaves a
  // branch target unreached still verifies. An `unreachable` is the honest
  // terminator there: nothing reaches the block, and the optimizer deletes it.
  void terminateDangling(llvm::Function* function);

  // --- statements -------------------------------------------------------------
  void lowerBlock(ast::AstId block);
  void lowerStatement(ast::AstId stmt);
  void lowerBinding(ast::AstId stmt);
  void lowerIf(ast::AstId stmt);
  void lowerWhile(ast::AstId stmt);
  void lowerFor(ast::AstId stmt);
  void lowerReturn(ast::AstId stmt);
  // Closes the current block with a branch to `next`, unless it already has a
  // terminator -- which is the ordinary case after a `return`, and the reason
  // every construct here ends with this call instead of an unconditional branch.
  void branchTo(llvm::BasicBlock* next);
  // A fresh block for code that cannot be reached, so the statements the checker
  // warned about are still *lowered* rather than dropped: a dropped statement
  // leaves whatever it names unlowered, and that omission does not show up until
  // someone adds a side effect to it.
  llvm::BasicBlock* deadBlock();

  // --- expressions ------------------------------------------------------------
  [[nodiscard]] Value lowerExpr(ast::AstId expr);
  [[nodiscard]] Value lowerLiteral(ast::AstId expr);
  [[nodiscard]] Value lowerPath(ast::AstId expr);
  [[nodiscard]] Value lowerPrefix(ast::AstId expr);
  [[nodiscard]] Value lowerPostfix(ast::AstId expr);
  [[nodiscard]] Value lowerBinary(ast::AstId expr);
  // `&&` and `||`, which short-circuit and are therefore blocks and a phi rather
  // than one `and i1`.
  [[nodiscard]] Value lowerLogical(ast::AstId expr, Tag kind, ast::AstId lhsNode,
                                   ast::AstId rhsNode);
  [[nodiscard]] Value lowerConditional(ast::AstId expr);
  [[nodiscard]] Value lowerAssign(ast::AstId expr);
  [[nodiscard]] Value lowerCall(ast::AstId expr);
  // `*p` and `p[i]` seen as expressions: a place is produced and then loaded,
  // because in this grammar those nodes are *values* everywhere except in the
  // place positions an assignment or an `&` gives them.
  [[nodiscard]] Value lowerDerefOrIndex(ast::AstId expr);
  // The instruction for a binary operator (or its compound spelling), at the
  // type `sema` decided the operation happens at. One function, so `x += y` and
  // `x + y` cannot choose two different instructions.
  [[nodiscard]] Value applyBinary(Tag op, const Value& lhs, const Value& rhs, sema::TypeId opType,
                                  ast::AstId at);
  // A pointer step: `p + n`, `p - n`, `++p`, `p--`, `p += n`. One function
  // because all six are one `getelementptr`, and the index type comes from the
  // record rather than from a width this stage picks.
  [[nodiscard]] Value pointerOffset(const Value& pointer, const Value& offset, bool negate);
  [[nodiscard]] Value pointerDifference(const Value& lhs, const Value& rhs);

  // --- literals and globals ----------------------------------------------------
  // The bytes of a string literal, and the private global holding them. Keyed by
  // the decoded text: two literals with the same bytes are *one object*, and that
  // is a contract rather than an optimisation now that `&x` makes the addresses
  // observable (`memory.md`, *Objects*).
  [[nodiscard]] llvm::Constant* stringGlobal(ast::AstId literal);
  [[nodiscard]] std::optional<llvm::APFloat> floatValue(ast::AstId literal);
  [[nodiscard]] std::optional<llvm::APInt> wideInteger(ast::AstId literal);

  // --- the runtime -------------------------------------------------------------
  //
  // The operations `sema`'s integer table defines and the hardware does not:
  // division and remainder by zero, `INT_MIN / -1`, and a shift count that is
  // negative or past the width. Each one ends in `llvm.trap`, and no call site is
  // allowed to emit the bare instruction -- the mapping in `expr.cc` is the only
  // place the opcode appears, and `invariants.cc` scans for the difference.
  [[nodiscard]] Value checkedDiv(const Value& lhs, const Value& rhs, sema::TypeId opType,
                                 bool isRemainder);
  [[nodiscard]] Value checkedShift(const Value& value, const Value& count, sema::TypeId opType,
                                   bool left);
  void trapBlock();

  // Whether a value of this type is interpreted as signed. Not `static`: the
  // answer is a property of the *store*, and a type this stage cannot look up is
  // not one it can guess about.
  [[nodiscard]] bool isSigned(sema::TypeId type) const;
  [[nodiscard]] std::uint16_t bitsOf(sema::TypeId type) const;
  [[nodiscard]] llvm::CmpInst::Predicate unsignedPredicate(Tag op) const;
  [[nodiscard]] llvm::CmpInst::Predicate signedPredicate(Tag op) const;

  // --- state ------------------------------------------------------------------
  const ast::LoweredFile& file_;
  const resolve::DefMap& defs_;
  const sema::TypedFile& typed_;
  const sema::TypeStore& types_;
  const support::Interner& symbols_;
  // The lowering's options, kept whole rather than destructured: `debugInfo` is
  // read once here to decide whether `debug_` exists, and `producer` is the string
  // `DW_AT_producer` carries.
  LoweringOptions options_;
  // Null without `-g`. Every use is guarded by `debugEnabled()`, which is what
  // makes the debug half of this class a single switch rather than a rule with
  // holes in it.
  std::unique_ptr<DebugInfo> debug_;

  // The module, opaque even here: `ModuleStorage` owns the context, and the
  // module and the layout belong to it in that order. Declared first because
  // every reference below it points into it.
  std::unique_ptr<ModuleStorage> impl_;
  llvm::LLVMContext& context_;
  llvm::Module& module_;
  const llvm::DataLayout& layout_;
  llvm::IRBuilder<> builder_;
  // Locals go in the entry block, so this builder is parked there and never
  // moves: a conditional `alloca` would grow the frame on every execution of the
  // branch that contains it.
  llvm::IRBuilder<> allocaBuilder_;

  llvm::Function* current_ = nullptr;
  // Where the frame slots go. Held rather than reached through the builder,
  // because `declareLocal` re-seats the alloca builder for every slot: a binding
  // declared after a branch has already terminated the entry block would
  // otherwise have its `alloca` appended *after* the terminator, which is not a
  // module LLVM's verifier accepts.
  llvm::BasicBlock* entryBlock_ = nullptr;
  sema::TypeId currentReturn_ = sema::kInvalidType;

  std::unordered_map<std::uint64_t, llvm::AllocaInst*> locals_;
  std::unordered_map<std::uint64_t, llvm::Function*> functions_;
  std::unordered_map<std::string, llvm::GlobalVariable*> strings_;
  // Interned names, so a diagnostic and a symbol can spell one without a scan.
  // Built once in the constructor, because a lookup per `PathExpr` is what keeps
  // a unit's cost linear rather than quadratic.
  std::unordered_map<std::uint64_t, resolve::DefId> defByName_;
  std::unordered_map<std::uint64_t, resolve::DefId> refByOffset_;

  struct Loop {
    llvm::BasicBlock* condition = nullptr;
    llvm::BasicBlock* end = nullptr;
  };
  std::vector<Loop> loops_;

  std::vector<IRDiagnostic> diagnostics_;
  // Set by the first refusal, checked by `run()`; the walk stops soon after.
  bool failed_ = false;
  std::size_t depth_ = 0;
};

} // namespace minc::ir
