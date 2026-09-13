// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Conversions, recorded where they apply, and the deferred types decided.
//
// The checker's typing walk answers "what type does this have". This file
// answers the second question the IR needs -- "what conversion happens between
// an operand and its consumer" -- and it answers it *where the checker already
// is*, not by re-deriving it later (`ir.md`, *The fourth fact nobody recorded*,
// and the reason is not style: the operation type of `x <<= 9` on a `u16` is
// `i32`, and the tree names only `u16`, so a later stage re-deriving the
// promotion holds a second copy of `convert.h` and a place for the two to
// disagree).
//
// Three rules hold this together, and each is a property a test asserts:
//
//   * **Every conversion in the record is a pair of real types.** A deferred
//     literal is decided before its pair is stored, so `from` always equals
//     `typeOf(node)` and both are types the IR can map. No consumer has to
//     handle "untyped".
//   * **A conversion that changes nothing is not recorded.** `from == to` is
//     the absence of a conversion, and recording it would make the list longer
//     without saying anything.
//   * **A poisoned pair is not recorded.** A conversion whose operand or target
//     is `Error` describes a program that was already refused, and the tree is
//     never lowered.
#include "checker.h"

#include <cstddef>
#include <cstdint>
#include <string>

#include "sema/convert.h"

namespace minc::sema {

// --- the record --------------------------------------------------------------

void Checker::recordConversion(ast::AstId consumer, std::uint8_t operand, ast::AstId node,
                               TypeId from, TypeId to) {
  if (!consumer.valid() || !node.valid() || !from.valid() || !to.valid()) {
    return;
  }
  if (types_.isError(from) || types_.isError(to)) {
    return;
  }
  // Not reachable by construction (`decideAt` and `checkExpr` decide before
  // this), and left as a silent drop rather than an assertion because the cost
  // of being wrong is a missing conversion the IR will refuse loudly through the
  // verifier -- not a wrong one.
  if (types_.isDeferred(from) || types_.isDeferred(to)) {
    return;
  }
  if (from == to) {
    return;
  }
  if (!convertible(types_, from, to)) {
    // A pair the language does not permit is an error the checker already
    // reported; recording it would describe a conversion legal programs cannot
    // have.
    return;
  }
  out_.typed.addCoercion(Coercion{consumer, operand, node, from, to});
}

TypeId Checker::decideAt(ast::AstId node, TypeId decided) {
  if (!node.valid()) {
    return kTypeError;
  }
  const TypeId current = out_.typed.typeOf(node);
  if (!types_.isDeferred(current)) {
    // Concrete already: nothing to decide, and the answer is the node's own
    // type, not the hint.
    return current;
  }
  TypeId chosen = kInvalidType;
  if (decided.valid() && !types_.isError(decided) && !types_.isDeferred(decided)) {
    const bool floatLiteral = types_.get(current).kind == TypeKind::FloatLiteral;
    // A deferred literal's *class* is what the hint has to respect: an integer
    // literal becomes an integer or a float, and a float literal only becomes a
    // float. `let x: i32 = 1.0;` therefore keeps the `1.0` a float and the
    // consumer converts it, instead of silently re-reading it as an integer.
    const bool compatible = floatLiteral ? types_.isFloat(decided) : types_.isArithmetic(decided);
    if (compatible) {
      chosen = decided;
    }
  }
  if (!chosen.valid()) {
    // Nothing decided it: the language's default is what `let x = 1;` means.
    chosen = types_.defaultOf(current);
  }
  setType(node, chosen);
  return chosen;
}

TypeId Checker::checkOperand(ast::AstId consumer, std::uint8_t operand, ast::AstId child,
                             TypeId expected) {
  // `checkExpr` gives a deferred literal the context's type when it can, which
  // is what makes `let x: u8 = 255;` a `u8` all the way up, and writes it into
  // the artifact. What it cannot do is decide a literal whose *class* the context
  // cannot hold (`let x: i32 = 1.0;` keeps the float), and that is the only case
  // `decideAt` is still needed for -- it falls back to the literal's default and
  // the consumer records the conversion between the two.
  (void)checkExpr(child, expected);
  const TypeId from = decideAt(child, expected);
  recordConversion(consumer, operand, child, from, expected);
  return from;
}

void Checker::recordOperationOperand(ast::AstId consumer, std::uint8_t operand, ast::AstId node,
                                     TypeId opType) {
  // A deferred operation type (`1 + 2`, nothing constraining it) is decided by
  // the parent that consumes the operation, not here: deciding it now would pick
  // `i32` for `1 + 2` inside an `i64` binding, and the operands would disagree
  // with the operation they feed.
  if (!opType.valid() || types_.isDeferred(opType) || types_.isError(opType)) {
    return;
  }
  const TypeId from = decideAt(node, opType);
  recordConversion(consumer, operand, node, from, opType);
}

// --- the sweep ---------------------------------------------------------------

bool Checker::fitsDecided(ast::AstId node, TypeId type, bool negated) {
  const ExprInfo& facts = out_.typed.infoOf(node);
  if (!facts.hasIntValue) {
    // A float literal has no folded value (`sema.md`), and a `hasIntValue` of
    // false means there is nothing to compare. A value this stage cannot fold is
    // a value whose narrowing the IR materialises, not one it can be wrong
    // about.
    return true;
  }
  // A unary minus in between is not decoration: `-128` is legal in an `i8` and
  // `128` is not, and both are the same literal with the same folded value.
  const support::ConstInt value = negated ? support::negate(facts.value) : facts.value;
  if (fitsIn(types_, type, value)) {
    return true;
  }
  // Two mistakes, two sentences, the same split `adaptTo` makes: a literal too
  // large for its type is one token the reader wrote, while a value that came out
  // of folding is a constant expression -- blaming a token the program never
  // wrote would be the wrong sentence.
  if (kindOf(node) == ast::NodeKind::LiteralExpr) {
    error(node, SemaErrorCode::LiteralOutOfRange,
          "this integer literal" + std::string(negated ? " (negated)" : "") + " does not fit in `" +
              types_.spelling(type) + "`");
  } else {
    error(node, SemaErrorCode::ConstantOutOfRange,
          "this constant expression evaluates to `" + valueText(value) +
              "`, which does not fit in `" + types_.spelling(type) + "`");
  }
  return false;
}

void Checker::decideSubtree(ast::AstId node, TypeId decided, bool decidedValid, bool negated) {
  if (!node.valid()) {
    return;
  }
  const TypeId own = out_.typed.typeOf(node);
  TypeId effective = decided;
  bool effectiveValid = decidedValid;
  if (own.valid() && !types_.isError(own) && !types_.isDeferred(own)) {
    // A node's own type is what its operands take after. For an arithmetic node
    // that is exactly its operation type; for a binding it is the type the value
    // is stored as.
    effective = own;
    effectiveValid = true;
  }
  if (types_.isDeferred(own)) {
    const TypeId chosen = decideAt(node, effective);
    // The hint was actually taken (and not the default) only when the decided
    // type came from a valid context and the node accepted it. Only then is the
    // node *in* that context and the README's range rule applicable.
    const bool inContext =
        effectiveValid && effective.valid() && chosen == effective && !types_.isError(chosen);
    effectiveValid = false;
    if (inContext && !fitsDecided(node, chosen, negated)) {
      setType(node, kTypeError);
    } else if (!types_.isError(chosen)) {
      effective = chosen;
      effectiveValid = true;
    }
  }
  // A unary minus flips which way the value of the operand reads, and it flips
  // back at the next one. Threading the parity is what lets `-128` be accepted
  // where `128` is refused without a second range check anywhere.
  const bool negating = kindOf(node) == ast::NodeKind::PrefixExpr && tokenOf(node).valid() &&
                        tagOf(kindOf(tokenOf(node))) == kTokMinus;
  for (const ast::AstId child : operandsOf(node)) {
    decideSubtree(child, effective, effectiveValid, negating ? !negated : negated);
  }
}

void Checker::decideDeferredTypes() {
  decideSubtree(file_.root(), kInvalidType, /*decidedValid=*/false, /*negated=*/false);
}

// --- shift counts ------------------------------------------------------------

bool Checker::checkShiftCount(ast::AstId countExpr, TypeId opType) {
  const ExprInfo& facts = out_.typed.infoOf(countExpr);
  if (!facts.hasIntValue) {
    // A count the compiler cannot fold is checked at run time by the guard the
    // lowering emits (`ir.md`, *The runtime contract*); there is nothing to say
    // here.
    return true;
  }
  // The width is the width of the value being moved. A deferred operation type
  // means nothing decided the value's width yet, and a bare integer is `i32` --
  // the same answer it would get anywhere else.
  const TypeId widthType =
      opType.valid() && types_.known(opType) && types_.get(opType).kind == TypeKind::Int ? opType
                                                                                         : kTypeI32;
  const std::uint16_t width = types_.get(widthType).bits;
  if (facts.value.negative()) {
    error(countExpr, SemaErrorCode::ShiftCountOutOfRange, "this shift count is negative");
    return false;
  }
  if (facts.value.bits >= width) {
    error(countExpr, SemaErrorCode::ShiftCountOutOfRange,
          "this shift count (" + std::to_string(facts.value.bits) +
              ") is not less than the width of `" + types_.spelling(widthType) + "` (" +
              std::to_string(width) + ")");
    return false;
  }
  return true;
}

} // namespace minc::sema
