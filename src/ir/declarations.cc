// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The function signatures, the file-scope objects, and the names the linker sees.
//
// Every function is declared before any body is lowered, which is the same
// decision `sema::runSignatures` makes one stage up and for the same reason: a
// body may call a function written after it, so a call must find a `Function*`
// that already exists rather than one it has to create. The pass is therefore
// *all* signatures first, then *all* bodies (`lower.cc`), and the two loops are
// the shape of that.
//
// A declaration contributes a `declare` and nothing else: `extern fn` is how the
// language writes "this symbol is defined somewhere this compiler is not looking",
// and the linker is the stage that resolves it. An `extern` declaration that is
// never called emits no symbol at all, because LLVM drops a declaration nothing
// references -- which is why declaring a function the program never uses costs
// nothing and needs no bookkeeping here.
//
// The file-scope objects are declared in the same pass and for the same reason: a
// body that reads a global has to find the `GlobalVariable` that already exists.
// Their *bytes* are the one thing here the lowering does not emit but
// **materialises**, and that is not a shortcut: a file-scope object's value is
// written before the program runs, which is exactly what a `llvm::Constant` is
// and what an instruction is not.
#include "lowering.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "llvm/IR/Attributes.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"

#include "debug.h"
#include "sema/typed_ast.h"

namespace minc::ir {

std::string Lowering::linkageName(resolve::DefId def) const {
  if (!def.valid() || def.index >= defs_.defs.size()) {
    return {};
  }
  const support::SymId name = defs_.defs[def.index].name;
  if (name == support::kInvalidSym) {
    return {};
  }
  return std::string(symbols_.lookup(name));
}

void Lowering::declareFunctions() {
  for (const sema::FunctionInfo& info : typed_.functionTable) {
    if (failed_) {
      return;
    }
    if (!info.decl.valid() || inError(info.decl)) {
      continue;
    }
    // A **generic declaration is not a function.** It is a template, and what the
    // linker sees is one symbol per instance -- declared just below, from the list
    // the checker published. Declaring one here would put a symbol in the module
    // that nothing calls and whose signature still holds `Param`s, which is the
    // one thing this stage may not emit (`generics.md`, decision 20).
    if (info.binders != 0) {
      continue;
    }
    const ast::AstId nameNode = childOf(info.decl, ast::NodeKind::Name);
    const std::optional<resolve::DefId> def = defAtName(nameNode);

    // The name, from the declaration when it resolves and from the interned
    // signature otherwise. Two sources rather than one because the fallback is
    // what makes a name *always* printable: a diagnostic and a symbol both need
    // to spell the function, and a stage that gave up on an unresolved
    // declaration would have to say "a function" in a message about a name the
    // reader wrote down.
    std::string name;
    if (def.has_value()) {
      name = linkageName(*def);
    }
    if (name.empty() && info.name != support::kInvalidSym) {
      name = std::string(symbols_.lookup(info.name));
    }
    if (name.empty()) {
      error(info.decl.valid() ? info.decl : file_.root(), IRDiagnosticCode::Internal,
            "a function reached lowering with no name");
      continue;
    }

    llvm::Type* mapped = llvmFunctionType(info.functionType);
    if (mapped == nullptr) {
      continue;
    }
    auto* type = llvm::dyn_cast<llvm::FunctionType>(mapped);
    if (type == nullptr) {
      fatal(spanOf(info.decl), IRDiagnosticCode::Internal,
            "`" + name + "` is declared with a type that is not a function type");
      continue;
    }

    // One `Function` per definition, never two. Two entries in the table can
    // name the same def -- `extern fn i32 f();` above `fn i32 f() { }` is the
    // ordinary pair -- and a second `Function::Create` would leave a stray
    // symbol behind: LLVM renames the loser to `f.1`, so the module would carry
    // a declaration of `f`, a `define` of `f.1` nobody calls, and a link error
    // for the call the program actually wrote. `sema` has already refused two
    // *disagreeing* signatures for one name, so the type chosen here is the type
    // of the one function.
    if (def.has_value() && functions_.contains(defKey(*def))) {
      continue;
    }

    // The linkage comes from the declaration, where `resolve` decided it: a
    // file-scope function is external, and `static` is the one word that says
    // otherwise (`globals.md`, decision 5). It is *read* here and not recomputed,
    // so `static fn i32 f()` and `static let x` cannot end up with two different
    // answers about what the linker sees -- one rule, recorded once.
    const bool internal = def.has_value() && def->index < defs_.defs.size() &&
                          defs_.defs[def->index].linkage == resolve::Linkage::Internal;
    llvm::Function* function = llvm::Function::Create(
        type, internal ? llvm::GlobalValue::InternalLinkage : llvm::GlobalValue::ExternalLinkage,
        name, module_);

    // A `!` return type, told to LLVM in the one spelling it understands. The
    // fact is *derived* from the type rather than declared beside it: a function
    // whose return type is `!` never gives control back, so every call to it
    // inherits that, every edge after one becomes unreachable, and a declaration
    // and a definition of the same name (one `Function`, above) cannot disagree
    // about it. `sema` has already checked that a body keeps the promise, so this
    // is not a claim the compiler is taking on faith.
    if (types_.isNever(info.returnType)) {
      function->addFnAttr(llvm::Attribute::NoReturn);
    }

    // The aggregate return's destination, told to LLVM in the one spelling it
    // understands. The shape (a leading pointer and a `void` return) is what both
    // sides agree on by construction; `sret` is what says *why* that pointer is
    // there, and it is what lets a later stage reason about the callee writing
    // into the caller's object rather than through an arbitrary pointer.
    if (byReference(info.returnType) && function->arg_size() > 0) {
      function->getArg(0)->addAttr(
          llvm::Attribute::getWithStructRetType(context_, storageType(info.returnType)));
    }

    if (def.has_value()) {
      functions_.emplace(defKey(*def), function);
    }
  }
  declareInstances();
}

void Lowering::declareInstances() {
  // One `llvm::Function` per **instance**, in the order `sema` published them, and
  // the index in that table is what a call site names: an instance is not a
  // declaration, and nothing may confuse the two.
  //
  // The signature is the instance's own -- `fn i32(i32)` for `identity<i32>` -- so
  // the type mapper, the ABI and the debug info need no case of their own: an
  // instance is a function like any other, and the only new thing about it is that
  // its *name* was decided by the pair (declaration, arguments) instead of by the
  // source (`generics.md`, § 7).
  instances_.assign(typed_.instances().size(), nullptr);
  for (std::size_t index = 0; index < typed_.instances().size(); ++index) {
    const sema::InstantiationInfo& info = typed_.instances()[index];
    if (failed_) {
      return;
    }
    if (info.function >= typed_.functionTable.size()) {
      fatal(spanOf(file_.root()), IRDiagnosticCode::Internal,
            "an instance names a declaration this unit does not have");
      return;
    }
    const sema::FunctionInfo& decl = typed_.functionTable[info.function];
    llvm::Type* mapped = llvmFunctionType(info.functionType);
    if (mapped == nullptr) {
      continue;
    }
    auto* type = llvm::dyn_cast<llvm::FunctionType>(mapped);
    if (type == nullptr) {
      fatal(spanOf(decl.decl), IRDiagnosticCode::Internal,
            "the instance `" + info.name + "` has a type that is not a function type");
      continue;
    }
    // **The instance is private to this unit**, whatever the declaration's own
    // linkage says.
    //
    // A symbol's linkage answers one question -- may another unit call *this
    // name* -- and an instance's name is decided here: `__M2_idi32` is not
    // writable from any source, and no unit can reach it from another, because
    // resolution reads one unit at a time (a second file calling `id` without
    // declaring it is `resolve-unknown-name`, and there is no import yet). So
    // external linkage bought nothing and cost the whole program: two units that
    // each instantiate the same generic at the same type emitted **one** external
    // symbol twice, and the link ended in
    //
    //     ld: unit1.o: multiple definition of `__M2_idi32`
    //
    // which is a duplicate the source cannot rename -- the compiler chose the
    // name -- and therefore the one case `static` cannot be advised for.
    //
    // The two market answers are both unavailable here, and knowing why is the
    // reason to take this one. C++ instantiates a template in every unit and lets
    // the linker fold the copies (`linkonce_odr`, COMDAT), which is sound only
    // because the one-definition rule makes the bodies *identical*: two `.mx`
    // files are free to declare different functions under one name, so folding
    // could run the other file's body -- and even for identical text the
    // `-fcheck` guards carry the site's file and line, so the bodies are not
    // byte-identical and the message could name a file that has no such line.
    // Rust escapes this because **one crate owns the generic** and every copy
    // comes from one body of MIR.
    //
    // So the copies are private, exactly as clang emits an internal-linkage
    // template's instance, and `static fn T id<T>` and `fn T id<T>` agree here --
    // not a loss: the declaration's linkage is about the *declaration's* name,
    // which no linker will ever be shown (a generic declaration has no body to
    // emit), so nothing is widened or narrowed by it. The day a unit can import a
    // declaration, the module that owns it owns its instances, and a shared
    // weakly-linked instance becomes sound *then* -- behind the export map
    // modules need anyway (`generics.md`, § 7).
    llvm::Function* function =
        llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, info.symbol, module_);
    if (types_.isNever(types_.get(info.functionType).returnType)) {
      function->addFnAttr(llvm::Attribute::NoReturn);
    }
    if (byReference(types_.get(info.functionType).returnType) && function->arg_size() > 0) {
      function->getArg(0)->addAttr(llvm::Attribute::getWithStructRetType(
          context_, storageType(types_.get(info.functionType).returnType)));
    }
    instances_[index] = function;
  }
}

