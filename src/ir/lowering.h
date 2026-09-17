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

#include "builtins/builtin.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include "alias_name.h"
#include "ast/ast.h"
#include "ir/ir.h"
#include "lex/token_kind.h"
#include "parse/syntax_kind.h"
#include "resolve/def_index.h"
#include "resolve/map.h"
#include "sema/type_store.h"
#include "sema/typed_ast.h"
#include "support/intern/interner.h"
#include "support/span/file_id.h"
#include "tokens.h"
#include "values.h"

namespace minc::ir {

// A `DefId` packed into the one key every map here is keyed on: `functions_`,
// `locals_`, the string table's owner, the debug info.
//
// The *declaration* and not the name, because two bindings may share a spelling
// and a `PathExpr` that resolves to one of them must not find the other's
// storage -- and the file half is part of it, because a unit spans several files
// whose offsets restart.
//
// Spelled the same way for the whole compiler: `resolve::defKey` is also what
// the offset index keys a declaration by, and two packings of one pair is how a
// lookup starts missing entries it should have found.
using resolve::defKey;

class DebugInfo;

class Lowering {
public:
  // The store is **mutable**, and that is what instantiation costs: a generic body
  // is lowered once per instance, and reading one of its types through the
  // instance's substitution builds the type the store already had -- `[4]T` with
  // `T := i32` is `[4]i32`, one intern and one `TypeId`. The alternative would be a
  // second table per instance, which is the thing `generics.md` decision 20 exists
  // to avoid.
  Lowering(const ast::LoweredFile& file, const resolve::DefMap& defs, const sema::TypedFile& typed,
           sema::TypeStore& types, const support::Interner& symbols,
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
    // An invalid id names no node, and one caller passes none on purpose: a store
    // into a slot a binding just created is not an access, so `lowerBinding` has no
    // expression to point at. An empty span is the answer that keeps a diagnostic
    // about that store reportable instead of a lookup that cannot succeed.
    return id.valid() ? file_.at(id).origin : support::Span{};
  }
  // The token's kind for a leaf, as the checker's `tagOf` reads it.
  [[nodiscard]] lex::TokenKind tokenKindOf(ast::AstId id) const;

  // --- the answers `sema` published -------------------------------------------
  //
  // **Every type read passes through `concrete`.** A generic body is checked
  // *once*, abstractly, so the types `sema` published for the nodes inside it hold
  // its `Param`s -- and this stage is where they stop being abstract: the body is
  // lowered once per instance, and the instance's substitution is applied here,
  // at the boundary, rather than by a second table (decision 20). Outside an
  // instance this is the identity, so a non-generic unit reads exactly the types
  // it always read.
  [[nodiscard]] sema::TypeId concrete(sema::TypeId type) const;
  [[nodiscard]] sema::TypeId typeOf(ast::AstId id) const {
    return concrete(typed_.typeOf(id));
  }
  // The member position of a `FieldExpr`, read from the token the source wrote:
  // `t.0` is the member at the *constant* index this answers, and the checker
  // refused every access whose position is not a written-out number inside the
  // arity -- so a `nullopt` here is internal, like the other shape guards
  // (`tuples.md`, decision 3).
  [[nodiscard]] std::optional<unsigned> fieldIndex(ast::AstId expr) const;
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
  // By value, because the two types are the instance's: the record holds the
  // *template*'s pair (`u8` -> `T`) and what the lowering needs is the pair the
  // instance asks for (`u8` -> `i32`). One copy per conversion instead of a table.
  [[nodiscard]] std::optional<sema::Coercion> coercionFor(ast::AstId consumer,
                                                          ast::AstId child) const;
  // The access obligation at a place-expression that reaches memory through a
  // pointer. `nullptr` means the place denotes a binding, which needs no
  // obligation because the language already proved it is there.
  [[nodiscard]] std::optional<sema::AccessObligation> obligationFor(ast::AstId place) const;
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
  // Runs one instruction with **no** location, and restores what was current.
  //
  // For the instruction that is not a statement in the program: the one caller is
  // the parameter spill (`function.cc`), which is the ABI's arrival rather than
  // something the reader wrote.
  //
  // It matters because of what a line table is *for*. LLVM places the line table's
  // `prologue_end` flag on the first instruction of a function that is not frame
  // setup and carries a non-zero line, and a debugger reads that flag to decide
  // where `break <function>` lands -- so a line here says "the body starts on the
  // `fn` line", and every breakpoint a reader sets by function name stops on the
  // declaration, with the arguments still in their registers and therefore printed
  // as zero. Without one the flag lands on the first statement, and the frame is
  // already built at the first stop. `clang` emits the same store with no location
  // for the same reason, and `#dbg_declare` -- which carries the parameter's own
  // span -- is what names the argument in a debugger either way.
  class NoLocation {
  public:
    explicit NoLocation(llvm::IRBuilder<>& builder) : builder_(builder) {
      saved_ = builder_.getCurrentDebugLocation();
      builder_.SetCurrentDebugLocation(llvm::DebugLoc{});
    }
    ~NoLocation() {
      builder_.SetCurrentDebugLocation(saved_);
    }
    NoLocation(const NoLocation&) = delete;
    NoLocation& operator=(const NoLocation&) = delete;
    NoLocation(NoLocation&&) = delete;
    NoLocation& operator=(NoLocation&&) = delete;

