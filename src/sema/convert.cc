// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "sema/convert.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace minc::sema {
namespace {

// Rank for the usual arithmetic conversions. C ranks by kind; this language has
// no `size_t`-shaped surprises, so ranking by width is the same order and one
// fewer thing to keep in step. A deferred literal never reaches here (it is
// handled before the concrete case), but it is answered rather than asserted.
[[nodiscard]] std::uint16_t rank(const TypeStore& types, TypeId type) {
  const Type& info = types.get(type);
  if (info.kind == TypeKind::IntLiteral) {
    return 32;
  }
  if (info.kind == TypeKind::FloatLiteral) {
    return 64;
  }
  return info.bits;
}

[[nodiscard]] bool isSigned(const TypeStore& types, TypeId type) {
  const Type& info = types.get(type);
  if (info.kind == TypeKind::Char) {
    // `char` is an integer type and is unsigned, which is the README's decision;
    // it still promotes to `i32`, where the signedness stops mattering.
    return false;
  }
  return info.isSigned;
}

[[nodiscard]] TypeId asFloat(TypeStore& types, std::uint16_t bits) {
  return types.floatOf(bits);
}

// The width of the value an integer type holds, with `char` counted as the eight
// bits it is (`README`, *Types*).
//
// `bool` is answered as **one** bit and not as the thirty-two its storage leaves
// empty: it is not an integer type here, and the three rules that mention it are
// written out on their own -- but this function is asked about a *range*, and a
// range of 0/1 fits every float there is. Answering 32 would call `true as f32`
// a loss of precision, which is a `-Wcast` sentence about a rounding that never
// happens.
[[nodiscard]] std::uint16_t integerWidth(const TypeStore& types, TypeId id) {
  const Type& info = types.get(id);
  if (info.kind == TypeKind::Bool) {
    return 1;
  }
  if (info.kind == TypeKind::Char) {
    return 8;
  }
  return info.bits == 0 ? 32 : info.bits;
}

[[nodiscard]] std::uint16_t floatWidth(const TypeStore& types, TypeId id) {
  return types.get(id).bits;
}

// The bits of an integer a float's mantissa can hold exactly.
//
// `f32` has 24 (23 stored plus the implicit one), `f64` has 53, the x87 format
// 64, and IEEE binary128 113. An integer wider than that converts to a *rounded*
// float, which is the loss `-Wcast` names.
[[nodiscard]] std::uint16_t mantissaBits(std::uint16_t floatBits) {
  switch (floatBits) {
  case 32:
    return 24;
  case 64:
    return 53;
  case 80:
    return 64;
  default:
    return 113;
  }
}

[[nodiscard]] CastResult refused(std::string message) {
  CastResult result;
  result.message = std::move(message);
  return result;
}

[[nodiscard]] CastResult accepted(CastKind kind, CastLoss loss = CastLoss::None) {
  CastResult result;
  result.kind = kind;
  result.ok = true;
  result.loss = loss;
  return result;
}

} // namespace

TypeId promote(TypeStore& types, TypeId type) {
  const Type& info = types.get(type);
  switch (info.kind) {
  case TypeKind::Bool:
  case TypeKind::Char:
    // `bool` promotes to `int` in C. Here it is not arithmetic at all, so it
    // never reaches this function from an operator -- but a `bool` used where a
    // value's width matters is still an `int`-sized object, and returning `i32`
    // keeps that true instead of introducing a second rule.
    return types.signedInt(32);
  case TypeKind::Int:
    return info.bits < 32 ? types.signedInt(32) : type;
  default:
    return type;
  }
}

TypeId operationType(TypeStore& types, bool shift, TypeId left, TypeId right) {
  // A shift's result is its left operand's promoted type and its count is
  // promoted independently, so `x >> n` and `x >>= n` cannot disagree about the
  // width the instruction is performed at.
  return shift ? promote(types, left) : usualArithmetic(types, left, right);
}

