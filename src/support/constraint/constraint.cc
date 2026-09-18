// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "support/constraint/constraint.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace minc::support {
namespace {

constexpr std::uint64_t bit(Operation op) {
  return std::uint64_t{1} << static_cast<std::uint8_t>(op);
}

// The constants, in one place and in the order a diagnostic lists them: the two
// identities, the two bounds, and the three a float has. The spelling is here and
// nowhere else, so the parser's reader, the checker's refusal and the dump cannot
// disagree about what a constant is called (`type_constants.md`).
constexpr std::array<TypeConstantInfo, 7> kTypeConstants = {{
    {TypeConstant::Zero, "ZERO"},
    {TypeConstant::One, "ONE"},
    {TypeConstant::Min, "MIN"},
    {TypeConstant::Max, "MAX"},
    {TypeConstant::Epsilon, "EPSILON"},
    {TypeConstant::Infinity, "INFINITY"},
    {TypeConstant::Nan, "NAN"},
}};

// The two operations every **scalar** admits and nothing else does. `bool`, `str`
// and a pointer are equatable and are not arithmetic, so `Eq` is wider than the
// arithmetic classes in members and narrower in grants -- which is the whole reason
// equality is a class of its own and not part of `Number`.
constexpr std::uint64_t kEquality = bit(Operation::Equal) | bit(Operation::NotEqual);

// Ordering, which is `Eq` plus the four. Every type that has an order has equality
// (a `<` that cannot answer `==` is not an order), so the inclusion is stated here
// rather than left for a reader to add up.
constexpr std::uint64_t kOrdering = kEquality | bit(Operation::Less) | bit(Operation::LessEqual) |
                                    bit(Operation::Greater) | bit(Operation::GreaterEqual);

// What a **number** admits: the four arithmetic operators, the unary sign and the
// step operators -- on top of ordering, because every number in this language is
// ordered. `%` is deliberately absent: `f64` is a number and `% f64` is refused, so
// a grant that included it would be a lie for half of `Number`'s members.
constexpr std::uint64_t kArithmetic = kOrdering | bit(Operation::Add) | bit(Operation::Sub) |
                                      bit(Operation::Mul) | bit(Operation::Div) |
                                      bit(Operation::Negate) | bit(Operation::Increment);

// What only an **integer** admits, and the reason `Integer` is a class the body
// needs and not a synonym for `Number`: the remainder, the bitwise family and the
// shifts are refused for a float by the operation rules themselves.
constexpr std::uint64_t kIntegerOnly = bit(Operation::Remainder) | bit(Operation::BitAnd) |
                                       bit(Operation::BitOr) | bit(Operation::BitXor) |
                                       bit(Operation::BitNot) | bit(Operation::ShiftLeft) |
                                       bit(Operation::ShiftRight);

// What a **pointer** admits: the comparisons and nothing else. `p < q` is an address
// comparison this language defines, and it is the whole of what is expressible on a
// pointer whose pointee nobody named -- `*p`, `p[i]` and `p + i` all need that type,
// which is why the header says so where the class is declared.
constexpr std::uint64_t kPointerGrants = kOrdering;

// The field order is the layout's: the two spans first (8-byte aligned), then the
// two enumerators, then the bit set -- which is what keeps this table from carrying
// more padding than the pointers need. clang-tidy's `performance.Padding` is what
// holds the order; the *reading* order is the table's below.
struct Row {
  std::string_view name;
  // The predicate the members come from, named rather than encoded: it is what the
  // `satisfies` check in `sema` switches on, and naming it here is how a reader
  // checks the invariant at the top of the header without leaving this file.
  std::string_view members;
  ConstraintClass klass;
  LiteralClass literal;
  std::uint64_t grants;
};

// One row per class, in `ConstraintClass` order. `members` is the predicate and
// `grants` the operations, and the test named in the header is what holds the two
// together.
constexpr Row kRows[] = {
    {"Any", "isObject", ConstraintClass::Any, LiteralClass::None, 0},
    {"Eq", "isScalar", ConstraintClass::Eq, LiteralClass::None, kEquality},
    // The pair that makes members and grants two facts: same members, and only the
    // comparison promised.
    {"Ordered", "isArithmetic", ConstraintClass::Ordered, LiteralClass::None, kOrdering},
    {"Number", "isArithmetic", ConstraintClass::Number, LiteralClass::None, kArithmetic},
    {"Integer", "isInteger", ConstraintClass::Integer, LiteralClass::Integer,
     kArithmetic | kIntegerOnly},
    // A **narrowing**: fewer members than `Number` and the same grants. That is its
    // whole use -- `fn T mean<T: Float>(xs: []T)` works for floats and is refused for
    // an integer, which is exactly what the declaration wants to say.
    {"Float", "isFloat", ConstraintClass::Float, LiteralClass::Float, kArithmetic},
    {"Pointer", "isPointer", ConstraintClass::Pointer, LiteralClass::None, kPointerGrants},
};

constexpr std::size_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);

// A class added to the enum without a row would silently admit nothing and grant
// nothing, which is the failure that looks like a working compiler.
static_assert(kRowCount == static_cast<std::size_t>(ConstraintClass::Pointer) + 1,
              "every ConstraintClass needs a row, in order");

constexpr const Row& rowOf(ConstraintClass klass) {
  return kRows[static_cast<std::size_t>(klass)];
}

// The names, in the table's order, so a sentence and a lookup read one list and
// cannot disagree about the set. `constexpr`, so this costs nothing at run time.
constexpr std::array<std::string_view, kRowCount> kClassNames = [] {
  std::array<std::string_view, kRowCount> out{};
  for (std::size_t i = 0; i < kRowCount; ++i) {
    out[i] = kRows[i].name;
  }
  return out;
}();

} // namespace