// --- the file scope ------------------------------------------------------------

void Lowering::declareGlobals() {
  for (const sema::GlobalInfo& info : typed_.globalTable) {
    if (failed_) {
      return;
    }
    if (!info.decl.valid() || inError(info.decl)) {
      continue;
    }
    const std::optional<resolve::DefId> def = defAtName(childOf(info.decl, ast::NodeKind::Name));
    std::string name;
    if (def.has_value()) {
      name = linkageName(*def);
    }
    if (name.empty()) {
      error(info.decl, IRDiagnosticCode::Internal,
            "a file-scope binding reached lowering with no name");
      continue;
    }
    // One `GlobalVariable` per declaration, never two -- the same rule the
    // functions follow, and for the same reason: LLVM renames the loser to
    // `name.1`, and the module would then carry an object nobody reads next to
    // the one the program uses.
    if (def.has_value() && globals_.contains(defKey(*def))) {
      continue;
    }

    llvm::Type* objectType = storageType(info.type);
    if (objectType == nullptr) {
      // The type has no LLVM mapping and the refusal is already recorded; an
      // object built from the null would be a crash where the reader was handed a
      // diagnostic.
      continue;
    }
    llvm::Constant* initializer = globalInitializer(info);
    if (initializer == nullptr) {
      continue;
    }

    const bool internal = def.has_value() && def->index < defs_.defs.size() &&
                          defs_.defs[def->index].linkage == resolve::Linkage::Internal;

    // **`isConstant` is false for every file-scope binding, including a `const`.**
    //
    // `const` protects a *name*, not memory (`memory.md`, decision 15), and LLVM's
    // `constant` is the opposite claim: it makes any write through a pointer to
    // the object undefined behaviour. The two are not the same rule and must not
    // be spelled the same way. Today the source cannot reach such a write --
    // `&SIZE` is refused as `sema-address-of-const`, precisely because a pointer
    // to it would be a way to write it -- but a compiler may not lean on a
    // refusal it may later relax, and the day an explicit `readonly` annotation
    // (or const-correctness, `*const T`) arrives, this flag is the *one* place it
    // has to be turned on. `const x: i32 = 5;` and `let x: i32 = 5;` produce the
    // same object, which is what the ABI and the user both see.
    //
    // The consequence is real and accepted: a load of a constant global is not
    // folded at `-O0` and not `.rodata`. `globals.md` records the trade; the
    // alternative is a module that is wrong for a program the checker passed,
    // which is the one thing the invariant list forbids.
    auto* global = new llvm::GlobalVariable(module_, objectType, /*isConstant=*/false,
                                            internal ? llvm::GlobalValue::InternalLinkage
                                                     : llvm::GlobalValue::ExternalLinkage,
                                            initializer, name);
    // The alignment the type states, and not the target's default for the
    // initializer: it is the number every access through this object will compare
    // against, so an object aligned differently from its own accesses would be an
    // access the invariant scan refuses.
    global->setAlignment(llvm::Align(alignmentOf(info.type)));
    if (def.has_value()) {
      globals_.emplace(defKey(*def), global);
    }
    if (debug_ != nullptr) {
      debug_->declareGlobal(*global, name, types_, info.type, spanOf(info.decl),
                            aliasNameAt(childOf(info.decl, ast::NodeKind::Type)));
    }
  }
}