TypeId usualArithmetic(TypeStore& types, TypeId left, TypeId right) {
  if (types.isError(left) || types.isError(right)) {
    return kTypeError;
  }
  const bool leftDeferred = types.isDeferred(left);
  const bool rightDeferred = types.isDeferred(right);

  // Both undecided: the result stays undecided, and only the *class* matters --
  // `1 + 2` is an integer literal, `1.0 + 2.0` is a float literal, and whatever
  // context follows decides the width once. Two literals of *different* classes
  // have no common type: the class of a number is the spelling's, and the two do
  // not meet (`convertible`).
  if (leftDeferred && rightDeferred) {
    const bool leftFloat = types.get(left).kind == TypeKind::FloatLiteral;
    const bool rightFloat = types.get(right).kind == TypeKind::FloatLiteral;
    if (leftFloat != rightFloat) {
      return kTypeError;
    }
    return leftFloat ? kTypeFloatLiteral : kTypeIntLiteral;
  }
  // One undecided: it adopts the other side, and only within its own class. The
  // two classes adopt differently, and each difference is a rule:
  //
  //   * an **integer** literal takes the other side *promoted*, so `1 + u8` is
  //     `i32` and not `u8` -- the promotion is what the addition happens at;
  //   * a **float** literal takes the other side exactly, because a float has no
  //     promotion: `1.0 + x` with an `f32` `x` is an `f32`, and defaulting the
  //     literal to `f64` first would widen a computation the reader wrote
  //     narrowly.
  //
  // A literal whose class is the other side's has no common type with it, which is
  // the same refusal two concrete operands get below.
  if (leftDeferred || rightDeferred) {
    const TypeId literalSide = leftDeferred ? left : right;
    const TypeId concrete = promote(types, leftDeferred ? right : left);
    if (types.get(literalSide).kind == TypeKind::FloatLiteral) {
      return types.isFloat(concrete) ? concrete : kTypeError;
    }
    return types.isInteger(concrete) ? concrete : kTypeError;
  }

  if (!types.isArithmetic(left) || !types.isArithmetic(right)) {
    return kTypeError;
  }
  // An integer and a float have no common type: not by picking the float (C's
  // answer, and a value the source never wrote) and not by picking the integer
  // (which would silently drop the fractional part). `convertible` states the
  // rule; this is the operator asking it.
  if (types.isFloat(left) != types.isFloat(right)) {
    return kTypeError;
  }
  if (types.isFloat(left)) {
    // Two floats: the wider one, which is C's rule and needs no cast: `f32` and
    // `f64` are one class, and the widening is exact.
    const std::uint16_t leftBits = types.get(left).bits;
    const std::uint16_t rightBits = types.get(right).bits;
    return asFloat(types, leftBits > rightBits ? leftBits : rightBits);
  }

  const TypeId leftPromoted = promote(types, left);
  const TypeId rightPromoted = promote(types, right);
  if (leftPromoted == rightPromoted) {
    return leftPromoted;
  }
  const bool leftSigned = isSigned(types, leftPromoted);
  const bool rightSigned = isSigned(types, rightPromoted);
  const std::uint16_t leftBits = rank(types, leftPromoted);
  const std::uint16_t rightBits = rank(types, rightPromoted);
  if (leftSigned == rightSigned) {
    return leftBits >= rightBits ? leftPromoted : rightPromoted;
  }
  // Different signedness. The unsigned side wins at equal or greater rank; a
  // wider signed type wins only if it can represent every value of the unsigned
  // one -- and since the widths here are powers of two, "wider" is exactly that.
  const TypeId unsignedSide = leftSigned ? rightPromoted : leftPromoted;
  const TypeId signedSide = leftSigned ? leftPromoted : rightPromoted;
  const std::uint16_t unsignedBits = leftSigned ? rightBits : leftBits;
  const std::uint16_t signedBits = leftSigned ? leftBits : rightBits;
  if (unsignedBits >= signedBits) {
    return unsignedSide;
  }
  return signedSide;
}

