// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Expressions.
//
// Every kind of expression has one worker, and the workers follow three rules
// that are what make the answers trustworthy:
//
//   * a worker computes a type and the expression's facts, and **never writes**
//     to the artifact -- `checkExpr` is the single place a node's answer is
//     stored, so a node cannot end up with a type from one path and facts from
//     another;
//   * a worker that consumes its operand through `checkExpr` sees the operand's
//     *context-adapted* type, which is what makes `let x: u8 = 255;` a `u8` all
//     the way up instead of an `i32` that narrows at the last step;
//   * the poison spreads and never produces a second diagnostic: every worker
//     that sees an `Error` operand returns `Error` and says nothing.
#include "checker.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sema/convert.h"
#include "support/consteval/literal.h"

namespace minc::sema {
namespace {

// The name of an expression, for a message that has to name one. A path prints
// its spelling; anything else is described by its type, which is what the reader
// has in front of them.
[[nodiscard]] bool isComparison(Tag kind) {
  switch (kind) {
  case kTokLess:
  case kTokLessEqual:
  case kTokGreater:
  case kTokGreaterEqual:
  case kTokEqualEqual:
  case kTokBangEqual:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] bool isShift(Tag kind) {
  return kind == kTokLessLess || kind == kTokGreaterGreater;
}

// Whether an operand counts as an integer for an integer-only operator. A
// deferred literal does: `20 % 3` has to work before anything has decided those
// literals are `i32`, and the *float* literal does not, because `1.0 % 2` is a
// mistake wherever it appears. The distinction is the literal's class, not its
// decidedness.
[[nodiscard]] bool isIntegerOperand(const TypeStore& types, TypeId type) {
  if (types.isInteger(type)) {
    return true;
  }
  return types.get(type).kind == TypeKind::IntLiteral;
}

[[nodiscard]] bool isIntegerOnly(Tag kind) {
  switch (kind) {
  case kTokPercent:
  case kTokAmp:
  case kTokPipe:
  case kTokCaret:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] const char* opText(Tag kind) {
  switch (kind) {
  case kTokPercent:
    return "%";
  case kTokAmp:
    return "&";
  case kTokPipe:
    return "|";
  case kTokCaret:
    return "^";
  case kTokLessLess:
    return "<<";
  case kTokGreaterGreater:
    return ">>";
  case kTokAmpAmp:
    return "&&";
  case kTokPipePipe:
    return "||";
  case kTokEqualEqual:
    return "==";
  case kTokBangEqual:
    return "!=";
  case kTokLess:
    return "<";
  case kTokLessEqual:
    return "<=";
  case kTokGreater:
    return ">";
  case kTokGreaterEqual:
    return ">=";
  case kTokPlus:
    return "+";
  case kTokMinus:
    return "-";
  case kTokStar:
    return "*";
  case kTokSlash:
    return "/";
  default:
    return "operator";
  }
}

} // namespace

TypeId Checker::checkExpr(ast::AstId expr, TypeId expected) {
  if (!expr.valid()) {
    return kTypeError;
  }
  if (inError(expr)) {
    // The parser already reported this region, so the answer is the poison and
    // nothing is said about it.
    setType(expr, kTypeError);
    return kTypeError;
  }
  if (!enterDepth()) {
    setType(expr, kTypeError);
    return kTypeError;
  }

  ExprInfo info;
  TypeId type = kTypeError;
  switch (kindOf(expr)) {
  case ast::NodeKind::LiteralExpr:
    type = checkLiteral(expr, expected, info);
    break;
  case ast::NodeKind::PathExpr:
    type = checkPath(expr, info);
    break;
  case ast::NodeKind::ParenExpr: {
    const std::vector<ast::AstId> operands = operandsOf(expr);
    if (operands.empty()) {
      type = kTypeError;
      break;
    }
    // A parenthesised expression keeps its value category: `(x) = 1` is legal
    // for the same reason `x = 1` is, which is C's rule and the one every
    // reader expects.
    type = checkExpr(operands.front(), expected);
    info = out_.typed.infoOf(operands.front());
    break;
  }
  case ast::NodeKind::PrefixExpr:
    type = checkPrefix(expr, info);
    break;
  case ast::NodeKind::PostfixExpr:
    type = checkPostfix(expr, info);
    break;
  case ast::NodeKind::BinaryExpr:
    type = checkBinary(expr, info);
    break;
  case ast::NodeKind::ConditionalExpr:
    type = checkConditional(expr, expected, info);
    break;
  case ast::NodeKind::AssignExpr:
    type = checkAssign(expr, info);
    break;
  case ast::NodeKind::CallExpr:
    type = checkCall(expr, info);
    break;
  default:
    // A reserved kind (`MacroCall`, `TokenTree`, `Attribute`) the grammar does
    // not produce, or a token where an expression was expected. The poison and
    // no diagnostic: the parser owns "there is no expression here".
    type = kTypeError;
    break;
  }

  const TypeId adapted = adaptTo(type, expected, expr, info);
  setExpr(expr, info);
  setType(expr, adapted);
  --depth_;
  return adapted;
}

TypeId Checker::checkLiteral(ast::AstId expr, TypeId expected, ExprInfo& info) {
  const ast::AstId token = tokenOf(expr);
  if (!token.valid()) {
    return kTypeError;
  }
  const std::string_view text = spelling(token);
  switch (tagOf(kindOf(token))) {
  case kTokIntegerLiteral: {
    // `.mx` has no implicit octal: `0755` is decimal, and `0o755` is the octal
    // spelling (README, *Literals*).
    const support::IntegerLiteral parsed =
        support::parseIntegerLiteral(text, support::IntegerBaseRule::DecimalLeadingZero);
    info.isConstant = true;
    if (parsed.ok) {
      info.hasIntValue = true;
      info.value = parsed.value;
      return kTypeIntLiteral;
    }
    if (parsed.tooWide) {
      // Only a 128-bit type can hold it. `fitsIn` cannot decide this (the value
      // is wider than the core), so the decision is made here, against the type
      // the context asked for -- and a literal with no context is defaulted to
      // `i32` by `adaptTo`, which this must refuse before that happens.
      const Type& shape = types_.get(expected);
      const bool wideEnough = expected.valid() && shape.kind == TypeKind::Int && shape.bits > 64;
      if (wideEnough) {
        return kTypeIntLiteral;
      }
      error(expr, SemaErrorCode::LiteralOutOfRange,
            "this integer literal is too large for `" +
                (expected.valid() && !types_.isError(expected) ? types_.spelling(expected)
                                                               : std::string("i32")) +
                "`");
      return kTypeError;
    }
    // The lexer's own finding (a redundant leading zero, a digit out of base);
    // it was reported there, so nothing is said here.
    info.isConstant = false;
    return kTypeIntLiteral;
  }
  case kTokFloatLiteral:
    // A float literal has a *type* but deliberately no folded value: a
    // hand-rolled float parser whose rounding this stage cannot verify is a
    // correctness risk, and nothing sema checks needs the number.
    info.isConstant = true;
    return kTypeFloatLiteral;
  case kTokCharLiteral: {
    const support::IntegerLiteral parsed = support::parseCharLiteral(text);
    info.isConstant = true;
    if (parsed.ok) {
      info.hasIntValue = true;
      info.value = parsed.value;
    }
    return kTypeChar;
  }
  case kTokStringLiteral:
    info.isConstant = true;
    return kTypeStr;
  default:
    return kTypeError;
  }
}

TypeId Checker::checkPath(ast::AstId expr, ExprInfo& info) {
  const std::optional<resolve::DefId> def = defOfPath(expr);
  if (!def.has_value()) {
    // The resolver already reported the unresolved name; one name, one
    // diagnostic.
    return kTypeError;
  }
  const resolve::Def* declaration = defFor(*def);
  const TypeId type = typeOfDef(*def);
  if (declaration == nullptr) {
    return kTypeError;
  }
  const bool isFunction = types_.get(type).kind == TypeKind::Function;
  info.isLvalue = !isFunction && !types_.isError(type);
  if (isConstDef(*def) || declaration->predefined) {
    info.isConstant = true;
    if (def->index < defHasConstValue_.size() && defHasConstValue_[def->index]) {
      info.hasIntValue = true;
      info.value = defConstValues_[def->index];
    }
  }
  return type;
}

bool Checker::checkModifiable(ast::AstId operand, TypeId type, ast::AstId at, SemaErrorCode code,
                              std::string_view what) {
  const ExprInfo& info = out_.typed.infoOf(operand);
  if (info.isLvalue) {
    // Through parentheses, because `(c) = 1` is still an assignment to `c`, and
    // a `const` that could be written through a pair of brackets would not be a
    // `const` at all.
    const std::optional<resolve::DefId> def = defOfPlace(operand);
    if (def.has_value() && isConstDef(*def)) {
      const std::string name = nameOf(operand);
      error(at, SemaErrorCode::AssignToConst,
            (name == "this expression" ? std::string("this expression") : "`" + name + "`") +
                " is a `const` and cannot be assigned to");
      return false;
    }
    return true;
  }
  if (!types_.isError(type)) {
    error(at, code, "this expression is not a place a value can be stored" + std::string(what));
  }
  return false;
}

TypeId Checker::checkPrefix(ast::AstId expr, ExprInfo& info) {
  const ast::AstId op = tokenOf(expr);
  const std::vector<ast::AstId> operands = operandsOf(expr);
  if (!op.valid() || operands.empty()) {
    return kTypeError;
  }
  const Tag kind = tagOf(kindOf(op));
  const ast::AstId operand = operands.front();

  if (kind == kTokBang) {
    const TypeId inner = checkExpr(operand, kInvalidType);
    if (types_.isError(inner)) {
      return kTypeError;
    }
    if (types_.get(inner).kind != TypeKind::Bool) {
      error(operand, SemaErrorCode::ConditionNotBool,
            "`!` needs a `bool`; `" + types_.spelling(inner) + "` is not one");
      return kTypeBool;
    }
    const ExprInfo& innerInfo = out_.typed.infoOf(operand);
    info.isConstant = innerInfo.isConstant;
    if (innerInfo.hasIntValue) {
      info.hasIntValue = true;
      info.value = support::logicalNot(innerInfo.value);
    }
    return kTypeBool;
  }

  if (kind == kTokPlusPlus || kind == kTokMinusMinus) {
    const TypeId inner = checkExpr(operand, kInvalidType);
    if (types_.isError(inner)) {
      return kTypeError;
    }
    if (!checkModifiable(operand, inner, expr, SemaErrorCode::IncDecNotLvalue,
                         " and be incremented")) {
      return inner;
    }
    if (!types_.isArithmetic(inner)) {
      error(expr, SemaErrorCode::InvalidOperands,
            "`++` needs an arithmetic operand; `" + types_.spelling(inner) + "` is not one");
      return kTypeError;
    }
    // The result has the operand's own type, not its promoted one: `++c` on a
    // `char` is a `char` in C, and changing that would make `let y = ++c;` an
    // `i32` nobody asked for.
    return inner;
  }

  // `-`, `+`, `~`.
  const TypeId inner = checkExpr(operand, kInvalidType);
  if (types_.isError(inner)) {
    return kTypeError;
  }
  const ExprInfo& innerInfo = out_.typed.infoOf(operand);
  info.isConstant = innerInfo.isConstant;
  info.hasIntValue = innerInfo.hasIntValue;
  info.value = innerInfo.value;
  if (!innerInfo.hasIntValue) {
    info.value = support::ConstInt{};
  }

  // A deferred literal stays deferred, so `-8` in an `i8` context range-checks
  // as -8 and not as "8, then negated after narrowing".
  if (types_.isDeferred(inner)) {
    if (kind == kTokTilde) {
      if (types_.get(inner).kind == TypeKind::FloatLiteral) {
        error(expr, SemaErrorCode::InvalidOperands, "`~` needs an integer operand");
        return kTypeError;
      }
      if (innerInfo.hasIntValue) {
        info.value = support::bitNot(innerInfo.value);
      }
    } else if (kind == kTokMinus) {
      if (innerInfo.hasIntValue) {
        info.value = support::negate(innerInfo.value);
      }
    }
    return inner;
  }

  if (!types_.isArithmetic(inner)) {
    error(expr, SemaErrorCode::InvalidOperands,
          std::string("`") + opText(kind) + "` needs an arithmetic operand; `" +
              types_.spelling(inner) + "` is not one");
    return kTypeError;
  }
  if (kind == kTokTilde && !types_.isInteger(inner)) {
    error(expr, SemaErrorCode::InvalidOperands,
          "`~` needs an integer operand; `" + types_.spelling(inner) + "` is not one");
    return kTypeError;
  }
  const TypeId result = promote(types_, inner);
  if (innerInfo.hasIntValue) {
    info.value = kind == kTokMinus   ? support::negate(innerInfo.value)
                 : kind == kTokTilde ? support::bitNot(innerInfo.value)
                                     : innerInfo.value;
  }
  return result;
}

TypeId Checker::checkPostfix(ast::AstId expr, ExprInfo& info) {
  const ast::AstId op = tokenOf(expr);
  const std::vector<ast::AstId> operands = operandsOf(expr);
  if (!op.valid() || operands.empty()) {
    return kTypeError;
  }
  const ast::AstId operand = operands.front();
  const TypeId inner = checkExpr(operand, kInvalidType);
  if (types_.isError(inner)) {
    return kTypeError;
  }
  if (!checkModifiable(operand, inner, expr, SemaErrorCode::IncDecNotLvalue,
                       " and be incremented")) {
    return inner;
  }
  if (!types_.isArithmetic(inner)) {
    error(expr, SemaErrorCode::InvalidOperands,
          "`" + std::string(tagOf(kindOf(op)) == kTokPlusPlus ? "++" : "--") +
              "` needs an arithmetic operand; `" + types_.spelling(inner) + "` is not one");
    return kTypeError;
  }
  // The value is the *old* one, so it is not a constant even when the operand's
  // declaration is.
  (void)info;
  return inner;
}

bool Checker::foldBinary(ast::AstId opToken, Tag op, const ExprInfo& left, const ExprInfo& right,
                         ExprInfo& out) {
  if (!left.hasIntValue || !right.hasIntValue) {
    return true; // not foldable; nothing to say
  }
  switch (op) {
  case kTokPlus:
    out.value = support::add(left.value, right.value);
    break;
  case kTokMinus:
    out.value = support::sub(left.value, right.value);
    break;
  case kTokStar:
    out.value = support::mul(left.value, right.value);
    break;
  case kTokSlash: {
    const std::optional<support::ConstInt> quotient = support::divide(left.value, right.value);
    if (!quotient.has_value()) {
      error(opToken, SemaErrorCode::DivisionByZero, "division by zero");
      return false;
    }
    out.value = *quotient;
    break;
  }
  case kTokPercent: {
    const std::optional<support::ConstInt> rest = support::remainder(left.value, right.value);
    if (!rest.has_value()) {
      error(opToken, SemaErrorCode::DivisionByZero, "remainder by zero");
      return false;
    }
    out.value = *rest;
    break;
  }
  case kTokAmp:
    out.value = support::bitAnd(left.value, right.value);
    break;
  case kTokPipe:
    out.value = support::bitOr(left.value, right.value);
    break;
  case kTokCaret:
    out.value = support::bitXor(left.value, right.value);
    break;
  default:
    return true;
  }
  out.hasIntValue = true;
  return true;
}

TypeId Checker::checkBinary(ast::AstId expr, ExprInfo& info) {
  const ast::AstId op = tokenOf(expr);
  const std::vector<ast::AstId> operands = operandsOf(expr);
  if (!op.valid() || operands.size() < 2) {
    return kTypeError;
  }
  const Tag kind = tagOf(kindOf(op));
  const ast::AstId lhs = operands[0];
  const ast::AstId rhs = operands[1];
  const TypeId left = checkExpr(lhs, kInvalidType);
  const TypeId right = checkExpr(rhs, kInvalidType);
  if (types_.isError(left) || types_.isError(right)) {
    return kTypeError;
  }
  const ExprInfo& leftInfo = out_.typed.infoOf(lhs);
  const ExprInfo& rightInfo = out_.typed.infoOf(rhs);
  info.isConstant = leftInfo.isConstant && rightInfo.isConstant;

  if (kind == kTokAmpAmp || kind == kTokPipePipe) {
    // `&&`/`||` require `bool`, and their result is `bool`. C would accept any
    // scalar; that is the conversion this language rejects (`sema.md`).
    bool ok = true;
    if (types_.get(left).kind != TypeKind::Bool) {
      error(lhs, SemaErrorCode::ConditionNotBool,
            std::string("`") + opText(kind) + "` needs `bool` operands; `" + types_.spelling(left) +
                "` is not one");
      ok = false;
    }
    if (types_.get(right).kind != TypeKind::Bool) {
      error(rhs, SemaErrorCode::ConditionNotBool,
            std::string("`") + opText(kind) + "` needs `bool` operands; `" +
                types_.spelling(right) + "` is not one");
      ok = false;
    }
    if (!ok) {
      return kTypeBool;
    }
    if (leftInfo.hasIntValue && rightInfo.hasIntValue) {
      const bool bothTrue = leftInfo.value.truthy() && rightInfo.value.truthy();
      const bool eitherTrue = leftInfo.value.truthy() || rightInfo.value.truthy();
      info.value =
          support::ConstInt::fromSigned((kind == kTokAmpAmp ? bothTrue : eitherTrue) ? 1 : 0);
      info.hasIntValue = true;
    }
    return kTypeBool;
  }

  if (isComparison(kind)) {
    if (kind == kTokEqualEqual || kind == kTokBangEqual) {
      // Equality is defined for arithmetic values, and for two `bool`s or two
      // `str`s. `str == str` is refused with everything else: C's `s1 == s2`
      // compares addresses, and this operator does not mean that.
      const bool arithmetic = types_.isArithmetic(left) && types_.isArithmetic(right);
      const bool sameScalar = left == right && (types_.get(left).kind == TypeKind::Bool ||
                                                types_.get(left).kind == TypeKind::Str);
      if (!arithmetic && !sameScalar) {
        error(expr, SemaErrorCode::InvalidOperands,
              "`" + std::string(opText(kind)) + "` needs two arithmetic values, two `bool`s or " +
                  "two `str`s; got `" + types_.spelling(left) + "` and `" + types_.spelling(right) +
                  "`");
        return kTypeError;
      }
      if (arithmetic) {
        static_cast<void>(usualArithmetic(types_, left, right));
      }
      return kTypeBool;
    }
    if (!types_.isArithmetic(left) || !types_.isArithmetic(right)) {
      error(expr, SemaErrorCode::InvalidOperands,
            std::string("`") + opText(kind) + "` needs arithmetic operands; got `" +
                types_.spelling(left) + "` and `" + types_.spelling(right) + "`");
      return kTypeError;
    }
    static_cast<void>(usualArithmetic(types_, left, right));
    return kTypeBool;
  }

  if (!types_.isArithmetic(left) || !types_.isArithmetic(right)) {
    error(expr, SemaErrorCode::InvalidOperands,
          std::string("`") + opText(kind) + "` needs arithmetic operands; got `" +
              types_.spelling(left) + "` and `" + types_.spelling(right) + "`");
    return kTypeError;
  }
  if (isIntegerOnly(kind) &&
      (!isIntegerOperand(types_, left) || !isIntegerOperand(types_, right))) {
    error(expr, SemaErrorCode::InvalidOperands,
          std::string("`") + opText(kind) + "` needs integer operands; got `" +
              types_.spelling(left) + "` and `" + types_.spelling(right) + "`");
    return kTypeError;
  }
  if (isShift(kind)) {
    if (!isIntegerOperand(types_, left) || !isIntegerOperand(types_, right)) {
      error(expr, SemaErrorCode::InvalidOperands,
            std::string("`") + opText(kind) + "` needs integer operands; got `" +
                types_.spelling(left) + "` and `" + types_.spelling(right) + "`");
      return kTypeError;
    }
    // Folded only when the count is in range: an out-of-range shift is C's
    // undefined behaviour and the IR's to reason about, and there is no
    // diagnostic for it here.
    if (leftInfo.hasIntValue && rightInfo.hasIntValue) {
      const std::optional<support::ConstInt> shifted =
          kind == kTokLessLess ? support::shiftLeft(leftInfo.value, rightInfo.value)
                               : support::shiftRight(leftInfo.value, rightInfo.value);
      if (shifted.has_value()) {
        info.value = *shifted;
        info.hasIntValue = true;
      }
    }
    return promote(types_, left);
  }

  const TypeId result = usualArithmetic(types_, left, right);
  if (types_.isError(result)) {
    error(expr, SemaErrorCode::InvalidOperands,
          std::string("`") + opText(kind) + "` cannot combine `" + types_.spelling(left) +
              "` and `" + types_.spelling(right) + "`");
    return kTypeError;
  }
  if (!foldBinary(op, kind, leftInfo, rightInfo, info)) {
    return kTypeError;
  }
  return result;
}

TypeId Checker::checkConditional(ast::AstId expr, TypeId expected, ExprInfo& info) {
  const std::vector<ast::AstId> operands = operandsOf(expr);
  if (operands.size() < 3) {
    return kTypeError;
  }
  const ast::AstId condition = operands[0];
  const ast::AstId thenExpr = operands[1];
  const ast::AstId elseExpr = operands[2];

  const TypeId conditionType = checkExpr(condition, kInvalidType);
  if (!types_.isError(conditionType) && types_.get(conditionType).kind != TypeKind::Bool) {
    error(condition, SemaErrorCode::ConditionNotBool,
          "the condition of `?:` must be `bool`; `" + types_.spelling(conditionType) +
              "` is not one");
  }
  // Both arms are given the context's type, so a literal in either one is
  // range-checked where the reader wrote it.
  const TypeId thenType = checkExpr(thenExpr, expected);
  const TypeId elseType = checkExpr(elseExpr, expected);
  if (types_.isError(thenType) || types_.isError(elseType)) {
    return kTypeError;
  }

  TypeId result = kTypeError;
  if (thenType == elseType) {
    result = thenType;
  } else if (types_.isArithmetic(thenType) && types_.isArithmetic(elseType)) {
    result = usualArithmetic(types_, thenType, elseType);
  }
  if (!result.valid() || types_.isError(result)) {
    error(expr, SemaErrorCode::InvalidOperands,
          "the two branches of `?:` must have a common type; got `" + types_.spelling(thenType) +
              "` and `" + types_.spelling(elseType) + "`");
    return kTypeError;
  }

  const ExprInfo& conditionInfo = out_.typed.infoOf(condition);
  const ExprInfo& thenInfo = out_.typed.infoOf(thenExpr);
  const ExprInfo& elseInfo = out_.typed.infoOf(elseExpr);
  if (conditionInfo.isConstant && conditionInfo.hasIntValue) {
    // A constant condition makes the whole thing the arm that is taken, which is
    // what lets `true ? 1 : 2` fold.
    info = conditionInfo.value.truthy() ? thenInfo : elseInfo;
  } else {
    info.isLvalue = thenInfo.isLvalue && elseInfo.isLvalue && thenType == elseType;
  }
  return result;
}

TypeId Checker::checkAssign(ast::AstId expr, ExprInfo& info) {
  const ast::AstId op = tokenOf(expr);
  const std::vector<ast::AstId> operands = operandsOf(expr);
  if (!op.valid() || operands.size() < 2) {
    return kTypeError;
  }
  const Tag kind = tagOf(kindOf(op));
  const ast::AstId lhs = operands[0];
  const ast::AstId rhs = operands[1];

  const TypeId target = checkExpr(lhs, kInvalidType);
  const TypeId value = checkExpr(rhs, target);
  if (types_.isError(target) || types_.isError(value)) {
    return kTypeError;
  }
  if (!checkModifiable(lhs, target, expr, SemaErrorCode::InvalidAssignment,
                       " and be assigned to")) {
    return target;
  }

  if (kind != kTokEqual) {
    // `x op= y` needs the same operand types the binary operator does, and then
    // the result converts back to `x`'s type.
    if (!types_.isArithmetic(target) || !types_.isArithmetic(value)) {
      error(expr, SemaErrorCode::InvalidOperands,
            "this compound assignment needs arithmetic operands");
      return kTypeError;
    }
    const bool integerOnly = kind == kTokPercentEqual || kind == kTokAmpEqual ||
                             kind == kTokPipeEqual || kind == kTokCaretEqual ||
                             kind == kTokLessLessEqual || kind == kTokGreaterGreaterEqual;
    if (integerOnly && (!types_.isInteger(target) || !types_.isInteger(value))) {
      error(expr, SemaErrorCode::InvalidOperands,
            "this compound assignment needs integer operands");
      return kTypeError;
    }
  }

  checkAssignable(value, target, rhs, SemaErrorCode::InvalidAssignment,
                  kind == kTokEqual ? " in this assignment" : " in this compound assignment");
  // The value of an assignment is not a place, and never a constant.
  (void)info;
  return target;
}

TypeId Checker::checkCall(ast::AstId expr, ExprInfo& info) {
  const std::vector<ast::AstId> operands = operandsOf(expr);
  if (operands.empty()) {
    return kTypeError;
  }
  (void)info;
  const ast::AstId callee = operands.front();
  const TypeId calleeType = checkExpr(callee, kInvalidType);

  std::vector<ast::AstId> args;
  if (operands.size() > 1) {
    args = operandsOf(operands[1]);
  }

  if (types_.isError(calleeType)) {
    for (const ast::AstId arg : args) {
      (void)checkExpr(arg, kInvalidType);
    }
    return kTypeError;
  }
  if (types_.get(calleeType).kind != TypeKind::Function) {
    error(callee, SemaErrorCode::NotAFunction,
          "`" +
              std::string(kindOf(callee) == ast::NodeKind::PathExpr ? spelling(callee)
                                                                    : std::string_view("this")) +
              "` is not a function");
    for (const ast::AstId arg : args) {
      (void)checkExpr(arg, kInvalidType);
    }
    return kTypeError;
  }

  const std::span<const TypeId> params = types_.paramsOf(calleeType);
  if (args.size() != params.size()) {
    std::string message = "this function takes ";
    message += params.empty() ? "no arguments" : std::to_string(params.size()) + " argument(s)";
    message += ", but " + std::to_string(args.size()) + " were given";
    error(expr, SemaErrorCode::ArgumentCount, std::move(message));
    // The arguments are still checked: a wrong count must not hide a wrong
    // argument.
    for (std::size_t i = 0; i < args.size(); ++i) {
      (void)checkExpr(args[i], i < params.size() ? params[i] : kInvalidType);
    }
    return types_.get(calleeType).returnType;
  }

  for (std::size_t i = 0; i < args.size(); ++i) {
    const TypeId argumentType = checkExpr(args[i], params[i]);
    checkAssignable(argumentType, params[i], args[i], SemaErrorCode::InvalidAssignment,
                    " as this argument");
  }
  return types_.get(calleeType).returnType;
}

} // namespace minc::sema
