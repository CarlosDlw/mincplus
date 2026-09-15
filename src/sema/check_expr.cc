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

// Is a *constant* index inside `[0, count)`? The comparison is made in the
// index's own signedness, which is the whole point of keeping the bits and the
// signedness apart in `ConstInt`: `p[-1]` is below the range and `p[4294967295u]`
// is above it, while the same 32 bits read the other way would be one of each.
[[nodiscard]] bool indexInRange(support::ConstInt index, std::uint64_t count) {
  if (index.isUnsigned) {
    return index.bits < count;
  }
  const std::int64_t value = index.signedValue();
  return value >= 0 && static_cast<std::uint64_t>(value) < count;
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
    // reader expects. `(*p) = 1` and `(&x)` follow from the same sentence.
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
  case ast::NodeKind::IndexExpr:
    type = checkIndex(expr, info);
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
  case ast::NodeKind::ArrayLiteral:
    type = checkArrayLiteral(expr, expected, info);
    break;
  case ast::NodeKind::TypedInitializer:
    type = checkTypedInitializer(expr, info);
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
      //
      // `expected` is invalid when the literal is an operand of a binary
      // expression whose operation type has not been decided yet (the checker
      // decides the operands *after* the operation type), so the validity
      // check must come first: `types_.get(kInvalidType)` is a vector
      // out-of-bounds access.
      const bool wideEnough = expected.valid() && [&] {
        const Type& shape = types_.get(expected);
        return shape.kind == TypeKind::Int && shape.bits > 64;
      }();
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
  // A predefined name is a *value*: `true`, `false` and `null` denote no storage,
  // so `&null` is not an address and `null = p` is not a store. Asking the def
  // rather than the kind keeps that true for the next predefined name too.
  info.isLvalue =
      !isFunction && !resolve::isPredefined(declaration->predefined) && !types_.isError(type);
  if (isConstDef(*def) || resolve::isPredefined(declaration->predefined)) {
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

  // The two pointer operators come first because they are the two that *produce*
  // and *consume* a place, and everything below is about values.
  if (kind == kTokAmp) {
    return checkAddressOf(expr, info);
  }
  if (kind == kTokStar) {
    return checkDeref(expr, info);
  }

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
    // A pointer is stepped, an arithmetic value is incremented, and the two are
    // the same operator with the scaling belonging to the type.
    if (const std::optional<TypeId> stepped = checkPointerStep(expr, inner)) {
      return *stepped;
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
  // The operand is computed at the promoted type -- `-c` on a `char` is an `i32`
  // `sub` -- so the conversion is recorded like any other.
  recordOperationOperand(expr, 0, operand, result);
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
  if (const std::optional<TypeId> stepped = checkPointerStep(expr, inner)) {
    return *stepped;
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

// --- pointers ----------------------------------------------------------------

TypeId Checker::checkAddressOf(ast::AstId expr, ExprInfo& info) {
  const std::vector<ast::AstId> operands = operandsOf(expr);
  if (operands.empty()) {
    return kTypeError;
  }
  const ast::AstId operand = operands.front();
  const TypeId inner = checkExpr(operand, kInvalidType);
  if (types_.isError(inner)) {
    return kTypeError;
  }
  // `&` needs a *place*: an object that lives somewhere. `&(a + b)` has no
  // address, and neither has a literal or a call's result. C refuses all three,
  // and the reason is the model's: a pointer's provenance has to name an
  // allocation, so there has to *be* one (`memory.md`, *Provenance*).
  if (!out_.typed.infoOf(operand).isLvalue) {
    error(operand, SemaErrorCode::AddressOfNonLvalue,
          "`&` needs a place to take the address of; this expression is a value and has no "
          "address");
    return kTypeError;
  }
  // ... and a place that may be **written**, which is the second half of
  // `memory.md`'s rule (`&x` takes "the address of a modifiable lvalue"). A
  // pointer to a `const` binding would be a way to write it, and `const` is the
  // promise that the name cannot be written -- so the two cannot both hold. This
  // is *not* the same refusal as `&(a + b)`: the operand is a place, and what is
  // missing is permission.
  if (const std::optional<resolve::DefId> def = defOfPlace(operand);
      def.has_value() && isConstDef(*def)) {
    const std::string name = nameOf(operand);
    error(expr, SemaErrorCode::AddressOfConst,
          (name == "this expression" ? std::string("this expression") : "`" + name + "`") +
              " is a `const`, so its address cannot be taken: a pointer to it would be a way "
              "to write it");
    return kTypeError;
  }
  // Nothing is recorded for the operand: taking an address *reads nothing*, which
  // is why this is not an access and why the definite-assignment pass walks the
  // operand as a place. `&x` on an unassigned `x` is legal; reading `x` is not.
  const TypeId pointer = types_.pointerTo(inner);
  if (!pointer.valid()) {
    reportLimit(expr);
    return kTypeError;
  }
  (void)info;
  return pointer;
}

TypeId Checker::checkDeref(ast::AstId expr, ExprInfo& info) {
  const std::vector<ast::AstId> operands = operandsOf(expr);
  if (operands.empty()) {
    return kTypeError;
  }
  const ast::AstId operand = operands.front();
  const TypeId inner = checkExpr(operand, kInvalidType);
  if (types_.isError(inner)) {
    return kTypeError;
  }
  if (!types_.isPointer(inner)) {
    error(operand, SemaErrorCode::DerefNotPointer,
          "`*` needs a pointer operand; `" + types_.spelling(inner) + "` is not one");
    return kTypeError;
  }
  const TypeId pointee = types_.pointeeOf(inner);
  // A pointer to `void` is a pointer, and that is all it is: `void` has no size,
  // so there is nothing to load and nothing to align. `*i32` and `*void` differ
  // in exactly this -- the second one is an address to be converted, never one to
  // be used (`memory.md`, *Access*).
  if (types_.isVoid(pointee)) {
    error(expr, SemaErrorCode::PointerVoidAccess,
          "`*void` cannot be dereferenced: `void` has no size, so there is nothing to access");
    return kTypeError;
  }
  // The result is a place: `*p = v` and `(*p)++` are stores through a pointer,
  // and they are checked by the same modifiable-lvalue rules a name is.
  info.isLvalue = true;
  info.isConstant = false;
  info.hasIntValue = false;
  recordAccess(expr, pointee, provenanceOf(operand));
  return pointee;
}

TypeId Checker::checkIndex(ast::AstId expr, ExprInfo& info) {
  const std::vector<ast::AstId> operands = operandsOf(expr);
  if (operands.size() < 2) {
    return kTypeError;
  }
  const ast::AstId base = operands[0];
  const ast::AstId index = operands[1];
  const TypeId baseType = checkExpr(base, kInvalidType);
  const TypeId indexType = checkExpr(index, kInvalidType);
  if (types_.isError(baseType) || types_.isError(indexType)) {
    return kTypeError;
  }
  // `a[i]` on an **array**: an element of an object whose count is in its type.
  // This is the check C cannot make and this language can (`arrays.md` decision
  // 7), and it is the same node as `p[i]` on purpose -- one subscript, two
  // kinds of base, and neither decays into the other.
  if (types_.isArray(baseType)) {
    if (!isIntegerOperand(types_, indexType)) {
      error(index, SemaErrorCode::IndexNotInteger,
            "`[]` needs an integer index; `" + types_.spelling(indexType) + "` is not one");
      return kTypeError;
    }
    // The index is materialised at the pointer index width, exactly as it is for
    // `p[i]`: the `getelementptr` index has one width, and recording the
    // conversion here keeps the sign extension out of the lowering.
    (void)checkOperand(expr, 1, index, types_.signedInt(types_.target().pointerBits));

    const TypeId element = types_.elementOf(baseType);
    const std::uint64_t count = types_.countOf(baseType);
    // A constant index outside the object is a diagnostic, not a trap, and it
    // costs no analysis at all: the count is a number in the type and the index
    // is a number in the literal.
    const ExprInfo& indexInfo = out_.typed.infoOf(index);
    if (indexInfo.hasIntValue && !indexInRange(indexInfo.value, count)) {
      error(index, SemaErrorCode::IndexOutOfRange,
            "index " + valueText(indexInfo.value) + " is out of range for `" +
                types_.spelling(baseType) + "`: a constant index goes from 0 to " +
                std::to_string(count - 1));
      return kTypeError;
    }
    info.isLvalue = true;
    info.isConstant = false;
    info.hasIntValue = false;
    // The record carries the element type *and the count*, which is the extent
    // the checked build bounds-checks against when the base is an object this
    // unit named (26).
    recordAccess(expr, element, arrayProvenanceOf(base), count);
    return element;
  }
  if (!types_.isPointer(baseType)) {
    // C also accepts `i[p]`, because for C it is `*(i + p)` and addition is
    // commutative. It is a curiosity of C's definition and not of the operation,
    // and this language says so instead of accepting it: the reader who wrote it
    // meant `p[i]` with the operands the other way round.
    error(base, SemaErrorCode::DerefNotPointer,
          "`[]` needs an array or a pointer on the left and an integer index; `" +
              types_.spelling(baseType) + "` is not one");
    return kTypeError;
  }
  if (!isIntegerOperand(types_, indexType)) {
    error(index, SemaErrorCode::IndexNotInteger,
          "`[]` needs an integer index; `" + types_.spelling(indexType) + "` is not one");
    return kTypeError;
  }
  // The index is materialised at the **pointer index width**, which is what the
  // model's arithmetic is defined in and what the backend's `getelementptr`
  // index is. Recording the conversion here is the same rule the arithmetic
  // operators follow: a `u8` index is zero-extended, an `i8` one sign-extended,
  // and the lowering is told which rather than deriving it.
  (void)checkOperand(expr, 1, index, types_.signedInt(types_.target().pointerBits));

  const TypeId pointee = types_.pointeeOf(baseType);
  if (types_.isVoid(pointee)) {
    error(expr, SemaErrorCode::PointerVoidAccess,
          "`*void` cannot be indexed: `void` has no size, so there is nothing to access");
    return kTypeError;
  }
  info.isLvalue = true;
  info.isConstant = false;
  info.hasIntValue = false;
  recordAccess(expr, pointee, provenanceOf(base));
  return pointee;
}

std::optional<TypeId> Checker::checkPointerStep(ast::AstId expr, TypeId type) {
  if (!types_.isPointer(type)) {
    return std::nullopt;
  }
  if (types_.isVoidPointer(type)) {
    error(expr, SemaErrorCode::PointerVoidArithmetic,
          "`*void` cannot be stepped: `void` has no size, so there is no stride");
    return kTypeError;
  }
  // The value is the pointer either way -- before the step for a postfix form,
  // after it for a prefix one -- because a pointer step moves *within* an object
  // and cannot leave the type behind. No conversion and no access is recorded: a
  // step is arithmetic on the pointer, not a load or a store through it.
  return type;
}

std::optional<TypeId> Checker::checkPointerBinary(ast::AstId expr, Tag kind, ast::AstId lhs,
                                                  ast::AstId rhs, TypeId left, TypeId right,
                                                  ExprInfo& info) {
  const bool leftPointer = types_.isPointer(left);
  const bool rightPointer = types_.isPointer(right);
  if (!leftPointer && !rightPointer) {
    return std::nullopt;
  }
  (void)info;
  const TypeId isize = types_.signedInt(types_.target().pointerBits);

  const auto message = [&types = types_](TypeId a, TypeId b) {
    return "`" + types.spelling(a) + "` and `" + types.spelling(b) + "`";
  };

  // Comparison. Two pointers of the same pointee -- or either of them `*void` --
  // are compared by address, which the model defines for any two pointers, live
  // or not. A pointer and a non-pointer are not comparable: C's `p == 0` is the
  // spelling of the empty pointer, and this language has `null` for that.
  if (isComparison(kind)) {
    if (!leftPointer || !rightPointer) {
      error(expr, SemaErrorCode::PointerMismatch,
            message(left, right) +
                " cannot be compared: a pointer is compared with a pointer (use `null` for the "
                "empty one)");
      return kTypeError;
    }
    if (!convertible(types_, left, right) && !convertible(types_, right, left)) {
      error(expr, SemaErrorCode::PointerMismatch,
            message(left, right) +
                " cannot be compared: a pointer comparison needs one pointee type");
      return kTypeError;
    }
    // One common type for the pair, so a `*void` operand is materialised at the
    // pointer it is compared with. The conversion is a no-op in the IR -- both
    // map to one `ptr` -- and it is recorded anyway, because the record says
    // what the consumer *applies*, and a lowering that decided by itself which
    // operand needed which type would be re-deriving the rule.
    const TypeId common = types_.isVoidPointer(left) ? right : left;
    if (left != common) {
      recordOperationOperand(expr, 0, lhs, common);
    }
    if (right != common) {
      recordOperationOperand(expr, 1, rhs, common);
    }
    return kTypeBool;
  }

  const bool plus = kind == kTokPlus;
  const bool minus = kind == kTokMinus;
  if (!plus && !minus) {
    // `*`, `/`, `%`, `&`, `|`, `^`, `<<`, `>>`: a pointer is not an arithmetic
    // value, and the message the arithmetic path gives is the right one. Returning
    // `nullopt` hands the operator back to it rather than duplicating its rules.
    return std::nullopt;
  }

  if (leftPointer && rightPointer) {
    if (!minus) {
      error(expr, SemaErrorCode::PointerMismatch,
            "two pointers cannot be added; " + message(left, right) + " has no meaning");
      return kTypeError;
    }
    // `p - q`: the element count between them, at the pointer difference type.
    // The model defines it only when both are derived from one allocation -- the
    // checked build is what finds the other case -- and the type is `isize`
    // either way.
    if (!convertible(types_, left, right) && !convertible(types_, right, left)) {
      error(expr, SemaErrorCode::PointerMismatch,
            "`-` needs two pointers to the same type; got " + message(left, right));
      return kTypeError;
    }
    if (types_.isVoidPointer(left) || types_.isVoidPointer(right)) {
      error(expr, SemaErrorCode::PointerVoidArithmetic,
            "`*void` cannot be stepped: `void` has no size, so there is no element to count");
      return kTypeError;
    }
    return isize;
  }

  const ast::AstId indexSide = leftPointer ? rhs : lhs;
  const TypeId pointerType = leftPointer ? left : right;
  const TypeId indexType = leftPointer ? right : left;
  if (!isIntegerOperand(types_, indexType)) {
    error(indexSide, SemaErrorCode::InvalidOperands,
          std::string("`") + opText(kind) + "` with a pointer needs an integer offset; `" +
              types_.spelling(indexType) + "` is not one");
    return kTypeError;
  }
  if (types_.isVoidPointer(pointerType)) {
    error(expr, SemaErrorCode::PointerVoidArithmetic,
          std::string("`") + opText(kind) +
              "` cannot step a `*void`: `void` has no size, so there is no stride");
    return kTypeError;
  }
  // Scaling is the pointee's, and the arithmetic is in the pointer index width,
  // so the offset is converted to it -- the same conversion `p[i]` records, for
  // the same reason: `p + n` and `p[n]` are one operation and must not disagree.
  // The pointer operand keeps its type: `p + 1` is a pointer, not an integer.
  // The operand index is the source order of the two operands, which is what the
  // record is keyed on: for `n + p` the offset is operand 0, for `p + n` it is 1.
  (void)checkOperand(expr, static_cast<std::uint8_t>(leftPointer ? 1 : 0), indexSide, isize);
  return pointerType;
}

bool Checker::foldBinary(Tag op, const ExprInfo& left, const ExprInfo& right, ExprInfo& out) {
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
  // Division and remainder have no diagnostic here: `checkDivisor` owns it, and
  // owns it for the case this function cannot see. Folding needs *both* operands
  // constant, and `x / 0` is a mistake for exactly the same reason `1 / 0` is,
  // so a check here would answer only half the question -- and would then need a
  // second copy of the message at the compound-assignment path to cover the
  // other half.
  case kTokSlash: {
    const std::optional<support::ConstInt> quotient = support::divide(left.value, right.value);
    if (!quotient.has_value()) {
      return false;
    }
    out.value = *quotient;
    break;
  }
  case kTokPercent: {
    const std::optional<support::ConstInt> rest = support::remainder(left.value, right.value);
    if (!rest.has_value()) {
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

// --- array literals ----------------------------------------------------------

TypeId Checker::checkArrayLiteral(ast::AstId expr, TypeId expected, ExprInfo& info) {
  // `[1, 2, 3]`: the list form, whose type the **context** gives it -- the same
  // rule the number literals follow, and the reason a `let` can write a binding's
  // type on the left and the value on the right.
  //
  // Deliberately not a deferred *type* the way `IntLiteral` is. A number needs
  // one because it flows through operators that must find the common type of
  // operands nobody has typed yet; an array literal cannot be an operand of any
  // operator (`arrays.md` decision 25), so the only thing it can meet is a
  // consumer that already has the type -- and `expected` is exactly that
  // consumer's type, handed down by `checkOperand`. No second typing mechanism,
  // and no way for two answers to coexist.
  const std::vector<ast::AstId> operands = operandsOf(expr);
  const bool isFill = hasFillSeparator(expr);
  if (!types_.known(expected) || types_.isError(expected)) {
    // No context to decide it, and that is a refusal rather than a guess: the
    // count is part of the *type* of an object whose type this language writes
    // down, and an inferred one would be a number that appears nowhere in the
    // source (`arrays.md` decision 8).
    error(expr, SemaErrorCode::LiteralTypeUnknown,
          "the type of this array literal is not known here: annotate it (`let a: [3]i32 = "
          "[1, 2, 3];`) or name the type in the literal (`[_]i32{1, 2, 3}`)");
    info = ExprInfo{};
    return kTypeError;
  }
  if (!types_.isArray(expected)) {
    error(expr, SemaErrorCode::InitializerShape,
          "this is an array of elements and its place here is `" + types_.spelling(expected) +
              "`: an array does not convert to anything else");
    info = ExprInfo{};
    return kTypeError;
  }
  return checkElements(expr, operands, 0, isFill, expected, info);
}

TypeId Checker::checkTypedInitializer(ast::AstId expr, ExprInfo& info) {
  // `[3]i32{1, 2, 3}`, `[_]u8{...}`, `[64]u8{0; 64}`. The type is written, so
  // nothing is inferred and nothing is deferred: the element rules below are the
  // whole of it, and they are the five that C's zero-fill habit gets wrong (`arrays.md`
  // decision 9).
  const std::vector<ast::AstId> operands = operandsOf(expr);
  if (operands.empty()) {
    return kTypeError;
  }
  const ast::AstId typeNode = operands.front();
  if (kindOf(typeNode) != ast::NodeKind::Type) {
    // The grammar builds a typed initializer with a `Type` as its first operand,
    // and a tree that does not is a compiler bug and not a program: the shape
    // was checked by the builder, so this is a `return` and not a diagnostic.
    return kTypeError;
  }
  const std::vector<TypePart> parts = typeParts(typeNode);
  const bool isFill = hasFillSeparator(expr);
  const std::span<const ast::AstId> elements(operands.data() + 1, operands.size() - 1);
  if (elements.empty()) {
    // Before the type is read, and not after: for `[_]` the count *is* the number
    // of elements, so an empty list has no count to read -- and "the count is at
    // least 1" would be a sentence about the spelling the reader did write
    // instead of about the value they did not.
    errorEmptyInitializer(expr);
    return kTypeError;
  }

  // The count a `[_]` needs, and where it is legal. Only the outermost part is
  // ever `_`, and only a *list* can supply it: a fill writes its own count, and
  // two numbers for one thing is one number too many.
  const bool inferred = !parts.empty() && parts.front().isArray && parts.front().countInferred;
  std::optional<std::uint64_t> inferredCount;
  if (inferred) {
    if (isFill) {
      error(expr, SemaErrorCode::InitializerShape,
            "`_` takes the count from the elements that are written, so a filled "
            "initializer names the count: `[64]u8{0; 64}`");
      return kTypeError;
    }
    inferredCount = static_cast<std::uint64_t>(elements.size());
  }

  const TypeSpecResult spec = readType(parts, types_, inferredCount);
  if (!spec.ok) {
    error(typeNode,
          spec.unknownWord.empty() ? SemaErrorCode::MalformedType : SemaErrorCode::UnknownType,
          spec.message);
    setType(typeNode, kTypeError);
    return kTypeError;
  }
  if (!spec.type.valid()) {
    reportLimit(typeNode);
    setType(typeNode, kTypeError);
    return kTypeError;
  }
  setType(typeNode, spec.type);
  if (!types_.isArray(spec.type)) {
    // Unreachable, and kept as a `return` rather than a diagnostic for a reason
    // worth writing down: the parser recognizes a typed initializer only when the
    // type run starts with `[N]`, so the constructor that introduces this node is
    // the array's. The general `T{...}` -- and with it the sentence about braces
    // being for aggregates and not for scalars -- arrives with the first
    // non-array aggregate, which is where a *type name* followed by `{` stops
    // being ambiguous with a block (`arrays.md`).
    return kTypeError;
  }

  return checkElements(expr, elements, 1, isFill, spec.type, info);
}

bool Checker::hasFillSeparator(ast::AstId expr) const {
  // The `;` is the whole difference between a list and a fill, and it is read
  // from the tokens because an *operand* list cannot tell them apart: `{a, b}`
  // and `{a; b}` hold two operands each.
  for (const ast::AstId child : file_.childrenOf(expr)) {
    if (file_.at(child).isToken() && tagOf(kindOf(child)) == kTokSemicolon) {
      return true;
    }
  }
  return false;
}

void Checker::errorEmptyInitializer(ast::AstId consumer) {
  // One sentence, in one place, because both forms can write an empty group and
  // the repair is the same: an array's value is written out, and "many of the
  // same" is the fill.
  error(consumer, SemaErrorCode::InitializerShape,
        "this initializer has no elements: an array's value is written out, and an array of "
        "zeroes has one spelling -- the fill, `[N]T{v; N}`");
}

TypeId Checker::checkElements(ast::AstId consumer, std::span<const ast::AstId> elements,
                              std::uint8_t operandBase, bool isFill, TypeId arrayType,
                              ExprInfo& info) {
  // The whole of what a list of elements has to get right, for **both** literal
  // forms: the two differ only in where the type comes from, so sharing this is
  // what keeps `[3]i32{1, 2, 3}` and `let a: [3]i32 = [1, 2, 3];` from being two
  // sets of rules that can drift apart.
  if (elements.empty()) {
    errorEmptyInitializer(consumer);
    return kTypeError;
  }
  if (isFill && elements.size() < 2) {
    // `{v;}` -- a fill with no count. The parser's `expect` has already said what
    // is missing, and the node is still well formed enough to be read, so this
    // stops before a count that is not there rather than diagnosing twice.
    return kTypeError;
  }

  const TypeId elementType = types_.elementOf(arrayType);
  const std::uint64_t count = types_.countOf(arrayType);

  if (isFill) {
    // The value, checked against the element type, and the count, read by the
    // compiler. The count is *not* a stored value: the fill is a shape, and the
    // lowering writes it as a splat without the program ever holding the number
    // (`arrays.md` decision 15).
    const auto valueOperand = static_cast<std::uint8_t>(operandBase);
    const TypeId valueType = checkOperand(consumer, valueOperand, elements[0], elementType);
    checkAssignable(valueType, elementType, elements[0], SemaErrorCode::InvalidAssignment,
                    " in this initializer");
    const ast::AstId countNode = elements[1];
    (void)checkExpr(countNode, kInvalidType);
    const ExprInfo& countFacts = out_.typed.infoOf(countNode);
    if (!countFacts.isConstant || !countFacts.hasIntValue) {
      error(countNode, SemaErrorCode::InitializerShape,
            "the count of a filled initializer has to be a number this compiler can read, "
            "like `[64]u8{0; 64}`");
      return kTypeError;
    }
    if (static_cast<std::uint64_t>(countFacts.value.bits) != count) {
      // Two numbers for one thing, and this is the check that turns a
      // copy-and-paste off-by-one into a diagnostic instead of a silently short
      // or long object.
      error(countNode, SemaErrorCode::InitializerShape,
            "this fill writes " + valueText(countFacts.value) + " elements and the type has " +
                std::to_string(count) +
                ": an array's length is exact, and the two numbers have "
                "to be the same one");
      return kTypeError;
    }
    info = ExprInfo{};
    return arrayType;
  }

  if (elements.size() != count) {
    // Exact length, with the fix in the sentence. C's "fewer initializers means
    // zero-fill" is how an array ends up half written with no diagnostic at all,
    // and it is the one convention this language will not inherit.
    error(consumer, SemaErrorCode::InitializerShape,
          "this initializer writes " + std::to_string(elements.size()) + " elements and `" +
              types_.spelling(arrayType) + "` has " + std::to_string(count) +
              ": an array's length is exact -- write every element, or write a fill as in "
              "`[" +
              std::to_string(count) + "]" + std::string(types_.spelling(elementType)) + "{v; " +
              std::to_string(count) + "}`");
    return kTypeError;
  }

  // Every element, checked against the element type. A nested initializer is
  // checked by the same code one level down -- `checkOperand` hands its own
  // expected type down -- which is what makes ragged input impossible: a row is
  // an initializer of the row's type, and no initializer has a length of its own.
  bool allConstant = true;
  for (std::size_t i = 0; i < elements.size(); ++i) {
    const ast::AstId element = elements[i];
    const auto operand = static_cast<std::uint8_t>(operandBase + i);
    const TypeId written = checkOperand(consumer, operand, element, elementType);
    checkAssignable(written, elementType, element, SemaErrorCode::InvalidAssignment,
                    " in this initializer");
    if (!out_.typed.infoOf(element).isConstant) {
      allConstant = false;
    }
  }
  info = ExprInfo{};
  // Deliberately *not* `isConstant`, even when every element is one. An
  // aggregate's constant-ness is a property of its **shape** -- element type,
  // count, the list or the splat -- and that record is what the file-scope step
  // adds (`arrays.md` decision 15, step 8). Until it exists, a value that names no
  // value is exactly what the initializer-constant-expression walk must not be
  // handed: the walk reads `isConstant` and then asks for the value.
  static_cast<void>(allConstant);
  return arrayType;
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

  // Pointer operands before arithmetic ones: exactly one of the two can claim
  // the operator, and a pointer handed to `usualArithmetic` would be an error
  // with a message about arithmetic rather than one about pointers.
  if (const std::optional<TypeId> pointerResult =
          checkPointerBinary(expr, kind, lhs, rhs, left, right, info)) {
    return *pointerResult;
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
        // The two values are compared at their common type, and the pair is
        // recorded: `u8 == i32` is an `icmp` at `i32`, so the conversion is
        // required by the IR rather than an optimisation of it.
        const TypeId opType = usualArithmetic(types_, left, right);
        recordOperationOperand(expr, 0, lhs, opType);
        recordOperationOperand(expr, 1, rhs, opType);
      }
      return kTypeBool;
    }
    if (!types_.isArithmetic(left) || !types_.isArithmetic(right)) {
      error(expr, SemaErrorCode::InvalidOperands,
            std::string("`") + opText(kind) + "` needs arithmetic operands; got `" +
                types_.spelling(left) + "` and `" + types_.spelling(right) + "`");
      return kTypeError;
    }
    const TypeId opType = usualArithmetic(types_, left, right);
    recordOperationOperand(expr, 0, lhs, opType);
    recordOperationOperand(expr, 1, rhs, opType);
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
    // The operation is at the *promoted left operand*, so that is the width the
    // count is measured against -- and the width the instruction is performed
    // at, which is why both operands are recorded as converting to it.
    const TypeId opType = operationType(types_, /*shift=*/true, left, right);
    // The count has a range, and it is not the value's range: it is the width of
    // the value being moved. C leaves a count at or past that width -- and a
    // negative one -- undefined, and the backend inherits a poison value: a `>>`
    // that does not shift is a silent wrong answer, and a `<<` the optimizer is
    // free to invent is worse. So the language refuses it here, at the count,
    // which is the token the reader has to change. `x <<= n` asks the same
    // question through the same function.
    if (!checkShiftCount(rhs, opType)) {
      info.hasIntValue = false;
      info.isConstant = false;
      recordOperationOperand(expr, 0, lhs, opType);
      recordOperationOperand(expr, 1, rhs, opType);
      return opType;
    }
    if (leftInfo.hasIntValue && rightInfo.hasIntValue) {
      const std::optional<support::ConstInt> shifted =
          kind == kTokLessLess ? support::shiftLeft(leftInfo.value, rightInfo.value)
                               : support::shiftRight(leftInfo.value, rightInfo.value);
      if (shifted.has_value()) {
        info.value = *shifted;
        info.hasIntValue = true;
      }
    }
    recordOperationOperand(expr, 0, lhs, opType);
    recordOperationOperand(expr, 1, rhs, opType);
    return opType;
  }

  const TypeId result = usualArithmetic(types_, left, right);
  if (types_.isError(result)) {
    // An integer and a float are the one pair of arithmetic types with no common
    // type, and the sentence for them is the sentence an assignment uses: this is
    // the same rule, and a reader who meets it here and there should not have to
    // work out that it is.
    if (mixedNumberPair(types_, left, right)) {
      error(expr, SemaErrorCode::InvalidOperands,
            std::string("`") + opText(kind) + "` cannot combine `" + types_.spelling(left) +
                "` and `" + types_.spelling(right) + "`: " + mixingAdvice(left));
      return kTypeError;
    }
    error(expr, SemaErrorCode::InvalidOperands,
          std::string("`") + opText(kind) + "` cannot combine `" + types_.spelling(left) +
              "` and `" + types_.spelling(right) + "`");
    return kTypeError;
  }
  // The operands are computed at the common type -- `u8 + u8` is an `add i32` --
  // and each one's conversion to it is recorded, so the lowering never derives
  // the promotion rule a second time.
  recordOperationOperand(expr, 0, lhs, result);
  recordOperationOperand(expr, 1, rhs, result);
  // The divisor before the fold: `x / 0` has a constant divisor and a
  // non-constant other side, so `foldBinary` never runs for it and the mistake
  // would reach the backend instead of the reader.
  if (!checkDivisor(rhs, kind)) {
    return kTypeError;
  }
  if (!foldBinary(kind, leftInfo, rightInfo, info)) {
    return kTypeError;
  }
  // A constant that does not fit the type the operator computes in is a mistake
  // the compiler can see, and this is the half of that rule the context cannot
  // catch: both operands already have a real type, so nothing downstream will
  // re-range-check the result. `INT_MIN / -1` through a `const` and `MAX + 1`
  // through one are the shapes. The runtime operation is defined to wrap, but a
  // constant is not a runtime operation, and a value this stage hands on but
  // cannot represent is exactly the kind of gap the IR would inherit.
  if (info.hasIntValue && types_.isInteger(result) && !types_.isError(result) &&
      !fitsIn(types_, result, info.value)) {
    error(expr, SemaErrorCode::ConstantOutOfRange,
          "this constant expression evaluates to `" + valueText(info.value) +
              "`, which does not fit in `" + types_.spelling(result) + "`");
    info.hasIntValue = false;
    info.isConstant = false;
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
  } else if (types_.isNever(thenType) != types_.isNever(elseType)) {
    // One arm never produces a value, so the other arm is the only one that can:
    // the expression's type is that arm's, and the `!` arm converts into it like
    // any other conversion. This is the case a word beside the signature could
    // not do at all -- `c ? 1 : die()` is an `i32` because the branch that cannot
    // produce a value cannot be the branch that decides the type.
    //
    // Both arms `!` is the arm above, and it is right for the same reason: a
    // conditional whose every path never produces a value never produces one.
    //
    // The surviving arm is *decided* rather than merely taken, because `!`
    // converts into a concrete type and a deferred literal is not one: the
    // coercion the lowering reads is recorded at the type the other arm will
    // have, so a `1` here becomes an `i32` on the spot -- exactly what
    // `never`-fallback means in every language that has this type.
    result = decideAt(types_.isNever(thenType) ? elseExpr : thenExpr, expected);
  } else if (types_.isArithmetic(thenType) && types_.isArithmetic(elseType)) {
    result = usualArithmetic(types_, thenType, elseType);
  } else if (types_.isPointer(thenType) && types_.isPointer(elseType) &&
             (convertible(types_, thenType, elseType) || convertible(types_, elseType, thenType))) {
    // `c ? &x : null`. The arm that names a type wins, so the result is usable
    // where a pointer to that type is wanted -- C's rule for `void*`, and the
    // only reading that does not make the expression's type depend on the branch
    // taken. The other arm's conversion is recorded below like any other.
    result = types_.isVoidPointer(thenType) ? elseType : thenType;
  }
  if (!result.valid() || types_.isError(result)) {
    error(expr, SemaErrorCode::InvalidOperands,
          "the two branches of `?:` must have a common type; got `" + types_.spelling(thenType) +
              "` and `" + types_.spelling(elseType) + "`");
    return kTypeError;
  }

  // Both arms are materialised at the arms' common type. The condition is
  // operand 0 and does not convert: it is already a `bool`, which is the whole
  // rule `sema.md` states.
  recordOperationOperand(expr, 1, thenExpr, result);
  recordOperationOperand(expr, 2, elseExpr, result);

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
  // A plain `=` gives the right operand the target's type, which is the
  // assignment conversion and is recorded as one. A compound assignment does
  // **not**: `x += e` computes `x + e` at the *operation* type, so handing `e`
  // the target's type would range-check `x += 300` against a `u8` and refuse an
  // expression the language defines (the store truncates, and `-Wconversion` is
  // where that is reported).
  const bool compound = kind != kTokEqual;
  const TypeId value = compound ? checkExpr(rhs, kInvalidType) : checkOperand(expr, 1, rhs, target);
  if (types_.isError(target) || types_.isError(value)) {
    return kTypeError;
  }
  if (!checkModifiable(lhs, target, expr, SemaErrorCode::InvalidAssignment,
                       " and be assigned to")) {
    return target;
  }

  if (compound) {
    // A pointer step: `p += n` is `p = p + n`, with the same stride and the same
    // index conversion `p + n` records. It is handled here rather than falling
    // through, because the tail's assignment check would ask whether an integer
    // converts to the pointer's type -- which is the refusal `memory.md` is built
    // on, and which a step does not make (`p + n` is a pointer, and *that* is what
    // is stored).
    if (types_.isPointer(target)) {
      const bool step = kind == kTokPlusEqual || kind == kTokMinusEqual;
      if (!step) {
        error(expr, SemaErrorCode::InvalidOperands,
              "a pointer can only be stepped with `+=` or `-=`; `" + std::string(opText(kind)) +
                  "` is not one of them");
        return kTypeError;
      }
      if (types_.isVoidPointer(target)) {
        error(expr, SemaErrorCode::PointerVoidArithmetic,
              "`*void` cannot be stepped: `void` has no size, so there is no stride");
        return kTypeError;
      }
      if (!isIntegerOperand(types_, value)) {
        error(rhs, SemaErrorCode::InvalidOperands,
              std::string("`") + opText(kind) + "` needs an integer offset; `" +
                  types_.spelling(value) + "` is not one");
        return kTypeError;
      }
      const TypeId isize = types_.signedInt(types_.target().pointerBits);
      (void)checkOperand(expr, 1, rhs, isize);
      // The operation happens at the pointer's own type -- the stride comes from
      // the pointee and the store is a pointer store -- so `opType` names the
      // pointer, and the offset's conversion to the index width is the record.
      info.opType = target;
      info.isLvalue = false;
      info.isConstant = false;
      info.hasIntValue = false;
      return target;
    }
    // `x op= y` needs the same operand types the binary operator does, and then
    // the result converts back to `x`'s type.
    if (!types_.isArithmetic(target) || !types_.isArithmetic(value)) {
      error(expr, SemaErrorCode::InvalidOperands,
            "this compound assignment needs arithmetic operands");
      return kTypeError;
    }
    const bool shift = kind == kTokLessLessEqual || kind == kTokGreaterGreaterEqual;
    const bool integerOnly = kind == kTokPercentEqual || kind == kTokAmpEqual ||
                             kind == kTokPipeEqual || kind == kTokCaretEqual || shift;
    if (integerOnly && (!isIntegerOperand(types_, target) || !isIntegerOperand(types_, value))) {
      error(expr, SemaErrorCode::InvalidOperands,
            "this compound assignment needs integer operands");
      return kTypeError;
    }
    // The type the operation happens at. It is not the assignment's type, and it
    // is nowhere else in the tree: `x <<= 9` on a `u16` is defined at `i32`,
    // which is why the count `9` is legal -- and a lowering that read the
    // `AssignExpr`'s own type would emit an out-of-range shift, a poison value
    // rather than a crash. `info.opType` is therefore the fact the IR reads for
    // both the operation and the conversion of its result back into the target.
    const TypeId opType = operationType(types_, shift, target, value);
    if (types_.isError(opType)) {
      error(expr, SemaErrorCode::InvalidOperands,
            "this compound assignment cannot combine `" + types_.spelling(target) + "` and `" +
                types_.spelling(value) + "`");
      return kTypeError;
    }
    if (shift) {
      (void)checkShiftCount(rhs, opType);
    }
    // `x /= 0` asks the divisor question through the same function the binary
    // form does, so the two spellings cannot come to different answers.
    if (kind == kTokSlashEqual || kind == kTokPercentEqual) {
      (void)checkDivisor(rhs, kind);
    }
    info.opType = opType;
    recordOperationOperand(expr, 0, lhs, opType);
    recordOperationOperand(expr, 1, rhs, opType);
  }

  checkAssignable(value, target, rhs, SemaErrorCode::InvalidAssignment,
                  compound ? " in this compound assignment" : " in this assignment");
  // The value of an assignment is not a place, and never a constant.
  info.isLvalue = false;
  info.isConstant = false;
  info.hasIntValue = false;
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
  // A variadic function accepts *at least* its declared parameters; everything
  // past them is an un-specified argument. A non-variadic one takes exactly
  // them, which is what makes the two counts one condition instead of two.
  const bool variadic = types_.isVariadic(calleeType);
  const bool countOk = variadic ? args.size() >= params.size() : args.size() == params.size();
  if (!countOk) {
    std::string message = "this function takes ";
    if (variadic) {
      // "at least", because the sentence is about a *minimum* here: telling the
      // reader the count is wrong when they passed too few is the whole point.
      message += "at least " + std::to_string(params.size()) + " argument(s)";
    } else {
      message += params.empty() ? "no arguments" : std::to_string(params.size()) + " argument(s)";
    }
    message += ", but " + std::to_string(args.size()) + " were given";
    error(expr, SemaErrorCode::ArgumentCount, std::move(message));
    // The arguments are still checked: a wrong count must not hide a wrong
    // argument. The conversions of the ones that have a parameter are recorded
    // even so; the extra ones have nowhere to land and are typed for their own
    // diagnostics' sake.
    for (std::size_t i = 0; i < args.size(); ++i) {
      if (i < params.size()) {
        (void)checkOperand(expr, static_cast<std::uint8_t>(i + 1), args[i], params[i]);
      } else {
        (void)checkExpr(args[i], kInvalidType);
      }
    }
    return types_.get(calleeType).returnType;
  }

  // The callee is operand 0, so an argument's operand index is its position plus
  // one: the conversion of argument `i` to parameter `i` is the pair the callee's
  // own parameter list would have to be read again to recover.
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (i >= params.size()) {
      // Past the declared parameters there is nothing to check *against*: the
      // argument is what it is, and the only rule left is the promotion the ABI
      // applies on the way out.
      (void)checkVariadicArgument(expr, static_cast<std::uint8_t>(i + 1), args[i]);
      continue;
    }
    const TypeId argumentType =
        checkOperand(expr, static_cast<std::uint8_t>(i + 1), args[i], params[i]);
    checkAssignable(argumentType, params[i], args[i], SemaErrorCode::InvalidAssignment,
                    " as this argument");
  }
  return types_.get(calleeType).returnType;
}

} // namespace minc::sema
