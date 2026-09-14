// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The type mapper and the conversion materialiser.
//
// One mapper, `llvmType`, total over `TypeKind` with no `default:`. Under this
// project's warnings (CI adds `-Werror`) that means a new kind is a *compile
// error* here rather than a silent gap -- which is the mechanism, not a comment,
// and the reason `Pointer` landing was a body rather than a `case`.
//
// The second half of the file is what makes this stage a materialiser rather than
// a second typing pass. `convert` takes a pair of types the record already
// decided and picks the instruction; it never asks *whether* a conversion
// applies, because that question was answered one stage up and the answer is in
// `TypedFile::coercionAt`.
#include "lowering.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/Support/Error.h"

#include "sema/type.h"
#include "sema/type_store.h"
#include "support/consteval/literal.h"

namespace minc::ir {

bool Lowering::isSigned(sema::TypeId type) const {
  if (!types_.known(type)) {
    return false;
  }
  const sema::Type& shape = types_.get(type);
  // `char` is unsigned by decision (README, *Types*), so it is not signed here
  // even though it is an integer type. `bool` has no signedness and never
  // reaches an arithmetic instruction.
  return shape.kind == sema::TypeKind::Int && shape.isSigned;
}

std::uint16_t Lowering::bitsOf(sema::TypeId type) const {
  if (!types_.known(type)) {
    return 0;
  }
  const sema::Type& shape = types_.get(type);
  switch (shape.kind) {
  case sema::TypeKind::Bool:
    return 1;
  case sema::TypeKind::Char:
    return 8;
  case sema::TypeKind::Int:
  case sema::TypeKind::Float:
    return shape.bits;
  default:
    return 0;
  }
}

llvm::Type* Lowering::llvmType(sema::TypeId id) {
  if (!types_.known(id)) {
    fatal(support::Span{}, IRDiagnosticCode::Internal,
          "a type this stage was asked to map is not in the store");
    return nullptr;
  }
  switch (types_.get(id).kind) {
  case sema::TypeKind::Void:
  case sema::TypeKind::Never:
    // One branch because it is one answer: a function returning `!` returns
    // nothing, and the promise that it also never *comes back* is a function
    // attribute rather than a return type (`declarations.cc`). No object can have
    // the bottom type -- `sema` refuses every position that would build one,
    // which is what keeps "an alloca of `!`" from existing to be asked about
    // here, and why mapping it to `void` cannot collide with a storage width.
    return llvm::Type::getVoidTy(context_);
  case sema::TypeKind::Bool:
    // `i1` and not `i8`: a comparison produces exactly this, so no conversion is
    // inserted to make a condition work. The *object* representation is `i8` and
    // lives in `storageType`.
    return boolType();
  case sema::TypeKind::Char:
    return byteType();
  case sema::TypeKind::Int: {
    const std::uint16_t bits = types_.get(id).bits;
    if (bits == 0 || bits > 128) {
      fatal(support::Span{}, IRDiagnosticCode::Internal,
            "an integer type of " + std::to_string(bits) + " bits reached the mapper");
      return nullptr;
    }
    return llvm::Type::getIntNTy(context_, bits);
  }
  case sema::TypeKind::Float: {
    switch (types_.get(id).bits) {
    case 32:
      return llvm::Type::getFloatTy(context_);
    case 64:
      return llvm::Type::getDoubleTy(context_);
    case 80:
      // `x86_fp80` is the x87 type, and it is a real type only where the ABI has
      // one. The same rule `ir.md` states for `f80`: supported where the triple
      // says it is, refused where it says it is not -- because on an AArch64 the
      // data layout has no 80-bit float, so every size and alignment for it
      // would be a guess.
      if (types_.target().longDoubleBits == 80) {
        return llvm::Type::getX86_FP80Ty(context_);
      }
      fatal(support::Span{}, IRDiagnosticCode::UnsupportedType,
            "`f80` is the x87 80-bit format, which `" + types_.target().name() +
                "` has no ABI for; use `f64` or `f128`-spelled `long double`");
      return nullptr;
    case 128:
      return llvm::Type::getFP128Ty(context_);
    default:
      fatal(support::Span{}, IRDiagnosticCode::Internal,
            "a float type of " + std::to_string(types_.get(id).bits) + " bits reached the mapper");
      return nullptr;
    }
  }
  // One opaque pointer, and deliberately not the pointee: `*i32`, `*f64`,
  // `*void` and `str` (a `*u8` in the language's own vocabulary) are the same
  // `llvm::Type`. The pointee survives as the element type of a `getelementptr`,
  // as the index type, and as the accessed type of an access -- all three read
  // from the tree or from the record, never from here.
  case sema::TypeKind::Str:
  case sema::TypeKind::Pointer:
    return pointerType();
  case sema::TypeKind::Function:
    return llvmFunctionType(id);
  case sema::TypeKind::IntLiteral:
  case sema::TypeKind::FloatLiteral:
    fatal(support::Span{}, IRDiagnosticCode::Internal,
          "a deferred literal type reached the mapper; a deferred type has no width, so it has no "
          "LLVM type at all");
    return nullptr;
  case sema::TypeKind::Error:
    fatal(support::Span{}, IRDiagnosticCode::Internal,
          "the poison type reached the mapper; the unit was not checked");
    return nullptr;
  case sema::TypeKind::Array:
    // The kind is reserved and the syntax that builds one does not exist. The
    // refusal is by name, which is what makes the day it lands a *body* here.
    fatal(support::Span{}, IRDiagnosticCode::UnsupportedType,
          "arrays are not lowered yet; the language does not have the syntax for one");
    return nullptr;
  }
  return nullptr;
}

llvm::Type* Lowering::llvmFunctionType(sema::TypeId id) {
  std::vector<llvm::Type*> params;
  const std::span<const sema::TypeId> declared = types_.paramsOf(id);
  params.reserve(declared.size());
  for (const sema::TypeId param : declared) {
    llvm::Type* mapped = llvmType(param);
    if (mapped == nullptr || mapped->isVoidTy()) {
      fatal(support::Span{}, IRDiagnosticCode::Internal,
            "a function has a parameter with no LLVM type");
      return nullptr;
    }
    params.push_back(mapped);
  }
  llvm::Type* result = llvmType(types_.get(id).returnType);
  if (result == nullptr) {
    return nullptr;
  }
  // `isVarArg` comes from the type and not from a second flag beside it: `sema`
  // already decided that `f(i32)` and `f(i32, ...)` are two types, and this is
  // the one place that decision reaches LLVM. A variadic *call* states the same
  // `FunctionType` at the call site (`expr.cc`), which is what tells the backend
  // the extra arguments are un-specified -- and, on x86-64, what makes it set the
  // vector-register count in `%al` from the argument types it is handed.
  return llvm::FunctionType::get(result, params, types_.isVariadic(id));
}

std::uint64_t Lowering::alignmentOf(sema::TypeId type) const {
  const std::size_t align = types_.alignOf(type);
  // `Align` requires a power of two and refuses zero. Nothing the language
  // accepts has a zero alignment -- `void` and the poison do not have objects --
  // so the fallback is for the *unreachable* case and is the weakest alignment
  // rather than a plausible-looking 8.
  return align == 0 ? 1 : static_cast<std::uint64_t>(align);
}

// --- conversions ---------------------------------------------------------------

Value Lowering::convert(const Value& value, sema::TypeId to) {
  const sema::TypeId from = value.type;
  if (from == to) {
    return value;
  }
  if (value.v == nullptr) {
    // A refused sub-expression. The diagnostic is already recorded; propagating
    // `nullptr` keeps one mistake to one message.
    return value;
  }
  llvm::Type* destination = llvmType(to);
  if (destination == nullptr) {
    return value;
  }

  // Pointers convert to pointers -- only through `*void`, by the checker's rule
  // -- and LLVM has one pointer type, so this is the *same* value with a new
  // label. No instruction: an opaque pointer conversion is an identity, and
  // emitting a `bitcast` here would be a no-op the optimiser deletes.
  if (types_.isPointer(from) && types_.isPointer(to)) {
    return Value{value.v, to};
  }

  const bool fromInteger = types_.isInteger(from);
  const bool toInteger = types_.isInteger(to);
  if (fromInteger && toInteger) {
    const std::uint16_t fromBits = bitsOf(from);
    const std::uint16_t toBits = bitsOf(to);
    if (toBits > fromBits) {
      // Sign-extension for a signed source, zero-extension for an unsigned one --
      // and the *source's* signedness, which is why LLVM's signless types cannot
      // answer this question and `sema`'s `Type` must.
      return Value{
          isSigned(from)
              ? static_cast<llvm::Value*>(builder_.CreateSExt(value.v, destination, "sext"))
              : static_cast<llvm::Value*>(builder_.CreateZExt(value.v, destination, "zext")),
          to};
    }
    if (toBits < fromBits) {
      return Value{builder_.CreateTrunc(value.v, destination, "trunc"), to};
    }
    // The same width: `i32` to `u32` is the same bits and no instruction at all.
    return Value{value.v, to};
  }

  if (fromInteger && types_.isFloat(to)) {
    return Value{
        isSigned(from)
            ? static_cast<llvm::Value*>(builder_.CreateSIToFP(value.v, destination, "sitofp"))
            : static_cast<llvm::Value*>(builder_.CreateUIToFP(value.v, destination, "uitofp")),
        to};
  }
  if (types_.isFloat(from) && toInteger) {
    // The *destination's* signedness decides this one: the bits are the same and
    // what changes is how they are read.
    return Value{
        isSigned(to)
            ? static_cast<llvm::Value*>(builder_.CreateFPToSI(value.v, destination, "fptosi"))
            : static_cast<llvm::Value*>(builder_.CreateFPToUI(value.v, destination, "fptoui")),
        to};
  }
  if (types_.isFloat(from) && types_.isFloat(to)) {
    if (bitsOf(to) > bitsOf(from)) {
      return Value{builder_.CreateFPExt(value.v, destination, "fpext"), to};
    }
    return Value{builder_.CreateFPTrunc(value.v, destination, "fptrunc"), to};
  }

  // A pair the language does not permit, and a pair the language does not permit
  // cannot reach here: the checker reported it and the unit has errors, so the
  // lowering was never called. It is an internal error rather than a refusal
  // about the program, because the only way to see it is a bug in this compiler.
  fatal(support::Span{}, IRDiagnosticCode::Internal,
        "a conversion from `" + types_.spelling(from) + "` to `" + types_.spelling(to) +
            "` reached the lowering; the language does not permit one");
  return value;
}

Value Lowering::lowerOperand(ast::AstId consumer, ast::AstId child) {
  Value value = lowerExpr(child);

  // A child whose type is `!` produces no value, and *every* consumer that
  // accepted one accepted it because of that: the call it ends in does not come
  // back, so nothing downstream can read what is not there. What the consumer
  // needs anyway -- a phi incoming, a call argument, a stored value -- is a
  // poison of the type the consumer asked for, which is exactly "a value of that
  // type that this program can never reach". The coercion record is where the
  // asked-for type comes from, so this is one lookup and no second place has to
  // know the special case.
  //
  // Asked about the *type* rather than about the value being null, because the
  // call that never returns still returns an `llvm::CallInst` -- it is a real
  // instruction with the `void` type, and handing that to a `phi` is the type
  // error this exists to prevent.
  if (types_.isNever(typeOf(child))) {
    const sema::Coercion* coercion = coercionFor(consumer, child);
    if (coercion != nullptr) {
      if (llvm::Type* shape = llvmType(coercion->to); shape != nullptr && !shape->isVoidTy()) {
        return Value{llvm::PoisonValue::get(shape), coercion->to};
      }
    }
    return value;
  }

  if (value.v == nullptr) {
    return value;
  }
  if (const sema::Coercion* coercion = coercionFor(consumer, child)) {
    return convert(value, coercion->to);
  }
  return value;
}

// --- literals ------------------------------------------------------------------

std::optional<llvm::APFloat> Lowering::floatValue(ast::AstId literal) {
  const ast::AstId token = tokenOf(literal);
  if (!token.valid()) {
    return std::nullopt;
  }
  const sema::TypeId type = typeOf(literal);
  llvm::Type* shape = llvmType(type);
  if (shape == nullptr) {
    return std::nullopt;
  }
  const llvm::fltSemantics* semantics = nullptr;
  switch (shape->getTypeID()) {
  case llvm::Type::FloatTyID:
    semantics = &llvm::APFloat::IEEEsingle();
    break;
  case llvm::Type::DoubleTyID:
    semantics = &llvm::APFloat::IEEEdouble();
    break;
  case llvm::Type::X86_FP80TyID:
    semantics = &llvm::APFloat::x87DoubleExtended();
    break;
  case llvm::Type::FP128TyID:
    semantics = &llvm::APFloat::IEEEquad();
    break;
  default:
    return std::nullopt;
  }
  // `APFloat`'s reader and not a hand-rolled one: the rounding this stage cannot
  // verify is exactly why `sema` refuses to fold a float literal (`sema.md`,
  // *Constant folding*), and the answer here has to be the correctly rounded one.
  llvm::APFloat value(*semantics);
  llvm::Expected<llvm::APFloat::opStatus> parsedResult =
      value.convertFromString(spelling(token), llvm::APFloat::rmNearestTiesToEven);
  if (!parsedResult) {
    // The lexer already validated the *shape* of a float literal, so a string
    // this reader cannot parse is a disagreement between two readers.
    fatal(spanOf(token), IRDiagnosticCode::Internal,
          "the float literal `" + std::string(spelling(token)) + "` could not be read");
    return std::nullopt;
  }
  if ((*parsedResult & llvm::APFloat::opInvalidOp) != llvm::APFloat::opOK) {
    fatal(spanOf(token), IRDiagnosticCode::Internal,
          "the float literal `" + std::string(spelling(token)) +
              "` names a value this target's format cannot represent");
    return std::nullopt;
  }
  // `opOverflow` is not a failure: `1e400` as an `f64` is a correctly rounded
  // infinity, and the language says the literal has that value.
  return value;
}

llvm::Constant* Lowering::stringGlobal(ast::AstId literal) {
  const ast::AstId token = tokenOf(literal);
  if (!token.valid()) {
    return nullptr;
  }
  const std::string text(spelling(token));
  // Two literals with one spelling are *one object*, and that is a contract
  // rather than a deduplication pass: `&x` makes the address of a string
  // observable, and two globals for one literal would make two equal pointers
  // compare unequal (`memory.md`, *Objects*).
  const auto found = strings_.find(text);
  if (found != strings_.end()) {
    return found->second;
  }
  const support::StringLiteral parsed = support::parseStringLiteral(text);
  if (!parsed.ok) {
    // The lexer validated the escapes it could; a body this reader still cannot
    // read is a disagreement between two readers, not a statement about the
    // program.
    fatal(spanOf(token), IRDiagnosticCode::Internal, parsed.message);
    return nullptr;
  }
  const std::string bytes(parsed.bytes.begin(), parsed.bytes.end());
  // `AddNull` is the language's promise: a `str` is NUL-terminated (README,
  // *Types*), and the terminator belongs to the object the lowering builds
  // rather than to the spelling the source wrote -- which is what keeps
  // `"a\0b"` and `"a\0b\0"` the two different strings they are.
  llvm::GlobalVariable* global =
      builder_.CreateGlobalString(bytes, "str", /*AddressSpace=*/0, &module_, /*AddNull=*/true);
  // A string has no alignment requirement beyond the byte, and the tightest
  // alignment is the honest one: an over-aligned global would be a promise
  // about the object this stage cannot make.
  global->setAlignment(llvm::Align(1));
  strings_.emplace(text, global);
  return global;
}

std::optional<llvm::APInt> Lowering::wideInteger(ast::AstId literal) {
  const ast::AstId token = tokenOf(literal);
  if (!token.valid()) {
    return std::nullopt;
  }
  std::string_view text = spelling(token);
  unsigned radix = 10;
  std::size_t offset = 0;
  if (text.size() > 2 && text[0] == '0') {
    switch (text[1]) {
    case 'x':
    case 'X':
      radix = 16;
      break;
    case 'b':
    case 'B':
      radix = 2;
      break;
    case 'o':
    case 'O':
      radix = 8;
      break;
    default:
      break;
    }
    if (radix != 10) {
      offset = 2;
    }
  }
  const sema::TypeId type = typeOf(literal);
  const std::uint16_t bits = bitsOf(type);
  if (bits == 0 || !types_.isInteger(type)) {
    return std::nullopt;
  }
  // A literal wider than the 64-bit core, and the only reader for it: `sema`
  // accepted the spelling against an `i128`/`u128` context but deliberately kept
  // no value, because `ConstInt` is 64 bits wide. The digits are validated by the
  // lexer (base prefix included), so the reader cannot fail on a checked tree.
  const llvm::StringRef digits(text.data() + offset, text.size() - offset);
  return llvm::APInt(bits, digits, static_cast<std::uint8_t>(radix));
}

} // namespace minc::ir