bool mixedNumberPair(const TypeStore& types, TypeId from, TypeId to) {
  if (types.isError(from) || types.isError(to)) {
    return false; // the poison is not a number, and it converts to everything
  }
  const auto floatSide = [&types](TypeId id) {
    const TypeKind kind = types.get(id).kind;
    return kind == TypeKind::Float || kind == TypeKind::FloatLiteral;
  };
  const auto integerSide = [&types](TypeId id) {
    const TypeKind kind = types.get(id).kind;
    return kind == TypeKind::Int || kind == TypeKind::Char || kind == TypeKind::IntLiteral;
  };
  return (floatSide(from) && integerSide(to)) || (integerSide(from) && floatSide(to));
}

bool convertible(const TypeStore& types, TypeId from, TypeId to) {
  if (types.isError(from) || types.isError(to)) {
    return true; // the poison converts to and from everything, silently
  }
  if (from == to) {
    return true;
  }
  const TypeKind fromKind = types.get(from).kind;
  const TypeKind toKind = types.get(to).kind;
  // The bottom type converts into everything, and nothing converts into it.
  //
  // That asymmetry is the whole of its meaning. "A value of type `!` becomes a
  // value of type `T`" is vacuous rather than false: the expression never
  // produces a value, so there is every value it will fail to produce, and the
  // only thing a consumer can do with the fact is proceed -- `c ? 1 : die()` is
  // an `i32`, `let x: i32 = die();` is legal, `return die();` is legal. The other
  // direction has no reading at all: a value that is *not* a `!` cannot become
  // one, and a type that admits no values cannot be arrived at.
  //
  // Rust states the same rule the same way -- `!` coerces into any other type,
  // and is deliberately not a subtype (`RFC 1216`, `primitive.never`) -- and the
  // difference matters here because this language has no subtyping to hang it on:
  // a conversion is what `checkAssignable` already asks about, so the feature
  // lands in the *existing* rule instead of adding a second relation.
  if (fromKind == TypeKind::Never) {
    return true;
  }
  if (fromKind == TypeKind::Void || toKind == TypeKind::Void) {
    return false;
  }
  // Pointers convert to pointers of the *same* pointee, and to and from `*void`.
  //
  // `*void` is the untyped pointer, and both directions are implicit because
  // that is the only way a value that names no type can be used at all: an
  // allocator returns one, a `null` is one, and a cast does not exist yet.
  // Everything else needs an explicit reinterpretation, which the language does
  // not have (`memory.md`: memory has no effective type, so punning is *defined*,
  // but it is never *implicit* -- C requires the cast for the same reason).
  //
  // A pointer is not an integer, and no arm below reaches one: neither direction
  // is a conversion here. The two operations that exist for it are named in the
  // model (`expose`, `with_exposed_provenance`) and are not in the grammar yet.
  if (fromKind == TypeKind::Pointer || toKind == TypeKind::Pointer) {
    if (fromKind != TypeKind::Pointer || toKind != TypeKind::Pointer) {
      return false;
    }
    return types.get(from).pointee == types.get(to).pointee || types.isVoidPointer(from) ||
           types.isVoidPointer(to);
  }
  // `bool` and `str` are not arithmetic, so they convert only to themselves.
  // C would promote a `bool` to `int` here; that promotion is what makes
  // `flag + 1` compile, and it is the footgun this language does not keep.
  if (fromKind == TypeKind::Bool || toKind == TypeKind::Bool || fromKind == TypeKind::Str ||
      toKind == TypeKind::Str) {
    return false;
  }
  // An integer and a float do not convert into each other, in **either**
  // direction.
  //
  // This is the one place the language departs from C's arithmetic, and it
  // departs on purpose. C turns `double d = 1;` into a silent widening and
  // `1 + 2.0` into a `double`, and both are values the reader did not write --
  // one of them with a rounding they cannot see. Here the class of a number is
  // the class of its **spelling** (`1` is an integer, `1.0` is a float), and
  // crossing between the two is a cast, which the language does not have yet and
  // will spell out when it does.
  //
  // One rule, in the one place every conversion is defined, so it covers every
  // consumer there is: an initializer, an assignment, an argument, a `return`
  // (`checkAssignable`) and the operands of an arithmetic operator
  // (`usualArithmetic`). An integer converts to another integer, a float to
  // another float, and a narrower one to a wider one of its own class.
  if (mixedNumberPair(types, from, to)) {
    return false;
  }
  // Everything else that reaches here is arithmetic (or a function type, which
  // has no conversion at all).
  return types.isArithmetic(from) && types.isArithmetic(to);
}