  private:
    llvm::IRBuilder<>& builder_;
    llvm::DebugLoc saved_;
  };

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
  // True for the types that cross a call as a **pointer to a copy the caller
  // makes**: an array, and a `struct` when it lands. A slice is not one of them.
  //
  // This is the one place the two aggregate kinds part company, and it is an ABI
  // decision rather than a language one. An array can be a megabyte, so it moves
  // by reference under a shape this compiler defines (a leading pointer, an
  // `sret` destination). A descriptor is two words -- a pointer and a length --
  // and it crosses as itself: `{ptr, usize}` in the signature, which is smaller
  // than a pointer to it would be and is what a debugger shows as two members
  // (`slices.md` decision 17).
  [[nodiscard]] bool byReference(sema::TypeId id) const {
    return types_.isAggregate(id) && !types_.isSlice(id);
  }
  [[nodiscard]] llvm::Type* storageType(sema::TypeId id) {
    // `llvmType` once, and a null is propagated: a type this stage cannot map
    // has already been refused (`ir-unsupported-type`), and asking a second time
    // would record the same refusal twice. It also keeps null out of
    // `CreateAlloca`, where it is a crash rather than a diagnostic.
    llvm::Type* mapped = llvmType(id);
    if (mapped == nullptr) {
      return nullptr;
    }
    llvm::Type* shape = mapped == boolType() ? byteType() : mapped;
    // **The object's size and alignment, checked against the target's own data
    // layout the first time this type enters the module.** `memory.md` states the
    // claim ("`ir` asserts that table against LLVM's own `Triple`/`DataLayout`")
    // and this is it: `sema::TypeStore` owns the numbers, every emitted
    // `align N` and every debug record comes from them, and LLVM owns the
    // authority. The check is here, at the object shape, because that is the
    // question both answers are about -- and it runs per *mapping*, so a type the
    // target refuses (`f80` on aarch64) is refused by the mapper with its own
    // sentence and never reaches this one.
    if (!layoutOf(id, shape)) {
      return nullptr;
    }
    return shape;
  }
  // True when the store's size and alignment for `id` are the ones the target's
  // data layout gives `shape`; records an internal diagnostic and answers false
  // when they are not, once per type.
  [[nodiscard]] bool layoutOf(sema::TypeId id, llvm::Type* shape);
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
  //
  // `at` is the expression that asked for it. One arm needs it: a float-to-
  // integer conversion of a *constant* the destination cannot hold is a refusal
  // about the program, and a refusal without a place in the source is a message a
  // reader cannot act on. The default is for the call sites whose value is not a
  // constant of that pair by construction.
  [[nodiscard]] Value convert(const Value& value, sema::TypeId to, support::Span at = {});
  // Lowers an operand and applies its recorded conversion, if any. Every
  // consumer of a value goes through here, which is the single place a
  // conversion can be missed.
  [[nodiscard]] Value lowerOperand(ast::AstId consumer, ast::AstId child);
  // `i1` <-> its `i8` object representation. The two directions exist once each,
  // because a `bool` object is a byte and *only* a load and a store care.
  [[nodiscard]] Value toStorage(const Value& value);
  [[nodiscard]] Value fromStorage(const Value& value);

