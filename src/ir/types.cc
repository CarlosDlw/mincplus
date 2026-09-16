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

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/IR/ConstantFold.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Error.h"

#include "sema/type.h"
#include "sema/type_store.h"
#include "support/consteval/literal.h"

namespace minc::ir {
namespace {

// The spelling of a float as `llvm::APFloat`'s reader needs to see it. Two
// translations, both *exact*, and both because the language and that reader
// disagree about which spellings exist rather than about what they mean
// (`literals.md`):
//
//   * the grouping separators go, because the reader stops at the first byte it
//     does not know and would silently read `1.000_000` as `1` -- a wrong value
//     instead of a refusal;
//   * a hexadecimal float with no exponent gets `p0`, because this language lets
//     the exponent be absent (decision 6) and `APFloat` does not. `p0` is
//     `x 2^0`: the significand and the exponent are untouched, so the rounding
//     the reader performs is the rounding of the number as written.
[[nodiscard]] std::string floatForAPFloat(std::string_view number) {
  std::string out = support::withoutSeparators(number);
  const bool hexadecimal = out.size() > 2 && out[0] == '0' && (out[1] == 'x' || out[1] == 'X');
  if (hexadecimal && out.find_first_of("pP") == std::string::npos) {
    out += "p0";
  }
  return out;
}

} // namespace

bool Lowering::signednessOf(const sema::TypeStore& types, sema::TypeId type) {
  if (!types.known(type)) {
    return false;
  }
  const sema::Type& shape = types.get(type);
  // `char` is unsigned by decision (README, *Types*), so it is not signed here
  // even though it is an integer type. `bool` has no signedness and never
  // reaches an arithmetic instruction.
  return shape.kind == sema::TypeKind::Int && shape.isSigned;
}

bool Lowering::isSigned(sema::TypeId type) const {
  return signednessOf(types_, type);
}

std::uint16_t Lowering::bitWidthOf(const sema::TypeStore& types, sema::TypeId type) {
  if (!types.known(type)) {
    return 0;
  }
  const sema::Type& shape = types.get(type);
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

std::uint16_t Lowering::bitsOf(sema::TypeId type) const {
  return bitWidthOf(types_, type);
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
      // `x86_fp80` is the x87 type, and it is a real type only on a machine that
      // has x87. The rule is the *target's* answer (`TargetInfo::hasFloat80`) and
      // not this stage's, and it is the same answer the checker read when it
      // accepted the spelling: refused here too, because on an AArch64 the data
      // layout has no 80-bit float, so every size and alignment for it would be a
      // guess.
      if (types_.target().hasFloat80()) {
        return llvm::Type::getX86_FP80Ty(context_);
      }
      // The backstop, and the same sentence the type-specifier reader refuses the
      // spelling with -- one fact, and a reader who reaches either place is owed
      // the same advice. The checker is where it is decided (no spelling for an
      // 80-bit float survives `sema` on a machine with no x87), so what this arm
      // protects is the other direction: a tree built *without* the checker must
      // not get an `x86_fp80` emitted for a target that cannot hold one, nor a
      // null `alloca` out of a mapper that answered nothing.
      fatal(support::Span{}, IRDiagnosticCode::UnsupportedType,
            "`f80` is the x87 80-bit format, which `" + types_.target().name() +
                "` has no ABI for; use `f64`, or `long double` for this target's extended "
                "format");
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
  case sema::TypeKind::Array: {
    // `[N x T]`, and `T` is the element's **storage** type and not its value
    // type: `[4]bool` is four bytes and not four bits. Built from `storageType`
    // rather than from `llvmType` because an array is only ever an *object* --
    // there is no register-shaped array -- and because a `bool` that is `i1`
    // inside an aggregate is a bitfield, which this language does not have
    // (`arrays.md` decision 12). The element access is where the `i1`/`i8`
    // normalisation happens, through `loadPlace`/`storePlace`, so there is
    // exactly one shape per array and no whole-object conversion.
    llvm::Type* element = storageType(types_.elementOf(id));
    if (element == nullptr) {
      return nullptr;
    }
    return llvm::ArrayType::get(element, types_.countOf(id));
  }
  case sema::TypeKind::Slice:
    // `{ ptr, usize }` -- the descriptor, and **structural** rather than a named
    // struct type: it is one shape, there are no user structs yet, and a named
    // type would be a symbol in every module for a type no program can write.
    //
    // The length is the *index* width -- the same integer a `getelementptr` index
    // is made of -- because the length is what an index is checked against, and
    // two widths for the same number would mean a truncation somewhere. The
    // pointer is opaque, so the element type is not in the descriptor: an access
    // carries it, and `elementOf` is where it is read (`slices.md` decision 17).
    return llvm::StructType::get(context_, {llvm::PointerType::get(context_, 0), indexType()});
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
    // A **by-value aggregate is a pointer to the caller's copy**
    // (`arrays.md` decision 13). The parameter's storage inside the callee is
    // that pointer, so `&a` and `a[i]` need no spill alloca, and the object the
    // callee writes is not the caller's variable -- which is what "a value type"
    // has to mean. It is also the shape the C ABI uses for an aggregate it
    // classifies as MEMORY, so `cinterop`'s work later is the parameter
    // *attributes* and not a second convention.
    params.push_back(byReference(param) ? pointerType() : mapped);
  }
  // A by-value aggregate **return** is the same shape the other way round: the
  // caller passes the address of the object it wants filled, the function returns
  // nothing, and the value is `sret` (`arrays.md` decision 13). The alternative --
  // an aggregate in the signature's return type -- is a megabyte of type in every
  // call site, every debug record and every function pointer for a
  // `[1 << 20]i32`, and it is the shape five languages shipped wrong code with.
  //
  // The pointer goes *first* and the return becomes `void`, so both sides agree
  // without a second rule: `declareFunctions` attributes that first parameter,
  // `defineFunction` reads it as the destination, and `lowerCall` fills it.
  const sema::TypeId back = types_.get(id).returnType;
  if (byReference(back)) {
    params.insert(params.begin(), pointerType());
    return llvm::FunctionType::get(llvm::Type::getVoidTy(context_), params, types_.isVariadic(id));
  }
  llvm::Type* result = llvmType(back);
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

bool Lowering::layoutOf(sema::TypeId id, llvm::Type* shape) {
  // Once per type: the answer is a property of (type, target, data layout), and
  // all three are fixed for the lifetime of this lowering. The table is grown to
  // the store's size because ids are indices into it and a stage may intern a
  // type the moment it is asked for one.
  if (id.index >= layoutChecked_.size()) {
    layoutChecked_.resize(static_cast<std::size_t>(id.index) + 1U, 0);
  }
  if (layoutChecked_[id.index] != 0) {
    return true;
  }
  layoutChecked_[id.index] = 1;

  const std::size_t size = types_.sizeOf(id);
  const std::size_t align = types_.alignOf(id);
  const std::uint64_t mappedSize = layout_.getTypeAllocSize(shape);
  const std::uint64_t mappedAlign = layout_.getABITypeAlign(shape).value();
  if (size == mappedSize && align == mappedAlign) {
    return true;
  }

  // A disagreement between the two tables is not a statement about the user's
  // program, it is a bug in this compiler -- the same category as a missing
  // access obligation, and reported the same way. The sentence names the type,
  // both answers and the target, because the fix is a row in one of the two
  // tables and a reader has to know which numbers disagreed.
  fatal(support::Span{}, IRDiagnosticCode::Internal,
        "this compiler states " + std::to_string(size) + " bytes and alignment " +
            std::to_string(align) + " for `" + types_.spelling(id) + "`, and the data layout of `" +
            types_.target().name() + "` says " + std::to_string(mappedSize) + " and " +
            std::to_string(mappedAlign) +
            "; one of the two tables is wrong, and every size, alignment and debug record "
            "in this module is built from this one");
  return false;
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
//
// Which conversion a pair of types needs is written **once** (`conversionFor`),
// and there are two appliers: an instruction, for a value computed at run time,
// and `llvm::ConstantFoldCastInstruction`, for the bytes of a file-scope object.
// The pair had to be shared rather than duplicated because a binding and a global
// initializer are exactly the case where the two must agree: `const wide: i64 =
// small;` is a `u8` sign- or zero-extended by one instruction when the program
// runs, and the same extension folded into an `i64` when it does not.
//
// **The pair always comes from the record, and the record never holds a mixed
// one.** An integer and a float do not convert into each other in either
// direction (`convert.h`), so `const half: f64 = 1;` is refused one stage up and
// the `SIToFP`/`FPToSI` arms below are reachable only from a coercion `sema`
// published -- which, today, no program can produce. They are written out rather
// than left as an internal error because the day the language gains a cast, the
// pair arrives through this same function and needs no new machinery here.

Lowering::Conversion Lowering::conversionFor(const sema::TypeStore& types, sema::TypeId from,
                                             sema::TypeId to) {
  if (from == to) {
    return Conversion::Identity;
  }
  // A `!` operand. The conversion is *vacuous* and not a value change: the
  // expression never produces a value, so there is nothing to convert and nothing
  // to emit, and what the consumer asked for is a poison of its own type -- the
  // one value of that type this program can never reach. `never.md` states the
  // rule; this is where it becomes an instruction sequence (none at all).
  //
  // Here rather than in `lowerCast`, because an *implicit* conversion of a `!`
  // operand reaches this same function -- `let x: i32 = die();`, a call argument,
  // a `return` -- and a second copy of the rule is the copy that disagrees.
  if (types.isNever(from)) {
    return Conversion::Poison;
  }
  // `bool` is not an integer type in this language, so its directions are written
  // out. Each is one instruction and each is defined: `i1` zero-extends into any
  // integer, any integer is `!= 0`, and `true` is `1.0` -- which is the same
  // definition as the first, one type over, and the one the matrix publishes as
  // `IntegerToFloat` (`casts.md`).
  if (types.get(from).kind == sema::TypeKind::Bool) {
    if (types.isInteger(to)) {
      return Conversion::Zext;
    }
    if (types.isFloat(to)) {
      // `bool` is unsigned, so this is `uitofp i1` and a `true` is exactly `1.0`.
      return Conversion::UIToFP;
    }
  }
  if (types.isInteger(from) && types.get(to).kind == sema::TypeKind::Bool) {
    return Conversion::ToBool;
  }
  // `str` is a pointer with a sentinel obligation, so the two names for the same
  // bytes convert with no instruction. `*void` is already an implicit conversion
  // one stage up; a cast is what reaches any other pair.
  const auto addressLike = [&types](sema::TypeId id) {
    const sema::TypeKind kind = types.get(id).kind;
    return kind == sema::TypeKind::Pointer || kind == sema::TypeKind::Str;
  };
  if (addressLike(from) && addressLike(to)) {
    return Conversion::Identity;
  }
  // The two named joins of `memory.md`: `expose` and `with_exposed_provenance`.
  // They exist only as casts -- no implicit rule reaches them -- and each is the
  // one LLVM instruction that means exactly what the model says.
  if (addressLike(from) && types.isInteger(to)) {
    return Conversion::PtrToInt;
  }
  if (types.isInteger(from) && addressLike(to)) {
    return Conversion::IntToPtr;
  }
  // Pointers convert to pointers -- only through `*void`, by the checker's rule
  // -- and LLVM has one pointer type, so this is the *same* value with a new
  // label. No instruction: an opaque pointer conversion is an identity, and
  // emitting a `bitcast` would be a no-op the optimiser deletes.
  if (types.isPointer(from) && types.isPointer(to)) {
    return Conversion::Identity;
  }

  const bool fromInteger = types.isInteger(from);
  const bool toInteger = types.isInteger(to);
  if (fromInteger && toInteger) {
    const std::uint16_t fromBits = bitWidthOf(types, from);
    const std::uint16_t toBits = bitWidthOf(types, to);
    if (toBits > fromBits) {
      // Sign-extension for a signed source, zero-extension for an unsigned one --
      // and the *source's* signedness, which is why LLVM's signless types cannot
      // answer this question and `sema`'s `Type` must.
      return signednessOf(types, from) ? Conversion::Sext : Conversion::Zext;
    }
    if (toBits < fromBits) {
      return Conversion::Trunc;
    }
    // The same width: `i32` to `u32` is the same bits and no instruction at all.
    return Conversion::Identity;
  }
  // Reachable only through a recorded coercion, and no input produces one today
  // (`convert.h`): an integer and a float are different classes of number and do
  // not convert into each other. Kept total for the day a `cast` publishes the
  // pair, at which point the instruction is `sitofp`/`uitofp` and nothing here
  // changes.
  if (fromInteger && types.isFloat(to)) {
    return signednessOf(types, from) ? Conversion::SIToFP : Conversion::UIToFP;
  }
  if (types.isFloat(from) && toInteger) {
    // The *destination's* signedness decides this one: the bits are the same and
    // what changes is how they are read.
    return signednessOf(types, to) ? Conversion::FPToSI : Conversion::FPToUI;
  }
  if (types.isFloat(from) && types.isFloat(to)) {
    return bitWidthOf(types, to) > bitWidthOf(types, from) ? Conversion::FPExt
                                                           : Conversion::FPTrunc;
  }
  return Conversion::Invalid;
}

std::optional<llvm::Instruction::CastOps> Lowering::castOpcodeOf(Conversion conversion) {
  switch (conversion) {
  case Conversion::Sext:
    return llvm::Instruction::SExt;
  case Conversion::Zext:
    return llvm::Instruction::ZExt;
  case Conversion::Trunc:
    return llvm::Instruction::Trunc;
  case Conversion::SIToFP:
    return llvm::Instruction::SIToFP;
  case Conversion::UIToFP:
    return llvm::Instruction::UIToFP;
  case Conversion::FPToSI:
    return llvm::Instruction::FPToSI;
  case Conversion::FPToUI:
    return llvm::Instruction::FPToUI;
  case Conversion::FPExt:
    return llvm::Instruction::FPExt;
  case Conversion::FPTrunc:
    return llvm::Instruction::FPTrunc;
  case Conversion::PtrToInt:
    return llvm::Instruction::PtrToInt;
  case Conversion::IntToPtr:
    return llvm::Instruction::IntToPtr;
  case Conversion::ToBool:
  case Conversion::Poison:
  case Conversion::Identity:
  case Conversion::Invalid:
    return std::nullopt;
  }
  return std::nullopt;
}

Value Lowering::convert(const Value& value, sema::TypeId to, support::Span at) {
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

  const Conversion conversion = conversionFor(types_, from, to);
  if (conversion == Conversion::Identity) {
    return Value{value.v, to};
  }
  if (conversion == Conversion::Poison) {
    // A `!` operand (`never.md`): nothing converts, because there is nothing --
    // and the instruction the call left behind is still lowered, because a call
    // inside a mistake is still a call the reader wrote.
    return Value{llvm::PoisonValue::get(destination), to};
  }
  if (conversion == Conversion::ToBool) {
    // `x != 0`, at the source's own width. A `bool` is an `i1` in the module, so
    // the comparison is the conversion.
    llvm::Type* source = value.v->getType();
    if (!source->isIntegerTy()) {
      fatal(support::Span{}, IRDiagnosticCode::Internal,
            "a conversion to `bool` reached lowering from a non-integer type");
      return value;
    }
    return Value{builder_.CreateICmpNE(value.v, llvm::ConstantInt::get(source, 0), "tobool"), to};
  }
  if (conversion == Conversion::FPToSI || conversion == Conversion::FPToUI) {
    // **The one row with a precondition.** Both arms are the same guard with a
    // different opcode, and it is emitted here -- at the single place a
    // conversion materialises -- so no future path can reach the bare
    // instruction (`casts.md`, *Float → integer*).
    return checkedFloatToInt(value, to, at);
  }
  if (conversion == Conversion::Invalid) {
    // A pair the language does not permit, and one cannot reach here: the checker
    // reported it and the unit has errors, so the lowering was never called. It
    // is an internal error rather than a refusal about the program, because the
    // only way to see it is a bug in this compiler.
    fatal(support::Span{}, IRDiagnosticCode::Internal,
          "a conversion from `" + types_.spelling(from) + "` to `" + types_.spelling(to) +
              "` reached the lowering; the language does not permit one");
    // A refusal, and not the unconverted value: `convert`'s callers all stop on an
    // empty value (`storePlace`, `lowerReturn`, an argument), so handing one back
    // is what keeps a bug in this stage from becoming a wrong-typed instruction in
    // the module -- a well-formed module is not the same thing as a correct one.
    return Value{};
  }
  const std::optional<llvm::Instruction::CastOps> opcode = castOpcodeOf(conversion);
  if (!opcode.has_value()) {
    return Value{value.v, to};
  }
  // Named from the opcode, so the instruction a reader sees in a dump is still
  // `sext`/`sitofp`/... and not an anonymous `%0`.
  return Value{
      builder_.CreateCast(*opcode, value.v, destination, llvm::Instruction::getOpcodeName(*opcode)),
      to};
}

Lowering::FloatRange Lowering::floatRangeOf(sema::TypeId to) const {
  // The destination is an integer type with a width: `llvmType` has answered for
  // it at every call site, and a `bitsOf` of zero would be a type this stage
  // cannot see -- which `llvmType` refuses as an internal error before reaching
  // any of this.
  const int bits = static_cast<int>(bitsOf(to));
  // The smallest value the destination holds and one past the largest: `-2^(n-1)`
  // and `2^(n-1)` for a signed one, `0` and `2^n` for an unsigned one. The two
  // ends are built apart rather than from one magnitude, because the sign is the
  // difference between a range that admits `-5.0` and one that refuses it.
  if (isSigned(to)) {
    return FloatRange{FloatBound{bits - 1, /*negative=*/true}, FloatBound{bits - 1, false}};
  }
  return FloatRange{FloatBound{0, false, /*isZero=*/true}, FloatBound{bits, false}};
}

llvm::APFloat Lowering::boundIn(const llvm::fltSemantics& in, const FloatBound& bound) {
  if (bound.isZero) {
    // The one end that is not a power of two, and the one that needs no
    // exactness question asked about it: a zero has no bits to round.
    return llvm::APFloat::getZero(in);
  }
  // One, scaled: exact wherever `holdsBound` said the format reaches it, and the
  // same operation for every format.
  const auto one = static_cast<llvm::APFloatBase::integerPart>(1);
  llvm::APFloat value =
      llvm::scalbn(llvm::APFloat(in, one), bound.exponent, llvm::APFloat::rmNearestTiesToEven);
  if (bound.negative) {
    value.changeSign();
  }
  return value;
}

void Lowering::refuseCastOutOfRange(sema::TypeId from, sema::TypeId to, support::Span at) {
  fatal(at, IRDiagnosticCode::CastOutOfRange,
        "this cast converts the constant `" + std::string(types_.spelling(from)) + "` to `" +
            types_.spelling(to) + "`, and no value of `" + types_.spelling(to) +
            "` holds it: a float-to-integer conversion is defined as a trap when the value "
            "is not representable, and this value is known here, so the program could only "
            "trap -- change the value or the type");
}

llvm::Constant* Lowering::foldedInteger(const llvm::APFloat& value, sema::TypeId to) {
  const auto* integerType = llvm::dyn_cast_or_null<llvm::IntegerType>(llvmType(to));
  if (integerType == nullptr) {
    return nullptr;
  }
  llvm::APSInt truncated(integerType->getBitWidth(), /*isUnsigned=*/!isSigned(to));
  // The exactness out-parameter is **given a real `bool`**, and it is not a
  // stylistic choice: LLVM 22's `convertToInteger` dereferences it unconditionally
  // (`APFloat.cpp`, `*isExact = false;` at the top), so the API's optional-looking
  // pointer is a segfault when it is null. It is a `const` member, so the operand
  // is read where it is rather than through a copy of it.
  bool isExact = false;
  if ((value.convertToInteger(truncated, llvm::APFloat::rmTowardZero, &isExact) &
       llvm::APFloat::opInvalidOp) != llvm::APFloat::opOK) {
    return nullptr;
  }
  return llvm::ConstantInt::get(context_, truncated);
}

bool Lowering::floatFitsInteger(const llvm::APFloat& value, sema::TypeId to) const {
  const std::uint16_t bits = bitsOf(to);
  if (bits == 0) {
    return false;
  }
  // The comparison is made in `f64`, whose bounds are exact for every width this
  // language has (they are powers of two, and `f64` reaches 2^1023): comparing in
  // the source's own format would have to *round* `2^128` on an `f32` source,
  // which is how a check like this becomes a check that lets poison through.
  llvm::APFloat asDouble = value;
  bool losesInfo = false;
  asDouble.convert(llvm::APFloat::IEEEdouble(), llvm::APFloat::rmNearestTiesToEven, &losesInfo);
  if (asDouble.isNaN()) {
    // NaN is neither in nor out of a range: it is not a value the destination can
    // hold, and `fptosi` on one is poison, so it is refused.
    return false;
  }
  // The *same* two bounds the run-time guard compares against, from the one place
  // that produces them, and in `f64` -- which holds every bound in the store
  // (`2^128` is far inside its range) so that a bound is never the rounded thing
  // the comparison is about. `asDouble` is where the compared value lives too.
  const FloatRange range = floatRangeOf(to);
  const llvm::fltSemantics& wide = llvm::APFloat::IEEEdouble();
  if (asDouble.compare(boundIn(wide, range.low)) == llvm::APFloat::cmpLessThan) {
    return false;
  }
  return asDouble.compare(boundIn(wide, range.high)) == llvm::APFloat::cmpLessThan;
}

llvm::Constant* Lowering::convertConstant(llvm::Constant* value, sema::TypeId from, sema::TypeId to,
                                          support::Span at) {
  if (value == nullptr || from == to) {
    return value;
  }
  const Conversion conversion = conversionFor(types_, from, to);
  if (conversion == Conversion::Identity) {
    return value;
  }
  if (conversion == Conversion::ToBool) {
    // The same `!= 0` the runtime path emits, folded: `ConstantFoldCastInstruction`
    // has no arm for it, and a `bool` is an `i1` either way.
    if (auto* integer = llvm::dyn_cast<llvm::ConstantInt>(value)) {
      return llvm::ConstantInt::get(context_, llvm::APInt(1, integer->getValue().isZero() ? 0 : 1));
    }
    return nullptr;
  }
  if (conversion == Conversion::FPToSI || conversion == Conversion::FPToUI) {
    // **No folding of an out-of-range constant.** `ConstantFoldCastInstruction`
    // returns a *poison* constant for one, and this language has no poison: the
    // value the program asked to write is not a value, so the program is refused
    // -- by the checker where it could see it, and here where it could not,
    // because the front end deliberately keeps no float value to compare with
    // (`casts.md`, *Float → integer*).
    if (auto* floating = llvm::dyn_cast<llvm::ConstantFP>(value)) {
      // **The same refusal and the same fold the run-time path uses**, so a
      // constant and a computed value cannot answer differently about the same
      // pair -- which is the whole point of the two appliers sharing
      // `conversionFor` above.
      if (!floatFitsInteger(floating->getValueAPF(), to)) {
        refuseCastOutOfRange(from, to, at);
        return nullptr;
      }
      return foldedInteger(floating->getValueAPF(), to);
    }
    // Not a float constant (already an instruction, or a value this stage cannot
    // classify): a file-scope initializer has to be a constant, so this is the
    // disagreement between two stages it looks like.
    return nullptr;
  }
  const std::optional<llvm::Instruction::CastOps> opcode = castOpcodeOf(conversion);
  if (!opcode.has_value()) {
    // `Invalid`: the checker refused the program and no module is built from it.
    // Returning the value unchanged keeps this function total and silent, because
    // a second message about it would be about bytes that are not going to be
    // compiled at all.
    return value;
  }
  llvm::Type* destination = llvmType(to);
  if (destination == nullptr) {
    return nullptr;
  }
  // Folded, not emitted: a file-scope initializer has to be a `llvm::Constant`,
  // and an instruction is not one. LLVM's own folder, so a constant and the
  // instruction the runtime path would have emitted round the same way -- a
  // hand-written folding of `sitofp` here would be a second rounding rule.
  if (llvm::Constant* folded = llvm::ConstantFoldCastInstruction(*opcode, value, destination)) {
    return folded;
  }
  return llvm::ConstantExpr::getCast(*opcode, value, destination);
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
    // The child's own span: it is the expression whose conversion this is, and
    // the one arm that reports from `convert` reports about it.
    return convert(value, coercion->to, spanOf(child));
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
  //
  // The **number** of the spelling and not the whole token: a suffix is part of
  // the literal (`1.5f32` is one token, `casts.md`), and `APFloat`'s reader is
  // handed a number. The split is `support`'s -- the same table the scanner and
  // the checker ask -- so no stage cuts a suffix off a spelling twice.
  // What the reader takes, prepared in one place: the separators removed and the
  // exponent the language may omit spelled out (`floatForAPFloat`).
  const std::string number = floatForAPFloat(support::readFloatLiteral(spelling(token)).number);
  llvm::APFloat value(*semantics);
  llvm::Expected<llvm::APFloat::opStatus> parsedResult =
      value.convertFromString(number, llvm::APFloat::rmNearestTiesToEven);
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
  // **Not a `constant` object**, and stated rather than inherited from
  // `CreateGlobalString`: LLVM builds the string global as `constant` (it is what
  // puts it in `.rodata`), and `constant` is a claim that nothing writes it -- so
  // a write through a pointer to it would be undefined behaviour rather than a
  // diagnostic. This model has no read-only memory (`memory.md`, *Objects*), and
  // its one rule about the subject is that the compiler may not infer `readonly`
  // from how a name is spelled (decision 15); a literal's object is a global like
  // any other, and `const x: i32 = 5;` and `let x: i32 = 5;` produce the same
  // object. The write is refused today by the *checker* -- a `str` is not
  // dereferenceable and does not convert to a pointer -- and leaning on a refusal
  // is exactly what decision 15 forbids, because a refusal is a rule that can be
  // relaxed while this flag would stay behind as a miscompile. It also keeps the
  // module independent of an LLVM implementation detail: every object this stage
  // emits says `global`, and the scan is what keeps it that way (`invariants.cc`).
  global->setConstant(false);
  strings_.emplace(text, global);
  return global;
}

std::optional<llvm::APInt> Lowering::wideInteger(ast::AstId literal) {
  const ast::AstId token = tokenOf(literal);
  if (!token.valid()) {
    return std::nullopt;
  }
  // The **digits** of the spelling, through the one reader that knows where they
  // stop: a suffix is part of the token (`0xFFusize` is one literal) and it is
  // not a digit. Reading them here rather than cutting the suffix off a second
  // time is what keeps this stage and the scanner agreeing about the split.
  // The grouping separators are removed rather than passed on, for the same reason
  // the float path removes them: `llvm::APInt`'s string constructor stops at the
  // first byte it does not know, so `0xFF'FF` would silently become `0xFF` -- a
  // wrong value instead of a refusal (`literals.md`, decision 10).
  const std::string number = support::withoutSeparators(
      support::parseIntegerLiteral(spelling(token), support::IntegerBaseRule::DecimalLeadingZero)
          .number);
  std::string_view text = number;
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
