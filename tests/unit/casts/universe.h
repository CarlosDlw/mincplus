// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The type universe the cast tests walk, written once.
//
// Two suites ask the same question from two sides -- `sema/cast_test.cc` about
// the matrix, `ir/cast_test.cc` about the module -- and they have to be asking it
// about the *same* types, or one of them is testing an alphabet the other does
// not have. So the list, the lookup and the "which instruction does this pair
// need" table live here and both read them.
//
// The universe is what the language has **today**: every type a value can have,
// plus the two the matrix still answers (`void`, and the deferred literals, which
// are the type an expression has before a context decides one). It is
// deliberately not the C spellings -- `int`, `long`, `size_t` and the rest
// resolve to these same `TypeId`s, which is the identity the store is built on
// (`sema/type.h`), and a row per spelling would test nothing a second time.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "sema/convert.h"
#include "sema/type.h"
#include "sema/type_store.h"

namespace minc::test {
namespace casts {

// The names, in the order the tests print them. The three pointer rows are
// representatives of one class rather than the whole product: a pointer cast is
// the identity whatever the pointee, so `*i32`, `*u8`, `*f64` and the untyped
// `*void` cover every arm the matrix has (`To`/`From` `*void`, and the rest).
inline constexpr std::string_view kUniverse[] = {
    "i8",
    "i16",
    "i32",
    "i64",
    "i128",
    "u8",
    "u16",
    "u32",
    "u64",
    "u128",
    "f32",
    "f64",
    "f80",
    "bool",
    "char",
    "str",
    "*i32",
    "*u8",
    "*void",
    "*f64",
    "[4]i32",
    "[]i32",
    "void",
    "!",
    "<int literal>",
    "<float literal>",
};

[[nodiscard]] constexpr std::span<const std::string_view> universe() {
  return kUniverse;
}

// The type a name in the list is. An unknown name is `kTypeError`, which is what
// makes a typo in a test a failure rather than a silent default -- the same rule
// the matrix itself follows about pairs nobody decided.
[[nodiscard]] inline sema::TypeId typeOf(sema::TypeStore& types, std::string_view name) {
  for (const std::string_view one : kUniverse) {
    if (one == name) {
      if (name == "i8") {
        return sema::kTypeI8;
      }
      if (name == "i16") {
        return sema::kTypeI16;
      }
      if (name == "i32") {
        return sema::kTypeI32;
      }
      if (name == "i64") {
        return sema::kTypeI64;
      }
      if (name == "i128") {
        return sema::kTypeI128;
      }
      if (name == "u8") {
        return sema::kTypeU8;
      }
      if (name == "u16") {
        return sema::kTypeU16;
      }
      if (name == "u32") {
        return sema::kTypeU32;
      }
      if (name == "u64") {
        return sema::kTypeU64;
      }
      if (name == "u128") {
        return sema::kTypeU128;
      }
      if (name == "f32") {
        return sema::kTypeF32;
      }
      if (name == "f64") {
        return sema::kTypeF64;
      }
      if (name == "f80") {
        return sema::kTypeF80;
      }
      if (name == "bool") {
        return sema::kTypeBool;
      }
      if (name == "char") {
        return sema::kTypeChar;
      }
      if (name == "str") {
        return sema::kTypeStr;
      }
      if (name == "void") {
        return sema::kTypeVoid;
      }
      if (name == "!") {
        return sema::kTypeNever;
      }
      if (name == "*i32") {
        return types.pointerTo(sema::kTypeI32);
      }
      if (name == "*u8") {
        return types.pointerTo(sema::kTypeU8);
      }
      if (name == "*void") {
        return types.pointerTo(sema::kTypeVoid);
      }
      if (name == "*f64") {
        return types.pointerTo(sema::kTypeF64);
      }
      if (name == "[4]i32") {
        return types.arrayOf(sema::kTypeI32, 4);
      }
      if (name == "[]i32") {
        return types.sliceOf(sema::kTypeI32);
      }
      if (name == "<int literal>") {
        return sema::kTypeIntLiteral;
      }
      if (name == "<float literal>") {
        return sema::kTypeFloatLiteral;
      }
      break;
    }
  }
  return sema::kTypeError;
}

// Can this type be written as a **parameter**, and so appear in the generated
// program the `ir` suite builds? `void` has no object, `!` has no value to write
// (it is only ever a destination), and a deferred literal is the state of a
// literal rather than a type a declaration can name -- `sema` decides every one
// of them before the lowering sees it.
[[nodiscard]] inline bool hasValues(std::string_view name) {
  return name != "void" && name != "!" && name != "<int literal>" && name != "<float literal>";
}

// Can this type be **named** on this target? `f80` is the one row whose existence
// is a property of the machine and not of the width table: it is x87's format, so
// AArch64 and RISC-V have no such type and the checker refuses the *spelling*
// there (`TypeSpecTest.Float80IsRefusedWhereTheMachineHasNoX87`). The suite
// therefore asks this before it writes a program, and the id level is untouched:
// every store still carries the row, because the numbering is fixed and shared
// (`sema/type.h`), which is why the matrix suites may keep asking about 26 types
// while a generated program has one fewer on a machine with no x87.
[[nodiscard]] inline bool nameable(const sema::TypeStore& types, std::string_view name) {
  return name != "f80" || types.target().hasFloat80();
}

// Can this type be the **destination** of a cast a program can write? `void` has
// nothing to convert to, and `!` is refused by the matrix and not even readable
// after `as` -- both are pinned in `sema/cast_test.cc` over the table, and neither
// is a pair a generated program can spell.
[[nodiscard]] inline bool canBeDestination(std::string_view name) {
  return name != "void" && name != "!";
}

// True when the type is signed, read the way the language's own rule reads it:
// only an `Int` carries a sign. `char` is unsigned by decision and `bool` is not
// an integer at all (`sema/type.h`).
[[nodiscard]] inline bool signedType(const sema::TypeStore& types, sema::TypeId id) {
  const sema::Type& info = types.get(id);
  return info.kind == sema::TypeKind::Int && info.isSigned;
}

// Bits of an integer-shaped type: `char` is the eight bits it is, and `bool` is
// the **one** bit of range it has -- not the thirty-two its storage leaves empty,
// which is what would make `true as f32` look like a rounding.
[[nodiscard]] inline std::uint16_t integerWidth(const sema::TypeStore& types, sema::TypeId id) {
  const sema::Type& info = types.get(id);
  if (info.kind == sema::TypeKind::Bool) {
    return 1;
  }
  if (info.kind == sema::TypeKind::Char) {
    return 8;
  }
  return info.bits == 0 ? 32 : info.bits;
}

// The bits of an integer a float's mantissa holds exactly -- the same four
// numbers `sema`'s matrix uses to decide what `-Wcast` calls `precision`.
[[nodiscard]] inline std::uint16_t mantissaBits(std::uint16_t floatBits) {
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

// The instruction a pair needs, spelled the way the module spells it: empty for a
// pair that emits nothing at all, `guarded` for the one row whose shape is a test
// and a trap, and the opcode's own name otherwise.
//
// This is a *reading of the matrix* and not a second copy of it: the kind comes
// from `sema::castResult`, so a row that changes fails both suites at once
// instead of leaving this table behind. The `ir` suite is where it stops being a
// reading -- there the name is looked for in the module.
[[nodiscard]] inline std::string_view instructionFor(const sema::TypeStore& types,
                                                     sema::TypeId from, sema::TypeId to) {
  const sema::CastResult result = sema::castResult(types, from, to);
  if (!result.ok) {
    return "refused";
  }
  switch (result.kind) {
  case sema::CastKind::Identity:
    return "";
  case sema::CastKind::BoolToInteger:
    return "zext";
  case sema::CastKind::IntegerExtend:
    return signedType(types, from) ? "sext" : "zext";
  case sema::CastKind::IntegerTruncate:
    return "trunc";
  case sema::CastKind::IntegerToBool:
    return "icmp ne";
  case sema::CastKind::IntegerToFloat:
    return signedType(types, from) ? "sitofp" : "uitofp";
  case sema::CastKind::FloatToInteger:
    // Two comparisons, one branch and one call: the guard, and the conversion it
    // protects (`casts.md`, *Float → integer*).
    return "guarded";
  case sema::CastKind::FloatExtend:
    return "fpext";
  case sema::CastKind::FloatTruncate:
    return "fptrunc";
  case sema::CastKind::PointerToInteger:
    return "ptrtoint";
  case sema::CastKind::IntegerToPointer:
    return "inttoptr";
  case sema::CastKind::None:
    break;
  }
  return "refused";
}

} // namespace casts
} // namespace minc::test