bool narrows(const TypeStore& types, TypeId from, TypeId to) {
  if (types.isError(from) || types.isError(to) || from == to) {
    return false;
  }
  const Type& fromInfo = types.get(from);
  const Type& toInfo = types.get(to);
  if (!types.isArithmetic(from) || !types.isArithmetic(to)) {
    return false;
  }
  // A float to an integer truncates; a float to a narrower float rounds.
  if (fromInfo.kind == TypeKind::Float || fromInfo.kind == TypeKind::FloatLiteral) {
    if (toInfo.kind == TypeKind::Int || toInfo.kind == TypeKind::Char) {
      return true;
    }
    return fromInfo.bits > toInfo.bits;
  }
  if (toInfo.kind == TypeKind::Float || toInfo.kind == TypeKind::FloatLiteral) {
    return false;
  }
  // Integer to integer.
  std::uint16_t fromBits = fromInfo.bits;
  std::uint16_t toBits = toInfo.bits;
  if (fromInfo.kind == TypeKind::IntLiteral || toInfo.kind == TypeKind::IntLiteral) {
    // An undecided literal fits whatever it is asked to; the range check is
    // `fitsIn`'s job and it produces an error, not a warning.
    return false;
  }
  if (fromInfo.kind == TypeKind::Char) {
    fromBits = 8;
  }
  if (toInfo.kind == TypeKind::Char) {
    toBits = 8;
  }
  if (fromBits > toBits) {
    return true;
  }
  if (fromBits < toBits) {
    return false;
  }
  return isSigned(types, from) && !isSigned(types, to);
}