  // --- storage ----------------------------------------------------------------
  // Where an expression's name lives: the frame slot of a local, or the
  // module-level object of a file-scope one. One function because "where does
  // this name live" has one answer, decided by *where it was declared* and by
  // nothing else -- and a second reader of that question is a second place for
  // the two answers to differ. Nullptr for a name that is neither, which the
  // caller reports.
  [[nodiscard]] llvm::Value* storageOf(ast::AstId pathExpr) const;
  // `at` is the declaration the slot is for, so the frame slot carries the
  // location of the binding the reader wrote rather than of whatever instruction
  // happened to be last.
  //
  // `parameterNumber` is `0` for a `let`, and one-based for a parameter's own
  // spill slot: the slot is where a debugger reads the argument from *after* the
  // prologue, and the number is what says the binding is an argument rather than
  // a variable (`debug.h`).
  // The name a type position was written as, when it was written as one: what a
  // binding's debug record needs to say `Arr` instead of `[8]u8` (`type_alias.md`,
  // decision 8). The *position* says it -- `sema` recorded it there and nowhere
  // else -- so this asks the node and not the type, which is the whole reason the
  // alias is transparent in the type store.
  [[nodiscard]] AliasName aliasNameAt(ast::AstId typeNode) const;
  [[nodiscard]] llvm::AllocaInst* declareLocal(resolve::DefId def, sema::TypeId type,
                                               std::string_view name, ast::AstId at,
                                               unsigned parameterNumber = 0,
                                               const AliasName& alias = {});
  // The caller's copy of a by-value aggregate argument: the temporary an
  // aggregate parameter points at (`arrays.md` decision 13). The slot is an
  // entry-block `alloca` -- the copy's lifetime is the call, and a slot in the
  // entry block is the one place a frame belongs -- while the store that fills
  // it happens where the call is, which is why the two seats are separate.
  [[nodiscard]] llvm::AllocaInst* argumentCopy(sema::TypeId type, const Value& value,
                                               ast::AstId at);
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
  // Every non-generic function, and then every instance of every generic one.
  void declareFunctions();
  // One `llvm::Function` per instance, named by its mangled symbol. Called at the
  // end of `declareFunctions`, because an instance is a function and the two
  // passes are one pass over two tables.
  void declareInstances();
  // The file-scope objects, before any function is lowered: a body that reads one
  // has to find the `GlobalVariable` that already exists, exactly as a call has
  // to find the `Function` its declaration made.
  void declareGlobals();
  void defineFunction(const sema::FunctionInfo& info);
  // The same body, under an instance's substitution (`generics.md`, § 5). The body
  // is lowered **once per instance** and not once: this is the one place where the
  // "one pass over the tree" assumption is deliberately given up, and the
  // substitution is applied at every type read (`concrete`) rather than by a
  // second table.
  void defineInstance(std::uint32_t index);
  // Both paths land here: the ABI, the frame, the parameters and the body are the
  // same work whether the function is written in the source or built for a pair
  // (declaration, arguments). `instance` is that pair, or null for a function the
  // source wrote whole -- and it is the one thing the two paths do not share,
  // because it carries the two names (`identity<i32>` for a debugger,
  // `__M8_identityi32` for the linker) and the two lists the template parameters
  // are printed from.
  void defineBody(const sema::FunctionInfo& decl, llvm::Function* function, sema::TypeId signature,
                  const sema::InstantiationInfo* instance);
  [[nodiscard]] std::string linkageName(resolve::DefId def) const;
  // Gives every block that has no terminator one, so a construct that leaves a
  // branch target unreached still verifies. An `unreachable` is the honest
  // terminator there: nothing reaches the block, and the optimizer deletes it.
  void terminateDangling(llvm::Function* function);