// --- the aggregate initializer ---------------------------------------------------
//
// One function per question, and the four of them are the whole of what an
// aggregate value is:
//
//   `aggregateConstant`  the array, from the recorded shape (a list or a splat)
//   `elementConstant`    one element, at the storage form of its own type
//   `constantToStorage`  the `bool`-is-a-byte difference, one level down too
//   `frameObjectFits`    the frame bound, at the two places a slot is created
//
// The recursion is the point. `[2][3]bool{[true, false, true], [false, true,
// false]}` is four levels of the same two questions -- is this an array, and is
// the element a byte or a bit -- and every one of them is answered by looking at
// the *record*, never by looking for a pattern in the tree. That is what makes
// the file-scope path and the runtime path agree about what an element is: they
// do not share a walk, they share the answer.

llvm::Constant* Lowering::aggregateConstant(const sema::GlobalInfo& info) {
  return aggregateConstant(info.elements, info.splat, info.type, spanOf(info.decl));
}

llvm::Constant* Lowering::aggregateConstant(std::span<const sema::GlobalElementValue> elements,
                                            bool splat, sema::TypeId type, support::Span at) {
  llvm::Type* shape = storageType(type);
  auto* array = shape == nullptr ? nullptr : llvm::dyn_cast<llvm::ArrayType>(shape);
  if (array == nullptr || !types_.known(type)) {
    fatal(at, IRDiagnosticCode::Internal,
          "an aggregate value was recorded for a type this stage cannot map to an array");
    return nullptr;
  }
  if (elements.empty() || (splat && elements.size() != 1)) {
    fatal(at, IRDiagnosticCode::Internal,
          "an aggregate value was recorded with " + std::to_string(elements.size()) +
              (splat ? " elements for a fill" : " elements"));
    return nullptr;
  }
  const std::uint64_t count = array->getNumElements();
  const sema::TypeId elementType = types_.elementOf(type);

  llvm::Constant* piece = elementConstant(elements.front(), elementType, at);
  if (piece == nullptr) {
    return nullptr;
  }

  if (splat) {
    // The zero fill first, and before the budget, because it is the one case
    // whose cost is **not** the count: one `ConstantAggregateZero` stands for any
    // number of zeroed elements. Putting the budget above it would refuse
    // `[1 << 40]u8{0; ...}`, which is a legal object and the reader's own decision
    // about its size.
    if (piece->isNullValue()) {
      return llvm::ConstantAggregateZero::get(array);
    }
    // A non-zero fill, which is the one place in the file-scope path where the
    // compiler's cost is decided by a number in the *type* rather than by the
    // source: LLVM's `splat` exists for vectors and not for arrays, so the
    // elements have to be written out. `kMaxFillElements` is the stated bound, and
    // the sentence names the count, the bound and the fix.
    if (count > support::kMaxFillElements) {
      // `fatal` and not `errorAt`: the walk stops, and it has to. An object with no
      // initializer is a symbol this module then reads as missing -- a `load` of it
      // in a body becomes "this place is not a binding this function owns", which is
      // a second message about one mistake and an `ir-internal` about a program that
      // is not buggy. One refusal, one sentence.
      fatal(at, IRDiagnosticCode::InitializerTooLarge,
            "this fill writes " + std::to_string(count) + " elements of `" +
                types_.spelling(elementType) +
                "`, and this compiler writes a non-zero fill "
                "out one element at a time: " +
                std::string(support::kMaxFillElementsText) +
                " is the most it will materialise. A zero fill costs nothing at any count, so "
                "`[N]" +
                types_.spelling(elementType) + "{0; N}` stays available");
      return nullptr;
    }
    return llvm::ConstantArray::get(
        array, std::vector<llvm::Constant*>(static_cast<std::size_t>(count), piece));
  }

  if (elements.size() != count) {
    // A list whose length is not the type's is one the checker refused; reaching
    // here means the record and the type disagree, which is a bug in this compiler
    // and not a statement about the program.
    fatal(at, IRDiagnosticCode::Internal,
          "an aggregate value records " + std::to_string(elements.size()) + " elements for a `" +
              types_.spelling(type) + "`");
    return nullptr;
  }
  std::vector<llvm::Constant*> pieces;
  pieces.reserve(elements.size());
  pieces.push_back(piece);
  for (std::size_t i = 1; i < elements.size(); ++i) {
    llvm::Constant* next = elementConstant(elements[i], elementType, at);
    if (next == nullptr) {
      return nullptr;
    }
    pieces.push_back(next);
  }
  return llvm::ConstantArray::get(array, pieces);
}

