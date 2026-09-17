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

void Checker::recordCast(ast::AstId consumer, std::uint8_t operand, ast::AstId node, TypeId from,
                         TypeId to) {
  if (!consumer.valid() || !node.valid() || !from.valid() || !to.valid()) {
    return;
  }
  // `recordConversion`'s three conditions, minus the one this exists for. A
  // *cast* is the source's statement about a pair, so `convertible` -- the rule
  // for conversions the language performs by itself -- is deliberately not asked:
  // `1 as f64` and `p as usize` are exactly the pairs it refuses, and refusing to
  // publish them is what would leave the lowering with no conversion to
  // materialise (`casts.md`, decision 1).
  if (types_.isError(from) || types_.isError(to)) {
    return;
  }
  if (types_.isDeferred(from) || types_.isDeferred(to)) {
    return;
  }
  if (from == to) {
    // A cast that changes nothing is not a conversion. Decision 14: the record
    // says "this value is already this type", and the lowering emits nothing for
    // it -- which is what keeps an explicit cast free.
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
  // A **binder** as the context, which is the one context the *class* decides
  // rather than the literal's own class (`generics.md`, decisions 7 and 11). The
  // body of a generic is checked once, so `let x: T = 1;` has to mean one thing for
  // every type the class admits -- and which thing follows from the class: `Integer`
  // admits an integer literal, `Float` a float one, and `Number` admits neither,
  // because for one instantiation the body would have to mean `1.0`.
  //
  // When the class admits it, the literal is decided **as the binder**, which is what
  // makes the value right per instance: the text is one `1` everywhere, and the type
  // travels through the substitution boundary like every other type of the node.
  if (types_.isParam(decided)) {
    const bool floatLiteral = types_.get(current).kind == TypeKind::FloatLiteral;
    const support::LiteralClass admitted = support::constraintLiteralClass(classOfBinder(decided));
    if (support::literalAdmittedBy(admitted, floatLiteral)) {
      setType(node, decided);
      return decided;
    }
    // The class does not admit this literal, so the language's default is **not**
    // taken here: defaulting would turn the reader's `1` into an `i32` and the
    // sentence that follows would be about a type nobody wrote. Left deferred, the
    // assignability check says what is wrong with the literal itself -- and the unit
    // is refused either way, so no stage below ever sees a deferred type.
    return current;
  }

  TypeId chosen = kInvalidType;
  if (decided.valid() && !types_.isError(decided) && !types_.isDeferred(decided)) {
    const bool floatLiteral = types_.get(current).kind == TypeKind::FloatLiteral;
    // A deferred literal's *class* is the **spelling's** and not the hint's: an
    // integer literal becomes an integer and a float literal becomes a float. The
    // hint still decides *which* type of that class -- `let x: u8 = 255;` is a
    // `u8`, and that is what range-checks a literal against the context it was
    // written in instead of defaulting it and converting it afterwards.
    //
    // A hint of the other class is not a decision at all. `let x: f64 = 1;` has
    // no type in the `f64` family to give the `1`, so it keeps the language's
    // default for its own class (`i32`) and the *consumer* reports that an
    // integer cannot be a float -- which is the refusal the language wants.
    // Adopting the `f64` here would have made the literal a float, left the
    // checker with two equal types and nothing to say, and handed the lowering a
    // value it would have to round from the digits, which is a rule no stage below
    // owns (`ir.md`).
    const bool compatible = floatLiteral ? types_.isFloat(decided) : types_.isInteger(decided);
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

TypeId Checker::checkVariadicArgument(ast::AstId consumer, std::uint8_t operand, ast::AstId child) {
  // No parameter to check against: the argument is checked by itself, and the
  // only rule left is what the ABI does with it.
  (void)checkExpr(child, kInvalidType);
  // `decideAt` with nothing to decide by is not a shortcut: a deferred literal
  // that reaches a variadic argument has exactly one meaning (`1` is an `i32`,
  // `1.0` is an `f64`), which is `defaultOf`, and that is the same answer the
  // language gives anywhere else nothing decides the type.
  const TypeId from = decideAt(child, kInvalidType);
  // An aggregate has no default promotion and no slot to arrive in: C's variadic
  // calling convention is built from values that fit one register or one pair,
  // and a product or an array is neither. Refused by name, and the fix is a
  // pointer -- the same answer the `extern` boundary gives a signature
  // (`tuples.md`, decision 18).
  if (types_.isAggregate(from)) {
    error(child, SemaErrorCode::VariadicAggregate,
          "`" + types_.spelling(from) +
              "` cannot be a variadic argument: the promotions a variadic call applies are for "
              "values that fit a register. Pass `&" +
              types_.spelling(from) + "`, or the members one by one");
    return from;
  }
  const TypeId to = promotedArgument(from);
  // A conversion that changes nothing is not recorded, and for most arguments
  // the promotion *is* nothing: `i32`, `i64`, a pointer and a `str` pass as they
  // are. The record exists for the ones that move -- `i8` to `i32`, `f32` to
  // `f64` -- which is exactly the pair the lowering has to emit an instruction
  // for.
  recordConversion(consumer, operand, child, from, to);
  return from;
}

// The default argument promotions (C 6.5.2.2), which is the rule a variadic
// call's un-specified arguments obey. It is not a language conversion the reader
// can rely on in an expression -- it exists only at the boundary, and only here.
TypeId Checker::promotedArgument(TypeId type) const {
  if (!type.valid() || types_.isError(type) || types_.isDeferred(type)) {
    return type;
  }
  const Type& declared = types_.get(type);
  switch (declared.kind) {
  case TypeKind::Bool:
  case TypeKind::Char:
    // Both are narrower than `int` and both fit in one, so both become `i32`.
    return kTypeI32;
  case TypeKind::Int:
    // `u8`/`u16` promote to `i32` too: `int` holds every value they have, which
    // is why C's rule is about rank and not about signedness.
    return declared.bits < 32 ? kTypeI32 : type;
  case TypeKind::Float:
    // `float` to `double`, and `f80` stays: `long double` is passed as itself.
    return declared.bits < 64 ? kTypeF64 : type;
  default:
    // Pointers, `str`, and everything else the language can pass are already the
    // shape the ABI wants.
    return type;
  }
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

// --- divisors ----------------------------------------------------------------

bool Checker::checkDivisor(ast::AstId divisorExpr, Tag op) {
  // The question is asked of every arithmetic operator -- the call site is one
  // line and a guard at each of them is a guard that can be forgotten -- so the
  // refusal to answer for an operator with no divisor is here. `300 * 0` is not a
  // division, and a check that said it was would be worse than no check.
  const bool remainder = op == kTokPercent || op == kTokPercentEqual;
  const bool division = op == kTokSlash || op == kTokSlashEqual;
  if (!division && !remainder) {
    return true;
  }
  const ExprInfo& facts = out_.typed.infoOf(divisorExpr);
  if (!facts.hasIntValue) {
    // A divisor the compiler cannot fold is the runtime trap the lowering
    // emits (`ir.md`, *The runtime contract*); there is nothing to say here.
    return true;
  }
  if (!support::isZero(facts.value)) {
    return true;
  }
  // `hasIntValue` is only ever set on an integer, so `0.0` never reaches this
  // arm: a float division by zero is the IEEE answer, not a mistake.
  error(divisorExpr, SemaErrorCode::DivisionByZero,
        remainder ? "remainder by zero" : "division by zero");
  return false;
}

} // namespace minc::sema