  // --- statements -------------------------------------------------------------
  // A block is a *scope* as well as a sequence: the two functions below are the
  // two halves of that sentence. `lowerBlock` opens a `DW_TAG_lexical_block`
  // around the statements and closes it after them, so a name declared inside is
  // out of scope outside; `lowerStatements` is the walk itself, and the function
  // body uses it directly because a body's bindings belong to the *function's*
  // scope and not to a block inside it (`debug.h`).
  void lowerBlock(ast::AstId block);
  void lowerStatements(ast::AstId block);
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
  // `x as T` and `(T)x`: the operand, converted through the pair `sema`
  // recorded at the cast node. A cast adds no concept to this stage.
  [[nodiscard]] Value lowerCast(ast::AstId expr);
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
  // --- builtins ---------------------------------------------------------------
  //
  // A builtin is not a function this compiler wrote and not a symbol it links:
  // it is an operation whose lowering is a *row* (`builtins/builtin.h`), and the
  // three functions below are the whole of this stage's knowledge about it. The
  // row is read by id and never by spelling, and `ir` is the one place a name
  // becomes an `llvm::Intrinsic::ID`.
  [[nodiscard]] const builtins::BuiltinInfo* builtinCallee(ast::AstId callee) const;
  [[nodiscard]] Value lowerBuiltinCall(ast::AstId expr);
  // The count reduced modulo the width, then the funnel shift: the language's
  // answer, because the raw intrinsic's is poison for a count that large.
  [[nodiscard]] Value lowerRotate(const builtins::BuiltinInfo& row, ast::AstId expr,
                                  std::span<const ast::AstId> args,
                                  std::span<llvm::Value*> arguments, llvm::Function* intrinsic);
  // One argument of a builtin call, with the recorded conversion applied.
  [[nodiscard]] llvm::Value* lowerBuiltinArgument(ast::AstId call, ast::AstId argument,
                                                  sema::TypeId type);
  // `*p` and `p[i]` seen as expressions: a place is produced and then loaded,
  // because in this grammar those nodes are *values* everywhere except in the
  // place positions an assignment or an `&` gives them.
  [[nodiscard]] Value lowerDerefOrIndex(ast::AstId expr);
  // `[N]T{...}` and its context-decided form `[1, 2, 3]`: the array a value is
  // written out as. A constant when every element is one, an `insertvalue` chain
  // otherwise.
  [[nodiscard]] Value lowerArrayInitializer(ast::AstId expr);
  // `a[1..2]` and its three other forms: the two-word descriptor, built from a
  // place and two values. It is **not** a place -- a descriptor is a value, and a
  // view of a temporary is the class of dangling this language does not hand out
  // (`slices.md` decisions 6 and 11).
  [[nodiscard]] Value lowerSlice(ast::AstId expr);
  // `(a, b)`: a product in **value** form -- a constant when every member is
  // one, an `insertvalue` chain from poison otherwise. There is no object form
  // and no `alloca`: a product is a value (`tuples.md`, decision 7).
  [[nodiscard]] Value lowerTupleExpr(ast::AstId expr);
  // `let (a, b) = t;`: one evaluation of the value, one slot per bound name, and
  // a member extracted into each (`tuples.md`, decision 6). `_` is skipped
  // because `resolve` never made a binding for it.
  void lowerDestructuring(ast::AstId stmt);
  // `t.0` and, later, `s.field`: the one postfix member access. A member of a
  // **place** is a place and a member of a **value** is an extract, and which
  // one this is comes from the checker's record, not from re-asking the base
  // (`tuples.md`, decision 9).
  [[nodiscard]] Value lowerField(ast::AstId expr);
  // The two operands of a view resolved into a place and a length, or a refusal.
  // Split out because the four forms and the three bases would otherwise be
  // twelve arms of one function.
  [[nodiscard]] std::optional<std::pair<llvm::Value*, llvm::Value*>>
  sliceRange(ast::AstId expr, const ast::SliceParts& parts);
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
  // The bytes of a file-scope object, from the record `sema` published. Always a
  // `llvm::Constant` and never an instruction: that is what "the compiler writes
  // these bytes" means, and it is the property the record exists to keep true.
  // Nullptr when the value cannot be built, with the refusal already recorded.
  [[nodiscard]] llvm::Constant* globalInitializer(const sema::GlobalInfo& info);
  // Applies the conversion `sema` recorded for a binding's initializer, or
  // refuses when the record and the value disagree. `valueType` is the type the
  // constant was built at.
  [[nodiscard]] llvm::Constant* convertGlobalValue(const sema::GlobalInfo& info,
                                                   llvm::Constant* value, sema::TypeId valueType);
  // A folded integer at the width and signedness of the type it was computed at.
  // A 64-bit core value widened to an `i128` has to be *sign*-extended when the
  // source type is signed and zero-extended when it is not, which is the same
  // pair of answers `convert` gives for the same widening.
  [[nodiscard]] llvm::ConstantInt* intConstant(support::ConstInt value, sema::TypeId type);
  // A literal whose value is the negation of what it spells (`-2.5`). Exact for
  // both forms and deliberately not arithmetic: a float's sign bit, and a
  // two's-complement negation for an integer wider than the 64-bit core.
  [[nodiscard]] llvm::Constant* negatedConstant(llvm::Constant* value);
  // The bytes of an aggregate initializer, from the *shape* `sema` recorded: one
  // constant per element, or one written `count` times when the record says the
  // initializer was a fill. `elements` and `splat` are the whole input, and the
  // tree is never re-walked here -- what makes an element constant is `sema`'s
  // rule, and a second copy of it is the copy that disagrees (`arrays.md` 15).
  [[nodiscard]] llvm::Constant* aggregateConstant(const sema::GlobalInfo& info);
  [[nodiscard]] llvm::Constant*
  aggregateConstant(std::span<const sema::GlobalElementValue> elements, bool splat,
                    sema::TypeId type, support::Span at);
  // One element of the shape above, at the *storage* form of `type`: a `bool` is
  // an `i1` as a value and a byte as an object, and that difference is one level
  // down in an array of `bool`.
  [[nodiscard]] llvm::Constant* elementConstant(const sema::GlobalElementValue& element,
                                                sema::TypeId type, support::Span at);
  // The storage form of a constant value: `i1` -> `i8` for a `bool`, and the same
  // rule element-wise for an array of them. `nullptr` when the two disagree, with
  // the internal error already recorded.
  [[nodiscard]] llvm::Constant* constantToStorage(llvm::Constant* value, sema::TypeId type,
                                                  support::Span at);
  // Whether an object of this type may live in a frame, and the diagnostic when it
  // may not (`kMaxStackObjectBytes`). One function for the two places a slot is
  // created -- a local binding and a by-value argument's caller-side copy -- so the
  // two cannot answer differently about the same object.
  [[nodiscard]] bool frameObjectFits(sema::TypeId type, ast::AstId at);

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
  // Float → integer, and the one conversion the language defines with a
  // *precondition* rather than a value: LLVM's `fptosi`/`fptoui` on an
  // out-of-range operand is poison, and this language has no poison, so the
  // instruction is reached only through a test of the operand that traps
  // (`casts.md`). Same shape as `checkedDiv`, and the scan knows it by shape.
  //
  // A **constant** operand never reaches the guard: it is folded where it is
  // representable and refused where it is not, so no `fptosi`/`fptoui` in the
  // module is ever a constant-folded `fcmp` away from being unguarded.
  [[nodiscard]] Value checkedFloatToInt(const Value& value, sema::TypeId to, support::Span at);
  void trapBlock();