llvm::Constant* Lowering::elementConstant(const sema::GlobalElementValue& element,
                                          sema::TypeId type, support::Span at) {
  llvm::Type* shape = storageType(type);
  if (shape == nullptr) {
    fatal(at, IRDiagnosticCode::Internal,
          "an element of an aggregate initializer has a type this stage cannot map");
    return nullptr;
  }
  switch (element.kind) {
  case sema::GlobalValueKind::Zero:
    // No initializer at all, or an element the checker read as zero: the bytes are
    // the object's, so this is the type's zero and not a conversion.
    return llvm::Constant::getNullValue(shape);
  case sema::GlobalValueKind::Null:
    if (!shape->isPointerTy()) {
      fatal(at, IRDiagnosticCode::Internal, "`null` as an element of a non-pointer array");
      return nullptr;
    }
    return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(shape));
  case sema::GlobalValueKind::Int:
    // Built at the *element's* storage form, exactly as a scalar binding's value
    // is: a `[3]bool{true, false, true}` is three `i8`s and not three `i1`s.
    return intConstant(element.intValue, type);
  case sema::GlobalValueKind::Literal: {
    // Read from the literal's own spelling by the reader that already existed for
    // it -- a float, a `str` (whose bytes are their own private object, so the
    // element is that object's address), or an integer wider than the core.
    const Value literal = lowerLiteral(element.node);
    auto* constant = llvm::dyn_cast_or_null<llvm::Constant>(literal.v);
    if (constant == nullptr) {
      return nullptr;
    }
    if (element.negated) {
      constant = negatedConstant(constant);
    }
    // The element's own type and the type the value was built at are the same
    // type whenever the checker accepted the initializer -- a deferred literal
    // takes its type from the element, so there is nothing to convert. The
    // conversion is written anyway and is the same call the scalar path makes:
    // leaving it out would mean this stage *assumes* the two, and an assumption
    // about types is the one kind this stage refuses to carry.
    return constantToStorage(convertConstant(constant, element.type, type, at), type, at);
  }
  case sema::GlobalValueKind::Aggregate:
    // One level down, the same function: an element that is an array is an array.
    return aggregateConstant(element.elements, element.splat, type, at);
  }
  return nullptr;
}