CastResult castResult(const TypeStore& types, TypeId from, TypeId to) {
  if (!from.valid() || !to.valid() || types.isError(from) || types.isError(to)) {
    // The poison spreads, and the checker said nothing about it: a second
    // sentence here would be about a mistake the reader has already been told
    // about, at the operand where it was written. The empty message is how the
    // caller knows to stay quiet.
    return CastResult{};
  }
  if (from == to) {
    // A cast to the type a value already has is the absence of a conversion: it
    // is legal, it says something (`x as i32` is how a reader writes "I know"),
    // and it must cost nothing -- decision 14 of the record.
    return accepted(CastKind::Identity);
  }

  const TypeKind fromKind = types.get(from).kind;
  const TypeKind toKind = types.get(to).kind;
  const std::string fromName(types.spelling(from));
  const std::string toName(types.spelling(to));

  // `!` is the type of an expression that never produces a value, so a cast of
  // one is *vacuous* and not a conversion: `never.md` already ships the rule for
  // the implicit case, and there is nothing for a cast to add. Nothing converts
  // *into* `!`: a type that admits no values cannot be arrived at.
  if (fromKind == TypeKind::Never) {
    return accepted(CastKind::Identity);
  }
  if (toKind == TypeKind::Never) {
    return refused("`!` is the type of an expression that never produces a value, so nothing "
                   "converts *into* it");
  }
  if (fromKind == TypeKind::Void) {
    return refused("`void` is the absence of a value, so there is nothing to convert");
  }
  if (toKind == TypeKind::Void) {
    return refused("`void` is the absence of a value: a cast cannot make one -- if the value is "
                   "unused, do not name it");
  }
  if (fromKind == TypeKind::Function || toKind == TypeKind::Function) {
    // There is one exception a C reader will look for and not find, and the
    // record names it: a function's *address* is not a function type, so a cast
    // between the two is a cast of a pointer to a pointer, which is an identity.
    return refused("a function type is not an object: take the address of the function first, "
                   "and cast the pointer");
  }

  // --- aggregates -------------------------------------------------------------
  //
  // Each refusal names what to write instead, because a refusal that does not is
  // a refusal that gets worked around (`casts.md`, *What is refused*).
  if (fromKind == TypeKind::Array) {
    return refused("an array is not a pointer and it does not decay: `&a[0]` is the address of "
                   "its first element, and the count travels beside it");
  }
  if (fromKind == TypeKind::Slice) {
    return refused("a view is a pointer *and* a length: `&s[0]` is the address of its first "
                   "element, and the extent is the view's own business");
  }
  if (toKind == TypeKind::Array || toKind == TypeKind::Slice) {
    return refused("`" + fromName + "` is not a `" + toName +
                   "`: an array converts element by element or not at all, and a view is taken "
                   "from an object that already has one");
  }

  const bool fromInteger = fromKind == TypeKind::Int || fromKind == TypeKind::Char;
  const bool toInteger = toKind == TypeKind::Int || toKind == TypeKind::Char;
  const bool fromFloat = fromKind == TypeKind::Float;
  const bool toFloat = toKind == TypeKind::Float;
  const bool fromAddress = fromKind == TypeKind::Pointer || fromKind == TypeKind::Str;
  const bool toAddress = toKind == TypeKind::Pointer || toKind == TypeKind::Str;

  // --- bool -------------------------------------------------------------------
  //
  // `bool` is not arithmetic here, so it is not reached by the integer arms: the
  // two directions are written out, and `bool` → float is allowed for the same
  // reason `bool` → integer is (it is a value with a definition).
  if (fromKind == TypeKind::Bool) {
    if (toInteger) {
      return accepted(CastKind::BoolToInteger);
    }
    if (toFloat) {
      return accepted(CastKind::IntegerToFloat);
    }
    if (toAddress) {
      return refused("a `bool` is not an address: `true` and `false` name no object");
    }
  }
  if (toKind == TypeKind::Bool) {
    if (fromInteger) {
      return accepted(CastKind::IntegerToBool);
    }
    if (fromFloat) {
      // NaN is neither true nor false, which is why this is refused and not
      // defined as `x != 0.0` (`casts.md`, decision 10).
      return refused("NaN is neither true nor false, so a float has no `bool` to convert to: "
                     "write `" +
                     fromName + " != 0.0`");
    }
    if (fromAddress) {
      return refused("a pointer is not a truth value: write `p != null`, which says the same "
                     "thing and says it once");
    }
  }

  // --- integers ---------------------------------------------------------------
  if (fromInteger && toInteger) {
    const std::uint16_t fromBits = integerWidth(types, from);
    const std::uint16_t toBits = integerWidth(types, to);
    if (toBits > fromBits) {
      // Value-preserving: every value of a narrower integer is a value of a wider
      // one. Two exceptions, and both are *pushes*, not different rules -- an
      // unsigned narrower source into a signed wider target is a `zext` and the
      // result is still a value (there is no sign to lose that was not already
      // lost in the source's own type).
      return accepted(CastKind::IntegerExtend);
    }
    if (toBits < fromBits) {
      return accepted(CastKind::IntegerTruncate, CastLoss::Truncation);
    }
    // The same width: the bits *are* the value, so there is no instruction and
    // the only thing that can change is what the bits mean. `char` and `u8` are
    // the same type in representation, so that pair loses nothing; `i32` and
    // `u32` are two readings of one pattern, which is what `-Wcast` names.
    const bool fromSigned = isSigned(types, from);
    const bool toSigned = isSigned(types, to);
    return accepted(CastKind::Identity, fromSigned != toSigned ? CastLoss::Sign : CastLoss::None);
  }

  // --- floats -----------------------------------------------------------------
  if (fromFloat && toFloat) {
    if (floatWidth(types, to) > floatWidth(types, from)) {
      return accepted(CastKind::FloatExtend);
    }
    return accepted(CastKind::FloatTruncate, CastLoss::Precision);
  }
  if (fromInteger && toFloat) {
    // Defined and rounded to nearest, as the hardware does it. The loss is real
    // when the integer holds values the destination's mantissa cannot: `i32`→
    // `f32`, `i64`→`f32`/`f64`, anything→`f32` above 24 bits.
    const bool loses = integerWidth(types, from) > mantissaBits(floatWidth(types, to));
    return accepted(CastKind::IntegerToFloat, loses ? CastLoss::Precision : CastLoss::None);
  }
  if (fromFloat && toInteger) {
    // **The guarded row.** Two losses, and both are named: the conversion
    // truncates toward zero, and a value outside the destination's range traps
    // rather than becoming poison (`casts.md`).
    return accepted(CastKind::FloatToInteger, CastLoss::Range | CastLoss::Truncation);
  }

  // --- addresses --------------------------------------------------------------
  if (fromAddress && toAddress) {
    // Opaque pointers: a pointer cast is a type change with no instruction.
    return accepted(CastKind::Identity);
  }
  if (fromAddress && toInteger) {
    // `memory.md`'s `expose`, and it is *counted* rather than forbidden --
    // `-Wprovenance` names every site. The loss is the address's width when the
    // integer is narrower than the pointer.
    const bool loses = integerWidth(types, to) < types.target().pointerBits;
    return accepted(CastKind::PointerToInteger, loses ? CastLoss::Truncation : CastLoss::None);
  }
  if (fromInteger && toAddress) {
    // `with_exposed_provenance`: permission over the allocations whose
    // provenance has been exposed, and no other.
    const bool loses = integerWidth(types, from) < types.target().pointerBits;
    return accepted(CastKind::IntegerToPointer, loses ? CastLoss::Truncation : CastLoss::None);
  }
  if (fromFloat && toAddress) {
    return refused("a float is not an address: expose a pointer with `p as usize` if that is "
                   "what was meant");
  }

  // Everything else: two types with no conversion between them, written down or
  // not. The sentence names both, because the reader has to see which pair it is.
  return refused("`" + fromName + "` and `" + toName +
                 "` do not convert into each other, and a cast cannot make one: a conversion "
                 "changes a value, and these two are not two of one kind");
}