  // --- the checked build -------------------------------------------------------
  //
  // The memory model's diagnostic half (`docs/architectures/checks.md`): every
  // access that reaches memory through a pointer is guarded, and the guards are
  // emitted **from the access obligation** -- the record `sema` wrote at the same
  // node -- so there is no path by which an unguarded access reaches the module,
  // and no guard whose rule this stage invented.
  //
  // All of it lives in `checks.cc`, and the four functions below are its whole
  // surface: `guardAccess` is called by the two functions an access can be
  // lowered through (`loadPlace`, `storePlace`), `guardBranch` is the one shape a
  // guard has, and the rest is the runtime entry the failure edge calls.
  [[nodiscard]] bool checksEnabled() const {
    return options_.checks;
  }
  // The guards for one access: null, alignment, and -- when the place carries the
  // evidence -- bounds. `obligation` is the record, and it is the argument rather
  // than a second lookup because a missing obligation is already a refusal at the
  // call site: a guard for an access nobody recorded would be a guard for a
  // program the checker never saw.
  void guardAccess(const Place& place, const sema::AccessObligation& obligation, support::Span at);
  // One guard: `bad` is the condition that means "outside the model", and the
  // failing edge prints `message` and traps. A condition that folded to a
  // constant is handled by what it folded to and not by pretending it is a test
  // (`checks.cc`).
  void guardBranch(llvm::Value* bad, const std::string& message);
  // The message for one rule at one span: "`mincc: trap: <rule> at <file>:<line>:<col>`".
  // Built at compile time, because everything in it is known then -- which is why
  // a fired guard needs no formatting machinery at run time (`checks.md`).
  [[nodiscard]] std::string checkMessage(std::string_view rule, support::Span span) const;
  // The private constant holding `text`, deduplicated by its own bytes: two
  // guards on one line of one file are one object.
  [[nodiscard]] llvm::GlobalVariable* checkMessageGlobal(const std::string& text);
  // The one runtime entry the failure edges call, defined in the module on first
  // use. `internal`, so the checked build adds no symbol to the program's
  // namespace and no library to its link line.
  void ensureCheckRuntime();
  // The failure edge itself: print `message` and trap. Shared by the two ways a
  // guard can end -- the conditional's failing edge, and a condition that folded
  // to "always bad" -- so a failed check is one call in one shape.
  void emitCheckFail(llvm::GlobalVariable* message, std::uint64_t length);