llvm::Constant* Lowering::constantToStorage(llvm::Constant* value, sema::TypeId type,
                                            support::Span at) {
  if (value == nullptr) {
    return nullptr;
  }
  llvm::Type* shape = storageType(type);
  if (shape == nullptr) {
    return nullptr;
  }
  if (value->getType() == shape) {
    return value;
  }
  // The one representation difference in the language: a `bool` is an `i1` as a
  // value and a byte as an object (`memory.md`, *Objects*).
  if (value->getType() == llvm::Type::getInt1Ty(context_) && shape == byteType()) {
    // `getCast` and not a hand-built `zext` expression: it folds, so the result is
    // a `ConstantInt` and not a `ConstantExpr` the object writer would have to
    // evaluate.
    return llvm::ConstantExpr::getCast(llvm::Instruction::ZExt, value, shape);
  }
  // And the same difference one level down, for the same reason `toStorage` has
  // the loop: `[4]bool` is `[4 x i1]` as a value and `[4 x i8]` as an object, and
  // a store between the two types is undefined behaviour that LLVM does not
  // report. Recursive, so `[2][3]bool` is the same rule twice.
  auto* from = llvm::dyn_cast<llvm::ArrayType>(value->getType());
  auto* to = llvm::dyn_cast<llvm::ArrayType>(shape);
  if (from != nullptr && to != nullptr && from->getNumElements() == to->getNumElements()) {
    const sema::TypeId element = types_.elementOf(type);
    std::vector<llvm::Constant*> pieces;
    pieces.reserve(static_cast<std::size_t>(to->getNumElements()));
    for (std::uint64_t i = 0; i < to->getNumElements(); ++i) {
      llvm::Constant* piece = value->getAggregateElement(static_cast<unsigned>(i));
      piece = constantToStorage(piece, element, at);
      if (piece == nullptr) {
        return nullptr;
      }
      pieces.push_back(piece);
    }
    return llvm::ConstantArray::get(to, pieces);
  }
  fatal(at, IRDiagnosticCode::Internal,
        "an initializer value of type `" + types_.spelling(type) +
            "` reached the lowering in "
            "a form that is not its storage form");
  return nullptr;
}