std::optional<support::ConstInt> foldIntCast(const TypeStore& types, TypeId from, TypeId to,
                                             support::ConstInt value) {
  const auto widthOf = [&types](TypeId id) -> std::uint16_t {
    const TypeKind kind = types.get(id).kind;
    if (kind == TypeKind::Bool) {
      return 1;
    }
    if (kind == TypeKind::Char) {
      return 8;
    }
    return types.get(id).bits;
  };
  const auto integerShaped = [](TypeKind kind) {
    return kind == TypeKind::Int || kind == TypeKind::Char || kind == TypeKind::Bool;
  };
  const TypeKind fromKind = types.get(from).kind;
  const TypeKind toKind = types.get(to).kind;
  if (!integerShaped(fromKind) || !integerShaped(toKind)) {
    return std::nullopt;
  }

  // An integer to a `bool` is `!= 0`, which is not a truncation: `2 as bool` is
  // `true` and a bit test would call it false. `bool` is not an integer type
  // here, so the one row that is not arithmetic is written out.
  if (toKind == TypeKind::Bool) {
    return support::ConstInt{value.bits != 0 ? 1U : 0U, false};
  }

  const std::uint16_t fromBits = widthOf(from);
  const std::uint16_t toBits = widthOf(to);
  const bool fromSigned = fromKind == TypeKind::Bool ? false : isSigned(types, from);
  const bool toSigned = toKind == TypeKind::Bool ? false : isSigned(types, to);

  // The source pattern, and then the *source's* extension to 64 bits: a signed
  // `-1` is `i32` is sixteen ones at 32 bits and must still be sixteen ones at
  // the point the target width reads it. Sign-extending here and letting the
  // target's width read the pattern with the target's own signedness is what
  // makes `(i128)-1` and `(u64)-1` two different answers from one fold.
  std::uint64_t source = value.bits;
  if (fromBits < 64) {
    source &= (std::uint64_t{1} << fromBits) - 1U;
  }
  std::uint64_t extended = source;
  if (fromSigned && fromBits < 64 && (source >> (fromBits - 1)) != 0) {
    extended = source | ~((std::uint64_t{1} << fromBits) - 1U);
  }

  // The target's pattern: the high bits are truncated when it is narrower, and
  // kept when it is wider (where the *value* reader extends the 64 bits this
  // core has to the width the type asks for).
  std::uint64_t pattern =
      toBits >= 64 ? extended : (extended & ((std::uint64_t{1} << toBits) - 1U));
  return support::ConstInt{pattern, !toSigned};
}