  // Which conversion a pair of types needs. Named rather than reduced to a cast
  // opcode because *two* appliers read it: an instruction, for a runtime value,
  // and a folded constant, for a file-scope initializer -- and an opcode would
  // force the second one to grow a copy of the rule that decides it.
  enum class Conversion : std::uint8_t {
    // The same value with a new type: a pointer pair, or one width.
    Identity,
    Sext,
    Zext,
    Trunc,
    SIToFP,
    UIToFP,
    FPToSI,
    FPToUI,
    FPExt,
    FPTrunc,
    // `x != 0`, which is how any integer becomes a `bool`. Not a `CastOps`: the
    // `i1` a `bool` is has no cast instruction from a wider integer.
    ToBool,
    // The two named joins of `memory.md`. Each is one instruction, and each is
    // reachable only from a cast -- no implicit rule reaches either.
    PtrToInt,
    IntToPtr,
    // A `!` operand: no instruction, and the answer is a poison of the type the
    // consumer asked for (`never.md`).
    Poison,
    // A pair the language does not permit. Only reachable from a bug in this
    // compiler, since the checker refuses the program first.
    Invalid,
  };
  [[nodiscard]] static Conversion conversionFor(const sema::TypeStore& types, sema::TypeId from,
                                                sema::TypeId to);
  [[nodiscard]] static std::optional<llvm::Instruction::CastOps>
  castOpcodeOf(Conversion conversion);
  // The signedness and width of a type, as the two questions the conversion rule
  // asks. Static and taking the store, so the rule above shares one answer with
  // the members below rather than restating it.
  [[nodiscard]] static bool signednessOf(const sema::TypeStore& types, sema::TypeId type);
  [[nodiscard]] static std::uint16_t bitWidthOf(const sema::TypeStore& types, sema::TypeId type);
  // The same conversion, applied to a `llvm::Constant` instead of to an
  // instruction: LLVM's own folder, so the constant and the runtime path round
  // the same way.
  [[nodiscard]] llvm::Constant* convertConstant(llvm::Constant* value, sema::TypeId from,
                                                sema::TypeId to, support::Span at = {});
  // Is a *constant* float inside the integer type's range, by the rule the
  // run-time guard uses: the value itself against the type's bounds, and not its
  // truncation. A constant outside them is refused here rather than folded to
  // LLVM's poison (`casts.md`, *Float → integer*).
  [[nodiscard]] bool floatFitsInteger(const llvm::APFloat& value, sema::TypeId to) const;
  // One end of the range a float has to be inside to become an integer: `-2^n`
  // when `negative`, `2^n` otherwise, and **zero** for the one end that is not a
  // power of two at all (an unsigned destination's floor).
  //
  // It is an exponent and not an `APFloat`, because the two questions asked of a
  // bound are "does this format hold it exactly" and "what is it in this format",
  // and exponent arithmetic answers both exactly: a power of two has one
  // significant bit, so a format holds it exactly whenever its largest exponent
  // reaches it. Asking either question by *converting* a number would introduce a
  // rounding, and a rounded bound is a bound that admits a value LLVM still calls
  // poison -- a guard that does not guard.
  struct FloatBound {
    int exponent = 0;
    bool negative = false;
    bool isZero = false;
  };
  // The half-open range the destination holds: `[ low, high )`.
  struct FloatRange {
    FloatBound low;
    FloatBound high;
  };
  // One function for the two readers, because the run-time test and the constant
  // check may not disagree about which values the conversion is defined for.
  [[nodiscard]] FloatRange floatRangeOf(sema::TypeId to) const;
  // Whether a float format holds a bound exactly. An exponent comparison and not a
  // conversion, and that is not only about rounding: `APFloat::convert` takes a
  // `bool *` that LLVM 22 dereferences unconditionally, so "convert and see
  // whether anything was lost" is a segfault when the answer is not wanted.
  [[nodiscard]] static bool holdsBound(const FloatBound& bound, const llvm::fltSemantics& in) {
    return bound.isZero || bound.exponent <= llvm::APFloatBase::semanticsMaxExponent(in);
  }
  // The bound as the constant it is **in a given format**, exact wherever
  // `holdsBound` said yes.
  [[nodiscard]] static llvm::APFloat boundIn(const llvm::fltSemantics& in, const FloatBound& bound);
  // The one refusal for a float-to-integer cast of a value the destination cannot
  // hold, and it is one function because it is one fact: the compiler sees the
  // value, and the conversion is defined as a trap on a value it cannot hold, so
  // the program could only ever trap. A file-scope object and a local `let` are
  // the same mistake with two places to be noticed, and two sentences about it
  // would be two chances for one of them to say something untrue.
  void refuseCastOutOfRange(sema::TypeId from, sema::TypeId to, support::Span at);
  // The constant a float becomes, by `APFloat`'s own conversion -- the exact one,
  // reached only for a value already known to be in range, so the result is the
  // truncation toward zero the instruction would have performed.
  [[nodiscard]] llvm::Constant* foldedInteger(const llvm::APFloat& value, sema::TypeId to);

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
  sema::TypeStore& types_;
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
  // The destination an aggregate-returning function writes its result into: the
  // first argument, and `nullptr` for every function whose return is not an
  // aggregate (`arrays.md` decision 13).
  llvm::Value* sretPointer_ = nullptr;