llvm::ConstantInt* Lowering::intConstant(support::ConstInt value, sema::TypeId type) {
  llvm::Type* shape = storageType(type);
  if (shape == nullptr || !shape->isIntegerTy()) {
    return nullptr;
  }
  // The width comes from the *object*, not from the 64-bit core the value was
  // folded in: a `bool` is a byte on the outside, and an `i128` needs the value
  // widened -- sign-extended when the source is signed, zero-extended when it is
  // not, which is the same pair of answers `Conversion::Sext`/`Zext` gives.
  const llvm::APInt raw(shape->getIntegerBitWidth(), value.bits, !value.isUnsigned);
  return llvm::ConstantInt::get(context_, raw);
}

llvm::Constant* Lowering::negatedConstant(llvm::Constant* value) {
  if (auto* floating = llvm::dyn_cast<llvm::ConstantFP>(value)) {
    // A sign bit, which is exact at every width: `-1.5` is `1.5` with one bit
    // flipped, and no rounding is involved anywhere.
    llvm::APFloat negated = floating->getValueAPF();
    negated.changeSign();
    return llvm::ConstantFP::get(context_, negated);
  }
  if (auto* integer = llvm::dyn_cast<llvm::ConstantInt>(value)) {
    // Two's-complement negation, which is exact for the same reason.
    return llvm::ConstantInt::get(context_, -integer->getValue());
  }
  // A `str`: there is no `-"abc"`, and the checker refused one before it got
  // here. Returning the value unchanged keeps this function total.
  return value;
}

// The bytes of an initializer, converted to the object's type by the pair the
// **record** holds.
//
// Read, never re-derived, and the difference is the whole point. A conversion is
// a fact about the program (`ir.md`, *The coercion record*), and a lowering that
// derived one itself -- from the value's type to the object's -- would be a
// second copy of `convert.h`: the copy that turns a value the checker refused
// into a silent cast. `convertible` refuses an integer in a float's place in
// both directions, so `const half: f64 = 1;` is an error one stage up and not a
// widening here, and the record is what makes that refusal structural instead of
// something this stage has to remember.
//
// The absent record is therefore not "no conversion needed" but *no conversion
// was recorded*, which is only consistent when the value was built at the
// object's own type. A difference under an absent record is a disagreement
// between two stages -- not a licence to convert.
llvm::Constant* Lowering::convertGlobalValue(const sema::GlobalInfo& info, llvm::Constant* value,
                                             sema::TypeId valueType) {
  if (value == nullptr) {
    return nullptr;
  }
  const std::optional<sema::Coercion> coercion =
      info.init.valid() ? coercionFor(info.decl, info.init) : std::nullopt;
  const sema::TypeId from = coercion.has_value() ? coercion->from : valueType;
  const sema::TypeId to = coercion.has_value() ? coercion->to : valueType;
  if (to != info.type || (coercion.has_value() && from != valueType)) {
    fatal(spanOf(info.decl), IRDiagnosticCode::Internal,
          "the initializer of this file-scope binding is a value of type `" +
              types_.spelling(valueType) + "` and its recorded conversion is `" +
              types_.spelling(from) + "` to `" + types_.spelling(to) +
              "`, which disagrees with the object's type `" + types_.spelling(info.type) + "`");
    return nullptr;
  }
  return convertConstant(value, from, to, spanOf(info.init.valid() ? info.init : info.decl));
}

