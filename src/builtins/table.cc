// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The rows, and the two lookups over them.
//
// The table is `constexpr` static data: no `Session`, no allocation, no lifetime.
// A table of facts needs none of them, and a builtin that needed one would be a
// builtin whose semantics depended on the compilation that read it.
//
// ### What a row may not be
//
// Reading the rows below, the useful question is not "what does it say" but "what
// is *not* here":
//
//   - Nothing the compiler emits for a construct: the object copy's `memcpy`, the
//     `llvm.memcpy` of an array assignment, the `llvm.trap` of a checked division,
//     `sret`. They are family 0 in `docs/architectures/builtins.md`: a row would
//     make them *writable*, and a writable operation is a promise about semantics
//     the language never made.
//   - Nothing a library could implement with the language as it stands: `exit`,
//     `abort`, `memcpy`, `strlen`, `sqrt`, `abs` are symbols in a runtime and are
//     declared with `extern fn`. The burden of proof is on leaving that list.
//   - Nothing whose only effect is metadata: `expect`, `assume`, `unreachable`.
//     The first two are a decision about the `ir` invariant scan's attribute
//     permit-list rather than a gap in a builtin list, and `unreachable` is
//     undefined behavior by construction: it is not here until a checked build
//     can hold it, and a row that cannot be lowered must not exist at all.
#include "builtins/builtin.h"

#include <array>
#include <string_view>

namespace minc::builtins {
namespace {

// The zero-argument list, spelled once so every such row points at the same
// empty span instead of at its own.
constexpr std::array<BuiltinType, 0> kNoArgs{};
// `f(x)` where `x` is an integer of any width: the family the four bit
// operations are one row of each.
constexpr std::array<BuiltinType, 1> kIntegerArg{BuiltinType::AnyInteger};
// `f(x, n)`: the value, then the count. The count is deliberately *its own*
// integer type and not `MatchArg`: this language converts no integer implicitly,
// so forcing `rotl(x: u8, n: u8)` would make every count written as a plain
// literal or held in a `usize` an error, for no gain -- the count's meaning does
// not depend on the width it is written in.
constexpr std::array<BuiltinType, 2> kRotateArgs{BuiltinType::AnyInteger, BuiltinType::AnyInteger};

// The table, in `BuiltinId` order, and the size is checked against the enum: a
// new enumerator with no row does not compile, and a row for a name that does not
// exist cannot be written.
//
// Every row states its own answer to "what does this do to the world" and "does
// the language promise it", and the two questions are related but not the same:
// `clz` is `Effect::None` and `Status::Stable` because every input has a defined
// answer, `__builtin_trap` is `Effect::Diverges` and `Status::Internal` because
// it is the raw layer the runtime is written in.
constexpr std::array<BuiltinInfo, kBuiltinIdCount> kRows{{
    BuiltinInfo{BuiltinId::Clz, "clz", SpellingClass::Prelude,
                Signature{kIntegerArg, BuiltinType::MatchArg, MatchedWidths::Any}, Effect::None,
                Lowering{Lowering::Kind::Intrinsic, "llvm.ctlz", TailOperand::I1False,
                         Lowering::Overload::FirstArgument},
                Status::Stable,
                "the number of leading zero bits, and the width when the value is zero"},

    BuiltinInfo{BuiltinId::Ctz, "ctz", SpellingClass::Prelude,
                Signature{kIntegerArg, BuiltinType::MatchArg, MatchedWidths::Any}, Effect::None,
                Lowering{Lowering::Kind::Intrinsic, "llvm.cttz", TailOperand::I1False,
                         Lowering::Overload::FirstArgument},
                Status::Stable,
                "the number of trailing zero bits, and the width when the value is zero"},

    BuiltinInfo{BuiltinId::Popcount, "popcount", SpellingClass::Prelude,
                Signature{kIntegerArg, BuiltinType::MatchArg, MatchedWidths::Any}, Effect::None,
                Lowering{Lowering::Kind::Intrinsic, "llvm.ctpop", TailOperand::None,
                         Lowering::Overload::FirstArgument},
                Status::Stable, "the number of bits set"},

    // The one row whose *widths* are restricted, and the reason the field exists:
    // `llvm.bswap.i8` does not verify ("bswap must be an even number of bytes"),
    // so a `bswap` of a `u8` is not a program with an undefined answer -- it is a
    // program whose module LLVM refuses to accept. The refusal belongs here, before
    // the call is typed, and not in a verifier error after it.
    BuiltinInfo{BuiltinId::Bswap, "bswap", SpellingClass::Prelude,
                Signature{kIntegerArg, BuiltinType::MatchArg, MatchedWidths::EvenBytes},
                Effect::None,
                Lowering{Lowering::Kind::Intrinsic, "llvm.bswap", TailOperand::None,
                         Lowering::Overload::FirstArgument},
                Status::Stable, "the bytes of an integer in the opposite order"},

    BuiltinInfo{BuiltinId::Rotl, "rotl", SpellingClass::Prelude,
                Signature{kRotateArgs, BuiltinType::MatchArg, MatchedWidths::Any}, Effect::None,
                Lowering{Lowering::Kind::RotateLeft, "llvm.fshl", TailOperand::None,
                         Lowering::Overload::FirstArgument},
                Status::Stable, "the value rotated left, by a count taken modulo the width"},

    BuiltinInfo{BuiltinId::Rotr, "rotr", SpellingClass::Prelude,
                Signature{kRotateArgs, BuiltinType::MatchArg, MatchedWidths::Any}, Effect::None,
                Lowering{Lowering::Kind::RotateRight, "llvm.fshr", TailOperand::None,
                         Lowering::Overload::FirstArgument},
                Status::Stable, "the value rotated right, by a count taken modulo the width"},

    // The result is `!` and not `void`, and that is a fact about the flow pass
    // rather than a flourish: `sema` asks an expression's *type* whether control
    // comes back, so `fn i32 fail() -> i32 { __builtin_trap(); }` is a function
    // that returns and not one that falls off its end.
    BuiltinInfo{BuiltinId::Trap, "__builtin_trap", SpellingClass::Reserved,
                Signature{kNoArgs, BuiltinType::Never, MatchedWidths::Any}, Effect::Diverges,
                Lowering{Lowering::Kind::Intrinsic, "llvm.trap", TailOperand::None,
                         Lowering::Overload::None},
                Status::Internal, "stop the program on the spot, where a debugger can see it"},
}};

static_assert(kRows.size() == kBuiltinIdCount,
              "every BuiltinId needs exactly one row and every row one id");
static_assert(kBuiltinIdCount + 1 <= kBuiltinIdLimit, "BuiltinId must hold every row plus None");

} // namespace

std::span<const BuiltinInfo> all() {
  return kRows;
}

const BuiltinInfo* lookup(std::string_view spelling) {
  for (const BuiltinInfo& row : kRows) {
    if (row.spelling == spelling) {
      return &row;
    }
  }
  return nullptr;
}

const BuiltinInfo* lookup(BuiltinId id) {
  // The table is in id order and an id is not a table index: `None` is not a
  // row, and a row's position is not a promise the enum's numbering makes. The
  // scan is over a handful of entries of static storage, and it is one function
  // so that no caller can decide to index instead.
  if (id == BuiltinId::None) {
    return nullptr;
  }
  for (const BuiltinInfo& row : kRows) {
    if (row.id == id) {
      return &row;
    }
  }
  return nullptr;
}

} // namespace minc::builtins