  // The number of leading LLVM arguments that are not parameters -- one when the
  // return type is an aggregate, zero otherwise. One place, so a parameter's
  // index and the arity check cannot disagree.
  [[nodiscard]] std::size_t sretOffset() const {
    return sretPointer_ == nullptr ? 0U : 1U;
  }

  // A binding's storage, by the def that declared it. A `Value*` and not an
  // `AllocaInst*` because a **parameter of aggregate type** is not a slot: it
  // arrives as a pointer to the caller's copy and that pointer *is* its storage
  // (`arrays.md` decision 13). Every other binding is an `alloca`, and this is
  // still the one map that answers "where does this name live".
  std::unordered_map<std::uint64_t, llvm::Value*> locals_;
  // The file-scope objects, by the def that declared them. Not cleared per
  // function: a global belongs to the unit, and one lives all the way through it.
  std::unordered_map<std::uint64_t, llvm::GlobalVariable*> globals_;
  std::unordered_map<std::uint64_t, llvm::Function*> functions_;

  // --- the instance being lowered -------------------------------------------------
  //
  // `kNoInstance` for a body that is not one, which is every non-generic function
  // and is what makes the substitution below the identity there. Inside an
  // instance the three fields are its whole context: which declaration's `Param`s
  // to replace (`instanceOwner_`), what to replace them with (`instanceArgs_`),
  // and which instance this is (`currentInstance_`, for the call table).
  std::uint32_t currentInstance_ = sema::kNoInstance;
  std::uint32_t instanceOwner_ = 0;
  std::vector<sema::TypeId> instanceArgs_;
  // One `llvm::Function` per instance, by the index `sema` published. A separate
  // map from `functions_` on purpose: an instance is not a declaration, and the
  // lookup a call makes is decided by *which* of the two it is.
  std::vector<llvm::Function*> instances_;
  std::unordered_map<std::string, llvm::GlobalVariable*> strings_;
  // The checked build's state: the runtime entry, once, and the message objects
  // by their text. Both are per module and both are created on demand, so a unit
  // with no access through a pointer carries neither.
  llvm::Function* checkFail_ = nullptr;
  std::unordered_map<std::string, llvm::GlobalVariable*> messages_;
  // Which types have had their layout checked, by `TypeId::index`. A byte per
  // type and not a set: the question is asked once per mapping of a type, and the
  // answer is "already known good" for every call after the first.
  std::vector<std::uint8_t> layoutChecked_;
  // Interned names, so a diagnostic and a symbol can spell one without a scan.
  // Built once in the constructor, because a lookup per `PathExpr` is what keeps
  // a unit's cost linear rather than quadratic -- and built by the *checker's*
  // own class (`resolve/def_index.h`), because this stage and `sema` ask one
  // question of one map and must not answer it two ways.
  resolve::DefIndex index_;

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