llvm::Constant* Lowering::globalInitializer(const sema::GlobalInfo& info) {
  llvm::Type* shape = storageType(info.type);
  if (shape == nullptr) {
    return nullptr;
  }
  switch (info.value) {
  case sema::GlobalValueKind::Zero:
    // No initializer, or a `let` whose value is zero: the C ABI's `.bss`, which
    // is a decision and not an omission (`memory.md`, decision 12). The bytes are
    // the object's type's zero, so there is no conversion to read.
    return llvm::Constant::getNullValue(shape);
  case sema::GlobalValueKind::Null:
    // `null` is `*void` and the object's type is some pointer, so this is a
    // pointer constant of the object's own type and not a cast to one. The two
    // are the *same* LLVM type -- the language's pointers are opaque, all of
    // them -- so the conversion the checker recorded for this pair is the
    // identity and is already applied by building the constant at the object.
    if (!shape->isPointerTy()) {
      fatal(spanOf(info.decl), IRDiagnosticCode::Internal,
            "`null` initializes an object whose type is not a pointer");
      return nullptr;
    }
    return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(shape));
  case sema::GlobalValueKind::Literal: {
    // One literal, read from its own spelling by the reader that already existed
    // for it: a float, a `str` (whose bytes are their own private object, so the
    // initializer is that object's address), or an integer wider than the core.
    const Value literal = lowerLiteral(info.node);
    auto* constant = llvm::dyn_cast_or_null<llvm::Constant>(literal.v);
    if (constant == nullptr) {
      // `lowerLiteral` records its own refusals; nothing is emitted rather than
      // an object with wrong bytes.
      return nullptr;
    }
    if (info.negated) {
      constant = negatedConstant(constant);
    }
    return convertGlobalValue(info, constant, literal.type);
  }
  case sema::GlobalValueKind::Aggregate:
    // The elements, from the *shape*: one constant per element, or one written
    // `count` times. No walk of the tree, no re-test of constness, and no
    // expansion of a zero fill in particular -- `[1 << 20]u8{0; ...}` is one
    // `zeroinitializer` whatever its count (`arrays.md` decision 15).
    return aggregateConstant(info);
  case sema::GlobalValueKind::Int: {
    // The value was folded at the *expression's* type -- the literal's, or the
    // operation type of the arithmetic -- and the object's type may be another of
    // the same class: `const wide: i64 = small;` is a `sext` of a `u8`
    // underneath. Which pair that is comes from the record.
    const sema::TypeId foldedAt = typeOf(info.init);
    llvm::ConstantInt* folded = intConstant(info.intValue, foldedAt);
    if (folded == nullptr) {
      // A folded *integer* value whose type is not an integer type is not a
      // statement about the program: it means this stage and `sema` disagree
      // about what an integer constant is. Left as an internal error rather than
      // a skipped object, because a skipped object is a symbol the reader's code
      // refers to and nothing defines.
      fatal(spanOf(info.decl), IRDiagnosticCode::Internal,
            "a file-scope binding was folded to an integer at `" + types_.spelling(foldedAt) +
                "`, which is not an integer type");
      return nullptr;
    }
    return convertGlobalValue(info, folded, foldedAt);
  }
  }
  return nullptr;
}

} // namespace minc::ir