std::string_view toString(CastLoss loss) {
  switch (loss) {
  case CastLoss::None:
    return "none";
  case CastLoss::Truncation:
    return "truncation";
  case CastLoss::Sign:
    return "sign";
  case CastLoss::Precision:
    return "precision";
  case CastLoss::Range:
    return "range";
  }
  return "none";
}

std::string lossPhrase(CastLoss loss) {
  std::string out;
  const auto add = [&out](std::string_view word) {
    if (!out.empty()) {
      out += " and ";
    }
    out += word;
  };
  if (hasLoss(loss, CastLoss::Range)) {
    add("an out-of-range value traps");
  }
  if (hasLoss(loss, CastLoss::Truncation)) {
    add("bits are dropped");
  }
  if (hasLoss(loss, CastLoss::Sign)) {
    add("the sign changes meaning");
  }
  if (hasLoss(loss, CastLoss::Precision)) {
    add("the value is rounded");
  }
  return out;
}

bool fitsIn(const TypeStore& types, TypeId type, support::ConstInt value) {
  const Type& info = types.get(type);
  switch (info.kind) {
  case TypeKind::Char:
    return !value.negative() && value.bits <= 0xFFU;
  case TypeKind::Bool:
    return !value.negative() && value.bits <= 1U;
  case TypeKind::Int:
    break;
  default:
    // A float, a deferred literal, the poison: there is no integer range to
    // check against, and refusing would be inventing a rule.
    return true;
  }
  const std::uint16_t bits = info.bits == 0 ? 32 : info.bits;
  if (bits >= 64) {
    // The 64-bit core cannot say anything about a wider range, and every value
    // it can hold fits.
    return true;
  }
  const std::uint64_t magnitude = std::uint64_t{1} << (bits - 1);
  if (info.isSigned) {
    const std::int64_t min = -static_cast<std::int64_t>(magnitude);
    const std::int64_t max = static_cast<std::int64_t>(magnitude - 1);
    if (!value.isUnsigned) {
      const std::int64_t signedValue = value.signedValue();
      return signedValue >= min && signedValue <= max;
    }
    if (value.negative()) {
      // An unsigned bit pattern whose high bit is set is read as unsigned, so a
      // negative reading is impossible; treat it as out of range for a signed
      // target of the same width only when it exceeds the maximum.
      return false;
    }
    return value.bits <= static_cast<std::uint64_t>(max);
  }
  if (value.negative()) {
    return false;
  }
  const std::uint64_t max = (std::uint64_t{1} << bits) - 1U;
  return value.bits <= max;
}

} // namespace minc::sema