std::span<const std::string_view> constraintClassNames() {
  return kClassNames;
}

std::optional<ConstraintClass> constraintClassFromName(std::string_view name) {
  for (const Row& row : kRows) {
    if (row.name == name) {
      return row.klass;
    }
  }
  return std::nullopt;
}

std::string_view constraintClassName(ConstraintClass klass) {
  return rowOf(klass).name;
}

bool constraintGrants(ConstraintClass klass, Operation op) {
  return (rowOf(klass).grants & bit(op)) != 0;
}

ConstraintClass constraintForOperation(Operation op) {
  ConstraintClass best = ConstraintClass::Any;
  std::size_t fewest = 0;
  for (const Row& row : kRows) {
    // `Any` is not advice -- it grants nothing, so it is neither a candidate nor a
    // worst case to beat.
    if (row.klass == ConstraintClass::Any || (row.grants & bit(op)) == 0) {
      continue;
    }
    // How many operations the class promises, which is what "least powerful" means.
    // The tie is broken by the table's order, and the only tie that matters is
    // `Number` against `Float`: both grant the arithmetic, and `Number` wins because
    // it is the wider of the two -- telling a reader to write `Float` for a `+`
    // would narrow their declaration to a subset of what they asked for.
    const std::size_t size = static_cast<std::size_t>(std::popcount(row.grants));
    if (best == ConstraintClass::Any || size < fewest) {
      best = row.klass;
      fewest = size;
    }
  }
  return best;
}

LiteralClass constraintLiteralClass(ConstraintClass klass) {
  return rowOf(klass).literal;
}

bool literalAdmittedBy(LiteralClass admitted, bool isFloatLiteral) {
  // One equality per kind, and the `None` row fails both by construction rather than
  // by an extra test: a class that admits no literal admits neither spelling.
  return (admitted == LiteralClass::Integer && !isFloatLiteral) ||
         (admitted == LiteralClass::Float && isFloatLiteral);
}

// --- the constants ------------------------------------------------------------

std::span<const TypeConstantInfo> typeConstants() {
  return kTypeConstants;
}

std::optional<TypeConstant> typeConstantFromName(std::string_view name) {
  for (const TypeConstantInfo& row : kTypeConstants) {
    if (name == row.name) {
      return row.constant;
    }
  }
  return std::nullopt;
}

std::string_view typeConstantName(TypeConstant constant) {
  for (const TypeConstantInfo& row : kTypeConstants) {
    if (row.constant == constant) {
      return row.name;
    }
  }
  return {};
}

ConstraintClass constraintForConstant(TypeConstant constant) {
  switch (constant) {
  case TypeConstant::Zero:
  case TypeConstant::One:
  case TypeConstant::Min:
  case TypeConstant::Max:
    return ConstraintClass::Ordered;
  case TypeConstant::Epsilon:
  case TypeConstant::Infinity:
  case TypeConstant::Nan:
    return ConstraintClass::Float;
  }
  return ConstraintClass::Any;
}

std::optional<IntConstant> integerConstantValue(unsigned bits, bool isSigned,
                                                TypeConstant constant) {
  if (bits < 1 || bits > 128) {
    return std::nullopt;
  }
  IntConstant value;
  value.isUnsigned = !isSigned;
  switch (constant) {
  case TypeConstant::Zero:
    return value; // both halves already zero
  case TypeConstant::One:
    value.low = 1;
    return value;
  case TypeConstant::Min:
    if (!isSigned) {
      return value; // an unsigned type's smallest value is zero
    }
    // The sign bit alone is `-2^(bits-1)`, which is exactly the smallest value of
    // a two's complement type -- and the *bits*, so no negation is performed here
    // and the width is the type's.
    if (bits <= 64) {
      value.low = std::uint64_t{1} << (bits - 1);
    } else {
      value.high = std::uint64_t{1} << (bits - 65);
    }
    return value;
  case TypeConstant::Max:
    // Every bit but the sign bit, when signed; every bit, when not.
    if (bits <= 64) {
      value.low = bits == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1;
      if (isSigned) {
        value.low >>= 1;
      }
      return value;
    }
    value.low = ~std::uint64_t{0};
    value.high = bits == 128 ? ~std::uint64_t{0} : (std::uint64_t{1} << (bits - 64)) - 1;
    if (isSigned) {
      value.high >>= 1;
    }
    return value;
  case TypeConstant::Epsilon:
  case TypeConstant::Infinity:
  case TypeConstant::Nan:
    return std::nullopt;
  }
  return std::nullopt;
}

bool constraintGrantsConstant(ConstraintClass klass, TypeConstant constant) {
  // One sentence, applied: a class grants a constant when every member of the
  // class has it. The four every number has, and the three only a float has --
  // and `Float` is the only class whose members are all floats, so it is the only
  // one that grants the second group (`constraint_test.cc` holds the two sets
  // together, per class, over the type vocabulary).
  switch (constant) {
  case TypeConstant::Zero:
  case TypeConstant::One:
  case TypeConstant::Min:
  case TypeConstant::Max:
    return klass == ConstraintClass::Ordered || klass == ConstraintClass::Number ||
           klass == ConstraintClass::Integer || klass == ConstraintClass::Float;
  case TypeConstant::Epsilon:
  case TypeConstant::Infinity:
  case TypeConstant::Nan:
    return klass == ConstraintClass::Float;
  }
  return false;
}

} // namespace minc::support
