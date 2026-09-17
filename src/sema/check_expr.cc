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

// The hex digits of a value with no leading zeros: what a diagnostic quotes when
// it names a code point (`\u{1F600}`) or the packed value of a multi-character
// constant (`0x6162`).
[[nodiscard]] std::string hexDigits(std::uint64_t value) {
  static constexpr char kDigits[] = "0123456789ABCDEF";
  std::string out;
  bool started = false;
  for (int shift = 60; shift >= 0; shift -= 4) {
    const auto digit = static_cast<unsigned>((value >> static_cast<unsigned>(shift)) & 0xFU);
    if (digit != 0 || started || shift == 0) {
      out.push_back(kDigits[digit]);
      started = true;
    }
  }
  return out;
}

// A constant as the *program* wrote it, for a message that has to name it: the
// decimal value signed as its own type reads it, exactly as a diagnostic would
// have to say it out loud.
[[nodiscard]] std::string constantSpelling(support::ConstInt value) {
  return value.isUnsigned ? std::to_string(value.bits) : std::to_string(value.signedValue());
}

// Why a character literal is not a `char`, in the language's own terms: a `char`
// is one byte (README, *Types*), so a literal of more than one code unit, or
// whose one unit is above a byte, is refused with the two spellings that *do*
// mean it (`literals.md`, decisions 21-23).
[[nodiscard]] std::string characterLiteralMessage(std::string_view text,
                                                  const support::CharLiteral& parsed) {
  const std::string body(text.substr(1, text.size() - 2));
  if (parsed.units != 1) {
    return "a `char` is one byte and this character literal is " + std::to_string(parsed.units) +
           " units: write it as a string (`\"" + body + "\"`), or the value as an integer (`0x" +
           hexDigits(parsed.value.bits) + "`)";
  }
  return "a `char` is one byte and this literal is above it: write the string (`\"\\u{" +
         hexDigits(parsed.value.bits) +
         "}\"`), which encodes it as UTF-8, or write a wider integer";
}

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

// The four comparisons that **order**, and the ones a written chain is always an
// error for. `==`/`!=` accept two `bool`s, so `(a < b) == c` is a legal expression
// and one reader's parenthesis away from the chain they meant; the ordering four
// take arithmetic operands, which a comparison's result never is, so a chain here
// is illegal whatever the operands are (`casts.md`, decision 20).
[[nodiscard]] bool isOrderingComparison(Tag kind) {
  switch (kind) {
  case kTokLess:
  case kTokLessEqual:
  case kTokGreater:
  case kTokGreaterEqual:
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

// Is a *constant bound* inside `[0, count]`? Inclusive, and that is the whole
// difference from `indexInRange`: the end of a view is one past the last element,
// so `a[0..4]` on a `[4]i32` is the whole object -- the same convention every
// language with slicing uses, chosen here because the alternative (`a[0..3]` as
// "the whole thing") makes the extent unreachable and the empty view unspellable
// (`slices.md`).
[[nodiscard]] bool boundInRange(support::ConstInt bound, std::uint64_t count) {
  if (bound.isUnsigned) {
    return bound.bits <= count;
  }
  const std::int64_t value = bound.signedValue();
  return value >= 0 && static_cast<std::uint64_t>(value) <= count;
}

// The magnitude of a constant bound, when it can be one at all: a *negative*
// bound is not "below zero and therefore backwards". For an array the range check
// above has already refused it, and for a slice or a pointer it is a real
// position relative to the view's start -- `p[-1..2]` is unchecked arithmetic the
// model permits (`memory.md`, *Access*) -- so the order question is only asked
// about the two numbers that are positions.
[[nodiscard]] std::optional<std::uint64_t> nonNegative(support::ConstInt value) {
  if (value.isUnsigned) {
    return value.bits;
  }
  if (value.signedValue() < 0) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(value.signedValue());
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

// The operation a **binary** operator performs, for the one question a class
// answers: may this body do this to a binder?
//
// Every binary operator is here, including the two no class grants (`&&`, `||`), so
// that "nothing grants it" is the table's answer rather than a case this file has to
// remember it is missing. The unary operators are not here and do not need to be:
// `-` is `Sub` as a binary operator and `Negate` as a unary one, and only the
// position knows which -- so the three unary sites name their operation themselves.
[[nodiscard]] support::Operation binaryOperationOf(Tag kind) {
  switch (kind) {
  case kTokPlus:
    return support::Operation::Add;
  case kTokMinus:
    return support::Operation::Sub;
  case kTokStar:
    return support::Operation::Mul;
  case kTokSlash:
    return support::Operation::Div;
  case kTokPercent:
    return support::Operation::Remainder;
  case kTokAmp:
    return support::Operation::BitAnd;
  case kTokPipe:
    return support::Operation::BitOr;
  case kTokCaret:
    return support::Operation::BitXor;
  case kTokLessLess:
    return support::Operation::ShiftLeft;
  case kTokGreaterGreater:
    return support::Operation::ShiftRight;
  case kTokLess:
    return support::Operation::Less;
  case kTokLessEqual:
    return support::Operation::LessEqual;
  case kTokGreater:
    return support::Operation::Greater;
  case kTokGreaterEqual:
    return support::Operation::GreaterEqual;
  case kTokEqualEqual:
    return support::Operation::Equal;
  case kTokBangEqual:
    return support::Operation::NotEqual;
  case kTokAmpAmp:
    return support::Operation::LogicalAnd;
  case kTokPipePipe:
    return support::Operation::LogicalOr;
  default:
    return support::Operation::Condition;
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
  case kTokTilde:
    return "~";
  case kTokBang:
    return "!";
  case kTokPlusPlus:
    return "++";
  case kTokMinusMinus:
    return "--";
  default:
    return "operator";
  }
}

// The operator as a diagnostic writes it, in backticks: `` `+` ``.
[[nodiscard]] std::string quotedOperator(Tag kind) {
  return "`" + std::string(opText(kind)) + "`";
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
  case ast::NodeKind::CastExpr:
    type = checkCast(expr, info);
    break;
  case ast::NodeKind::PrefixExpr:
    type = checkPrefix(expr, info);
    break;
  case ast::NodeKind::PostfixExpr:
    type = checkPostfix(expr, info);
    break;
  case ast::NodeKind::IndexExpr:
    type = checkIndex(expr, info);
    break;
  case ast::NodeKind::SliceExpr:
    type = checkSlice(expr, info);
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
    type = checkCall(expr, expected, info);
    break;
  case ast::NodeKind::TupleExpr:
    type = checkTupleExpr(expr, expected, info);
    break;
  case ast::NodeKind::FieldExpr:
    type = checkField(expr, info);
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

TypeId Checker::typeOfSuffix(const support::LiteralSuffix& suffix) {
  const TargetInfo& target = types_.target();
  switch (suffix.type) {
  case support::SuffixType::I8:
    return types_.signedInt(8);
  case support::SuffixType::I16:
    return types_.signedInt(16);
  case support::SuffixType::I32:
    return types_.signedInt(32);
  case support::SuffixType::I64:
    return types_.signedInt(64);
  case support::SuffixType::I128:
    return types_.signedInt(128);
  case support::SuffixType::Isize:
    return types_.signedInt(target.pointerBits);
  case support::SuffixType::U8:
    return types_.unsignedInt(8);
  case support::SuffixType::U16:
    return types_.unsignedInt(16);
  case support::SuffixType::U32:
    return types_.unsignedInt(32);
  case support::SuffixType::U64:
    return types_.unsignedInt(64);
  case support::SuffixType::U128:
    return types_.unsignedInt(128);
  case support::SuffixType::Usize:
    return types_.unsignedInt(target.pointerBits);
  case support::SuffixType::F32:
    return types_.floatOf(32);
  case support::SuffixType::F64:
    return types_.floatOf(64);
  case support::SuffixType::F80:
    return types_.floatOf(80);
  case support::SuffixType::F128:
    return types_.floatOf(128);
  // The four C spellings whose meaning is the *target's*. `10L` is `i64` on
  // Linux and `i32` on Windows, which is C's LP64/LLP64 story and the reason
  // `--target` exists (`casts.md`, *Cross-platform*).
  case support::SuffixType::CUnsignedInt:
    return types_.unsignedInt(target.intBits);
  case support::SuffixType::CLong:
    return types_.signedInt(target.longBits);
  case support::SuffixType::CUnsignedLong:
    return types_.unsignedInt(target.longBits);
  case support::SuffixType::CLongDouble:
    return types_.floatOf(target.longDoubleBits);
  case support::SuffixType::None:
    break;
  }
  return kTypeError;
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
    // A suffix **types** the literal where it is written, which is the whole
    // point of the third form of a cast: `let x = 10u8;` is a `u8` and not an
    // `i32` that happens to fit, while `let y = 10;` stays deferred. A suffix the
    // language knows and refuses (`10wb`) is named here, with the sentence the
    // table wrote.
    if (parsed.suffix.refused()) {
      error(expr, SemaErrorCode::CastInvalid, parsed.suffix.message);
      info.isConstant = false;
      return kTypeError;
    }
    if (parsed.suffix.typed()) {
      const TypeId named = typeOfSuffix(parsed.suffix);
      if (parsed.ok && !types_.isError(named) && !fitsIn(types_, named, parsed.value)) {
        // `300u8`: the same code and the same shape as `let x: u8 = 300;`,
        // because it is the same mistake -- a literal that does not fit the type
        // it was given, with the type now written in the literal itself.
        error(expr, SemaErrorCode::LiteralOutOfRange,
              "`" + std::string(text) + "` does not fit `" + types_.spelling(named) + "`");
        info.isConstant = false;
        return kTypeError;
      }
      if (parsed.ok) {
        info.hasIntValue = true;
        info.value = parsed.value;
      }
      return named;
    }
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
  case kTokFloatLiteral: {
    // A float literal has a *type* but deliberately no folded value: a
    // hand-rolled float parser whose rounding this stage cannot verify is a
    // correctness risk, and nothing sema checks needs the number.
    info.isConstant = true;
    const support::FloatLiteral parsed = support::readFloatLiteral(text);
    if (parsed.suffix.refused()) {
      error(expr, SemaErrorCode::CastInvalid, parsed.suffix.message);
      info.isConstant = false;
      return kTypeError;
    }
    if (parsed.suffix.typed()) {
      return typeOfSuffix(parsed.suffix);
    }
    return kTypeFloatLiteral;
  }
  case kTokCharLiteral: {
    const support::CharLiteral parsed = support::parseCharLiteral(text);
    // Three ways a character literal can already have been reported, and none of
    // them is repeated here: the body is empty (`lex-empty-char`), an escape is
    // unknown or malformed (`lex-unknown-escape`, `lex-escape-digits`,
    // `lex-escape-out-of-range`, `lex-named-escape`), or the literal never ended
    // (`lex-unterminated-char`). The scanner owns those sentences because it owns
    // the spelling; this stage owns the *type* rule below, which no earlier stage
    // can state. No value leaves here in either case, so the literal can never
    // reach the lowering as something that is not a byte.
    if (!parsed.ok || parsed.units == 0) {
      info.isConstant = false;
      return kTypeError;
    }
    // `char` is one byte, so a character literal is one code unit *and* that unit
    // fits a byte. The value half is the invariant rather than a rule the reader
    // enforces (`literals.md`, decision 23): the scanner flags an escape above a
    // byte, and this is what makes the crash that used to follow unreachable even
    // if a flag is ever missed.
    if (parsed.units != 1 || parsed.value.bits > 0xFFU) {
      error(expr, SemaErrorCode::LiteralOutOfRange, characterLiteralMessage(text, parsed));
      info.isConstant = false;
      return kTypeError;
    }
    info.isConstant = true;
    info.hasIntValue = true;
    info.value = parsed.value;
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
  if (declaration == nullptr) {
    return kTypeError;
  }
  // A builtin is an *operation* and not a value: `clz` in a place that wants a
  // value -- stored, passed, compared -- is refused here rather than being given
  // a function type it does not have. Only a call position is a use, and that
  // position is taken by `checkCall` before this function ever sees the name, so
  // reaching this point with a row means the program asked for the operation
  // itself. A row need not have an address at all: one lowered to an instruction
  // has no symbol behind it, which is why this is not a pointer to a function.
  if (declaration->builtin != builtins::BuiltinId::None) {
    const builtins::BuiltinInfo* row = builtins::lookup(declaration->builtin);
    const std::string name = row != nullptr ? std::string(row->spelling) : nameOf(expr);
    error(expr, SemaErrorCode::BuiltinNotAValue,
          "`" + name + "` is an operation, not a value: it can only be called");
    return kTypeError;
  }
  // A **generic name** is not a value: it names a family of functions and has no
  // type until an argument list says which one. It is refused here rather than
  // reaching a binder through the template's own signature, because what the
  // reader has is a *value* position and the fix is a spelling -- and the
  // sentence names it, so the reader does not have to know that a template holds
  // `Param`s (`generics.md`, § 9). `identity::<i32>` is refused by the parser
  // already: the `::` belongs to a call and nothing else.
  if (const auto generic = genericFunctionByDef_.find(def->index);
      generic != genericFunctionByDef_.end()) {
    const FunctionInfo& row = out_.typed.functionTable[generic->second];
    const std::string name =
        row.name == support::kInvalidSym ? nameOf(expr) : std::string(symbols_.lookup(row.name));
    error(expr, SemaErrorCode::GenericNameNotValue,
          "`" + name +
              "` is generic, so it is not a value: it has no type until its type "
              "arguments are written. `" +
              name + "::<i32>` is the function `i32` asks for");
    return kTypeError;
  }

  const TypeId type = typeOfDef(*def);

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

ast::AstId Checker::castOperand(ast::AstId expr) const {
  for (const ast::AstId child : operandsOf(expr)) {
    if (kindOf(child) != ast::NodeKind::Type) {
      return child;
    }
  }
  return ast::AstId{};
}

TypeId Checker::checkCast(ast::AstId expr, ExprInfo& info) {
  const ast::AstId typeNode = childOf(expr, ast::NodeKind::Type);
  const ast::AstId operand = castOperand(expr);
  if (!operand.valid()) {
    // `(i32)` with no operand: the parser only builds a cast when an expression
    // follows the `)`, so this is a tree with nothing to convert -- the refusal
    // the parser already gave is the one sentence for it.
    return kTypeError;
  }
  const TypeId to = typeNode.valid() ? resolveTypeNode(typeNode) : kTypeError;

  // The operand is typed **by itself**, with no expectation. A cast is exactly
  // where the context stops deciding -- that is what crossing a class means --
  // and passing `to` down would ask an integer literal to *be* a float, which
  // `decideAt` deliberately refuses (`let x: f64 = 1;`). `1 as f64` is therefore
  // an `i32` literal with one well-defined rounding on top of it, and
  // `1e30 as i32` is an `f64` with the guard every runtime conversion has.
  (void)checkExpr(operand, kInvalidType);
  const TypeId from = decideAt(operand, kInvalidType);
  const ExprInfo& operandFacts = out_.typed.infoOf(operand);

  const CastResult cast = castResult(types_, from, to);
  if (!cast.ok) {
    // An empty message means the operand was already reported (the poison): one
    // mistake, one diagnostic.
    if (!cast.message.empty()) {
      error(expr, SemaErrorCode::CastInvalid, cast.message);
    }
    info.isConstant = false;
    return kTypeError;
  }

  // **An address cannot come from a constant, not even zero.**
  // `with_exposed_provenance` is a named operation, and naming an address is an
  // assertion about *where the value came from*: an address is obtained -- from an
  // object (`&x`), from a pointer whose provenance was exposed, or from the system
  // (`mmap`, a table) -- and a constant is a number the program never obtained.
  // The null address is not an exception to that, because it has a spelling of its
  // own (`null`), and `0 as *T` is `inttoptr` of a zero: a pointer with permission
  // over *every* exposed allocation, which is the opposite of what a reader
  // writing `null` means (`casts.md`, decision 11b).
  //
  // This is `casts.md` decision 8's shape applied to the other join: the compiler
  // can *see* the value, so it refuses instead of emitting a program whose first
  // access crashes with no sentence anywhere -- which is what `let x = (str)1;
  // printf(x);` did. A value is never refused, however it was arrived at:
  // `memory.md`'s answer to "where did this come from" is to count the operation
  // (`-Wprovenance`), not to guess.
  if (cast.kind == CastKind::IntegerToPointer && operandFacts.isConstant &&
      operandFacts.hasIntValue) {
    const bool toIsStr = to == kTypeStr;
    error(expr, SemaErrorCode::AddressFromConstant,
          "an address cannot come from a constant: nothing in this program obtained " +
              constantSpelling(operandFacts.value) +
              ". An address comes from an object (`&x`), from a pointer that was exposed "
              "(`p as usize`, and that value cast back), or from the system; the null "
              "address is " +
              (toIsStr ? std::string("`null as str`, because a `str` is not a `*void`")
                       : std::string("`null`")));
    info.isConstant = false;
    return kTypeError;
  }

  // The pair is *published*, which is what makes the lowering materialise it
  // without knowing what a cast is (`casts.md`, decision 1).
  recordCast(expr, 0, operand, from, to);

  if (options_.warnCast && cast.loss != CastLoss::None) {
    warning(expr, SemaErrorCode::CastLoses,
            "this cast from `" + types_.spelling(from) + "` to `" + types_.spelling(to) +
                "` may lose something: " + lossPhrase(cast.loss));
  }

  // The two *named* operations of `memory.md`, counted where they are written.
  // The text uses the model's own words, so a reader who meets one here can read
  // the rule there (`casts.md`, *Pointer and integer*); the loss a cast reports is
  // a separate question and is not this warning's business.
  if (options_.warnProvenance &&
      (cast.kind == CastKind::PointerToInteger || cast.kind == CastKind::IntegerToPointer)) {
    warning(expr, SemaErrorCode::ProvenanceCast,
            cast.kind == CastKind::PointerToInteger
                ? "this cast is `expose`: the address in `" + types_.spelling(from) +
                      "` becomes an integer, and what it addresses afterwards is every "
                      "allocation whose provenance has been exposed"
                : "this cast is `with_exposed_provenance`: `" + types_.spelling(from) +
                      "` becomes an address, and an access through it is defined only for "
                      "an allocation whose provenance has been exposed");
  }

  // The facts. The value is the *operand's* converted, so the cast is constant
  // exactly when its operand is; and it keeps an integer value only where the
  // 64-bit core can hold the result, which is the same rule every other folded
  // value follows.
  info.isConstant = operandFacts.isConstant;
  info.hasIntValue = false;
  if (operandFacts.isConstant && operandFacts.hasIntValue) {
    const std::optional<support::ConstInt> folded =
        foldIntCast(types_, from, to, operandFacts.value);
    if (folded.has_value()) {
      info.hasIntValue = true;
      info.value = *folded;
    }
  }
  return to;
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
    if (refuseOperation(operand, inner, support::Operation::LogicalNot, "`!`")) {
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
    if (refuseOperation(operand, inner, support::Operation::Increment, "`++`")) {
      return kTypeError;
    }
    if (types_.isParam(inner)) {
      // A binder the class admitted, and the whole answer: the operation rules below
      // are about concrete types (`isArithmetic`, the pointer step), and the class is
      // the promise that every one of its members takes the step -- which is what the
      // grant just read. The place is checked like any other, because a class says
      // what may be done to a value and not whether the value is addressable.
      if (!checkModifiable(operand, inner, expr, SemaErrorCode::IncDecNotLvalue,
                           " and be incremented")) {
        return inner;
      }
      return inner;
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
  // `~` is the bitwise family and `-`/`+` are the sign, and the two are different
  // classes: `~` needs an integer and `-x` needs a number.
  if (refuseOperation(operand, inner,
                      kind == kTokTilde ? support::Operation::BitNot : support::Operation::Negate,
                      quotedOperator(kind))) {
    return kTypeError;
  }
  if (types_.isParam(inner)) {
    // Admitted by the class, so the result is the binder and not a promoted one: the
    // body is checked once, `-x` has to be usable as `T` (`let y: T = -x;`), and the
    // arithmetic below -- `promote`, the range check, the folded value -- is about the
    // *instance's* type, which is what the substitution boundary hands the lowering.
    // Nothing is recorded as an operand conversion: the operand is already the result.
    info.isLvalue = false;
    info.isConstant = false;
    info.hasIntValue = false;
    info.value = support::ConstInt{};
    return inner;
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
  // The grant first, so a binder gets the class's answer rather than the concrete
  // rule's: `x++` inside `fn T f<T: Number>` is granted for every member of `Number`,
  // and the rules below are about a type the body does not name. `++` and `--` are
  // one operation with two spellings (`support::Operation::Increment`).
  const std::string written =
      std::string("`") + (tagOf(kindOf(op)) == kTokPlusPlus ? "++" : "--") + "`";
  if (refuseOperation(operand, inner, support::Operation::Increment, written)) {
    return kTypeError;
  }
  if (!checkModifiable(operand, inner, expr, SemaErrorCode::IncDecNotLvalue,
                       " and be incremented")) {
    return inner;
  }
  if (types_.isParam(inner)) {
    // A binder the class admitted, and the whole answer -- the same statement the
    // prefix form makes, because `x++` and `++x` are the same operation with the
    // value kept or dropped.
    (void)info;
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
    recordAccess(expr, element, placeProvenanceOf(base), count, ExtentKind::Count);
    return element;
  }
  // `s[i]` on a **slice**: the element of the view, which is a load through the
  // descriptor's pointer word. The third base that reaches memory, and the one
  // whose extent is a *value* rather than a type -- so the record says
  // `ExtentKind::Length` and **not** a count: the number is the descriptor's own
  // `len` word, which the checked build is the reader of, and a record that
  // carried `0` here would be claiming an empty object rather than a length it
  // cannot see (`slices.md` decision 20, `checks.md`).
  if (types_.isSlice(baseType)) {
    if (!isIntegerOperand(types_, indexType)) {
      error(index, SemaErrorCode::IndexNotInteger,
            "`[]` needs an integer index; `" + types_.spelling(indexType) + "` is not one");
      return kTypeError;
    }
    // Materialised at the index width for the same reason `p[i]` is: the
    // `getelementptr` index has one width and the conversion is recorded here.
    (void)checkOperand(expr, 1, index, types_.signedInt(types_.target().pointerBits));
    const TypeId element = types_.elementOf(baseType);
    info.isLvalue = true;
    info.isConstant = false;
    info.hasIntValue = false;
    recordAccess(expr, element, provenanceOf(base), /*extent=*/0, ExtentKind::Length);
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

TypeId Checker::checkSlice(ast::AstId expr, ExprInfo& info) {
  // `a[l..r]` -- the view, in its four forms. One function, because the four
  // share every rule and differ only in which bound is absent: `a[l..]` is "from
  // l to the end of what this base is", and for an array that end is a number the
  // *type* holds, for a slice a word in the descriptor, and for a pointer
  // something only the programmer knows -- which is why a pointer has to write
  // both and the other two may omit the second (`slices.md` decisions 8 and 9).
  const ast::SliceParts parts = file_.slicePartsOf(expr);
  if (!parts.hasBase()) {
    return kTypeError;
  }
  const TypeId baseType = checkExpr(parts.base, kInvalidType);

  // Both bounds are typed before anything is refused, and they are typed even
  // when the base is already an error: the reader wrote them, and skipping them
  // would report one mistake and hide the one next to it.
  const TypeId indexInt = types_.signedInt(types_.target().pointerBits);
  // The operands as they appear in `SliceExpr`: the base, then the bounds, with
  // the one that was not written simply absent. The ordinal is what
  // `checkOperand` records the conversion against, so it is computed from the
  // form and not from the position of a token.
  const std::uint8_t endOperand = parts.hasBegin() ? 2 : 1;
  std::optional<support::ConstInt> beginValue;
  std::optional<support::ConstInt> endValue;
  bool boundsOk = true;
  if (parts.hasBegin()) {
    const TypeId boundType = checkExpr(parts.begin, kInvalidType);
    if (types_.isError(boundType)) {
      boundsOk = false;
    } else if (!isIntegerOperand(types_, boundType)) {
      error(parts.begin, SemaErrorCode::IndexNotInteger,
            "the beginning of a view is an integer; `" + types_.spelling(boundType) +
                "` is not one");
      boundsOk = false;
    } else {
      (void)checkOperand(expr, 1, parts.begin, indexInt);
      const ExprInfo& boundInfo = out_.typed.infoOf(parts.begin);
      if (boundInfo.hasIntValue) {
        beginValue = boundInfo.value;
      }
    }
  }
  if (parts.hasEnd()) {
    const TypeId boundType = checkExpr(parts.end, kInvalidType);
    if (types_.isError(boundType)) {
      boundsOk = false;
    } else if (!isIntegerOperand(types_, boundType)) {
      error(parts.end, SemaErrorCode::IndexNotInteger,
            "the end of a view is an integer; `" + types_.spelling(boundType) + "` is not one");
      boundsOk = false;
    } else {
      (void)checkOperand(expr, endOperand, parts.end, indexInt);
      const ExprInfo& boundInfo = out_.typed.infoOf(parts.end);
      if (boundInfo.hasIntValue) {
        endValue = boundInfo.value;
      }
    }
  }
  if (types_.isError(baseType) || !boundsOk) {
    return kTypeError;
  }

  const bool array = types_.isArray(baseType);
  const bool slice = types_.isSlice(baseType);
  const bool pointer = types_.isPointer(baseType);
  if (!array && !slice && !pointer) {
    error(parts.base, SemaErrorCode::SliceNotViewable,
          "`[..]` takes a view of an array, a slice or a pointer; `" + types_.spelling(baseType) +
              "` has no elements, and no extent to measure a bound against");
    return kTypeError;
  }
  // A pointer is the one base with no length in any type, so the two bounds are
  // both written and the compiler has nothing to infer. It is the form that
  // reads as *unchecked*, and it is the form that says so (`slices.md` decision
  // 8).
  if (pointer && !(parts.hasBegin() && parts.hasEnd())) {
    error(expr, SemaErrorCode::SlicePointerNeedsBothBounds,
          "a view of a pointer `" + types_.spelling(baseType) +
              "` has no length to count back from, so both bounds are written: `p[0..n]`");
    return kTypeError;
  }

  const TypeId element = pointer ? types_.pointeeOf(baseType) : types_.elementOf(baseType);
  if (!types_.known(element) || types_.isVoid(element)) {
    error(expr, SemaErrorCode::PointerVoidAccess,
          "`*void` cannot be viewed: `void` has no size, so there are no elements in a view of "
          "one");
    return kTypeError;
  }

  // The bounds that are numbers, checked where they are written -- the same
  // argument a constant `a[i]` gets and for the same reason: the count is in the
  // type, so "is this inside the object" is arithmetic on two numbers the
  // compiler already has, and refusing it costs no analysis at all
  // (`arrays.md` decision 7).
  if (array) {
    // The object's extent, and the end is allowed to *reach* it: `a[0..4]` on a
    // `[4]i32` is the whole object rather than an error, which is the one place a
    // bound and an index differ.
    const std::uint64_t count = types_.countOf(baseType);
    const auto outOfRange = [&](support::ConstInt value) { return !boundInRange(value, count); };
    if (beginValue && outOfRange(*beginValue)) {
      error(parts.begin, SemaErrorCode::IndexOutOfRange,
            "the beginning " + valueText(*beginValue) + " is outside `" +
                types_.spelling(baseType) + "`: a bound goes from 0 to " + std::to_string(count));
      return kTypeError;
    }
    if (endValue && outOfRange(*endValue)) {
      error(parts.end, SemaErrorCode::IndexOutOfRange,
            "the end " + valueText(*endValue) + " is outside `" + types_.spelling(baseType) +
                "`: a bound goes from 0 to " + std::to_string(count));
      return kTypeError;
    }
  }
  // A view of a negative number of elements. Its own sentence, because the
  // repair is different from "outside the object": the two numbers are each
  // inside it and in the wrong order, and the descriptor would hold a length that
  // is a huge unsigned number rather than the empty view the reader meant.
  if (beginValue && endValue) {
    const std::optional<std::uint64_t> beginBound = nonNegative(*beginValue);
    const std::optional<std::uint64_t> endBound = nonNegative(*endValue);
    if (beginBound && endBound && *beginBound > *endBound) {
      error(expr, SemaErrorCode::SliceBoundsReversed,
            "`" + valueText(*beginValue) + ".." + valueText(*endValue) +
                "` is backwards: the first bound is a beginning, so it cannot be past the end");
      return kTypeError;
    }
  }

  // The result is the view of the element, and it is **not** a place: `&s[0..2]`
  // is refused one stage up by the address-of rule, because a descriptor built
  // here is a value and a pointer to it would be a pointer to a temporary
  // (`slices.md` decision 11).
  info.isLvalue = false;
  info.isConstant = false;
  info.hasIntValue = false;
  const TypeId result = types_.sliceOf(element);
  if (!result.valid()) {
    reportLimit(expr);
    return kTypeError;
  }
  return result;
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

TypeId Checker::checkTupleExpr(ast::AstId expr, TypeId expected, ExprInfo& info) {
  // `(a, b)`: a **product**, whose members the *position* types. It is not the
  // array literal: an array's element type comes from the context because the
  // count is part of its type and a list has no type of its own, while a product
  // carries every member type it has and stands alone.
  //
  // What the context decides is therefore only *which* type each member is, and
  // it decides it per position: `let p: (i64, i64) = (1, 2);` is two `i64`s, and
  // `let p: (i64, i32) = (1, 2);` is one of each. That is the rule every other
  // literal position already obeys (a binding, a call argument, an element), and
  // a product is its fourth position rather than an exception to it
  // (`tuples.md`, decision 8).
  //
  // What the context cannot do is change a literal's *class*: an integer literal
  // in a `f64` member stays an `i32` -- `decideAt`'s rule, which is the whole
  // reason `let p: (f64, f64) = (1, 2);` is refused and `(1.0, 2.0)` is what has
  // to be written. Silent integer-to-float is the conversion this language does
  // not have (`casts.md`), and being inside a product is not an exception to it.
  const std::vector<ast::AstId> elements = operandsOf(expr);
  if (elements.size() < 2) {
    // The parser owns the sentences for a group of one (and for `()`); this is
    // the defensive half, with no second diagnostic.
    info = ExprInfo{};
    return kTypeError;
  }

  // The expected type, when it is a product of the same arity: the members are
  // then typed *against their own position*, which is what makes a literal adapt
  // (`1` in an `f64` member) while a non-literal value never narrows in silence.
  std::span<const TypeId> expectedMembers;
  if (types_.known(expected) && types_.isTuple(expected) &&
      types_.membersOf(expected).size() == elements.size()) {
    expectedMembers = types_.membersOf(expected);
  }

  std::vector<TypeId> members;
  members.reserve(elements.size());
  for (std::size_t i = 0; i < elements.size(); ++i) {
    const TypeId wanted = expectedMembers.empty() ? kInvalidType : expectedMembers[i];
    TypeId member = checkExpr(elements[i], wanted);
    if (types_.isError(member)) {
      // One bad member poisons the product: the members are a *type*, and a type
      // with a hole in it is not one the store can build. The member's own
      // sentence is the one that was printed.
      info = ExprInfo{};
      return kTypeError;
    }
    // Nothing decided a member whose type its context had not: an untyped literal
    // is an `i32` (an `f64` for a float), which is the same answer `let x = 1;`
    // gets.
    member = defaultValue(member);
    if (!types_.isObject(member)) {
      // `void` and `!` are the two that reach here: a member is *stored*, so it
      // has to be a type that can be stored. A call that produces nothing and a
      // call that never returns are both statements, not members.
      error(elements[i], SemaErrorCode::InvalidAssignment,
            "`" + types_.spelling(member) +
                "` cannot be a member of a product: a member is stored, so it has to be a type "
                "that can be stored");
      info = ExprInfo{};
      return kTypeError;
    }
    members.push_back(member);
  }

  // The product's size is checked *before* the intern so the sentences can differ:
  // a product larger than the target can address is a fact about the program, and
  // the type budget is a fact about the unit.
  //
  // A member with no size *yet* is not that, and the two are told apart first: a
  // product written over a binder -- `(left, right)` inside a generic body, where
  // each member is a `Param` -- has no layout until the declaration is
  // instantiated, and asking the layout rule about it would refuse the template
  // with a sentence about a size that is not yet known
  // (`generics.md`; `type_store.cc` makes the same exception where it builds one).
  const bool unknownSize = [&] {
    for (const TypeId member : members) {
      if (types_.hasUnknownSize(member)) {
        return true;
      }
    }
    return false;
  }();
  if (!unknownSize && !types_.tupleSize(members).has_value()) {
    error(expr, SemaErrorCode::InvalidAssignment,
          "this product of " + std::to_string(members.size()) +
              " members is larger than this target can address");
    info = ExprInfo{};
    return kTypeError;
  }
  const TypeId product = types_.tupleOf(members);
  if (!product.valid()) {
    reportLimit(expr);
    info = ExprInfo{};
    return kTypeError;
  }
  info.isLvalue = false;
  info.isConstant = false;
  info.hasIntValue = false;
  return product;
}

TypeId Checker::checkField(ast::AstId expr, ExprInfo& info) {
  // `t.0`: a member of a product, chosen **by position**, and the position is
  // settled here -- the access is not an index and has no runtime form
  // (`tuples.md`, decision 3).
  const std::vector<ast::AstId> children = operandsOf(expr);
  if (children.empty()) {
    return kTypeError;
  }
  const ast::AstId base = children.front();
  const TypeId baseType = checkExpr(base, kInvalidType);
  if (types_.isError(baseType)) {
    return kTypeError;
  }
  // The member token: the `.` is punctuation and the member is what follows it.
  // A tree with no member is the parser's finding (it reports one), so this
  // answers without a second sentence.
  const std::span<const ast::AstId> all = file_.childrenOf(expr);
  ast::AstId member;
  for (std::size_t i = 0; i < all.size(); ++i) {
    const ast::Node& node = file_.at(all[i]);
    if (node.isToken() && tagOf(node.kind) == kTokDot && i + 1 < all.size()) {
      member = all[i + 1];
      break;
    }
  }
  if (!member.valid()) {
    return kTypeError;
  }
  const ast::Node& memberNode = file_.at(member);
  // A member the *parser* refused -- a chain it could not tell from one number
  // (`t.0.1`), a token that is not a member at all -- is not this stage's finding.
  // The region is marked and its sentence is already printed, and answering it a
  // second time would be two diagnostics for one mistake (`tuples.md`, decision 3).
  if (memberNode.inError) {
    return kTypeError;
  }
  const bool isPosition = tagOf(memberNode.kind) == kTokIntegerLiteral;
  const bool isName = file_.at(member).is(kIdentifierNode);

  if (!types_.isTuple(baseType)) {
    // Today every value here is an array, a slice, a pointer or a scalar, and
    // none of them has members. The sentence says what a member *is* rather than
    // only that this is not one, because the reader who wrote `.len` is asking a
    // question this record has an answer for (`slices.md` decision 15).
    const std::string what = isName ? "`" + std::string(spelling(member)) + "`" : "a position";
    error(expr, SemaErrorCode::UnknownMember,
          "`" + types_.spelling(baseType) + "` has no members, so " + what +
              " is not one: `.` reads a member of a product, and lengths and views arrive with "
              "the operations the stdlib is where to find");
    return kTypeError;
  }

  // A product's members are named by *position* and by nothing else, so a name
  // after the dot is the wrong kind of component rather than an unknown one.
  if (!isPosition) {
    const std::size_t count = types_.membersOf(baseType).size();
    error(member, SemaErrorCode::UnknownMember,
          "a product's members have no names: `" + types_.spelling(baseType) + "` has " +
              std::to_string(count) + " of them, reached as `t.0` to `t." +
              std::to_string(count == 0 ? 0 : count - 1) + "`");
    return kTypeError;
  }

  // The position, folded where its spelling is: `t.0` is one member whatever
  // base the digits were written in, and a suffix (`t.0u8`) is a *value* and not
  // a position -- the reader would be asking for member `u8`, which is not a
  // number.
  const support::IntegerLiteral position =
      support::parseIntegerLiteral(spelling(member), support::IntegerBaseRule::DecimalLeadingZero);
  const std::size_t count = types_.membersOf(baseType).size();
  if (!position.ok || position.value.bits >= count) {
    error(member, SemaErrorCode::IndexOutOfRange,
          "this product has " + std::to_string(count) + " members, reached as `t.0` to `t." +
              std::to_string(count == 0 ? 0 : count - 1) + "`");
    return kTypeError;
  }

  // A member of a place **is** a place: `t.0 = 9` stores into the first member of
  // `t`, which is the same rule `a[0] = 9` obeys. And a member of a value is a
  // value, so the two questions stay answered by the base.
  const ExprInfo& baseInfo = out_.typed.infoOf(base);
  info.isLvalue = baseInfo.isLvalue;
  info.isConstant = baseInfo.isConstant;
  info.hasIntValue = false;
  const TypeId memberType =
      types_.membersOf(baseType)[static_cast<std::size_t>(position.value.bits)];

  // The access record, written the same way the array subscript writes its own
  // (`access.cc`): a member of a **place** is reached through an address, and the
  // checked build guards that address -- null, and alignment -- exactly as it
  // guards `a[0]`. A member of a *value* is not an access at all: it is an
  // `extractvalue` out of an aggregate in registers, and nothing is reached
  // through a pointer, so recording one would put an obligation in the artifact
  // for an access that does not exist.
  //
  // The extent is the arity and the kind is `Count`, which is the honest answer to
  // "how large is the object this access is inside" -- but there is no range to
  // test: the position is a *constant* the two refusals above already bounded
  // against that very count, so the lowering emits no bounds comparison for it
  // (`tuples.md`, decision 11). The record still carries the number, which is what
  // the dump and a future shadow memory read.
  if (info.isLvalue) {
    recordAccess(expr, memberType, placeProvenanceOf(base), count, ExtentKind::Count);
  }
  return memberType;
}

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

  const TypeSpecResult spec = readType(parts, types_, names(), inferredCount);
  if (!spec.ok) {
    error(typeNode, codeOf(spec), spec.message);
    setType(typeNode, kTypeError);
    return kTypeError;
  }
  if (spec.brokenName) {
    // A name whose expansion already failed, reported where it failed: the same
    // rule `resolveTypeNode` applies, for the same reason (`type_alias.md`).
    setType(typeNode, kTypeError);
    return kTypeError;
  }
  if (!spec.type.valid()) {
    reportLimit(typeNode);
    setType(typeNode, kTypeError);
    return kTypeError;
  }
  setType(typeNode, spec.type);
  if (types_.isSlice(spec.type)) {
    // `[]i32{1, 2, 3}`, and the one refusal this node has that the array does not.
    // A slice is a *view*: its bytes live somewhere else, so there is no object
    // for a literal to be. The sentence names the two things the reader can
    // actually write, which is the difference between a refusal and a dead end
    // (`slices.md` decision 7).
    error(typeNode, SemaErrorCode::InitializerShape,
          "a slice has no literal: `" + types_.spelling(spec.type) +
              "` is a view of storage something else owns, so name the object -- an array, or "
              "the result of a pointer -- and take a view of it: `table[..]`");
    return kTypeError;
  }
  if (!types_.isArray(spec.type)) {
    // A `Type` this node cannot hold. The parser recognizes a typed initializer
    // when the run opens with a bracket group (`[N]`, `[_]`, `[]`), so the two
    // shapes that can reach here are handled above and below; anything else is a
    // tree this stage did not build.
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

// A **binder** on one side of a binary operator, which is where a constraint is
// spent.
//
// The class decides whether the operation is allowed at all, and the refusal names
// the word to write. Everything else follows from the two operands being one hole:
// the operation happens *at* the binder, so its result is the binder for an
// arithmetic operator and `bool` for a comparison, and the operand record is written
// with the binder as the operation type -- which the lowering substitutes per
// instance, so `src/ir` never sees a `Param` (`generics.md`, decision 20).
//
// Three cases reach here and each has its own answer:
//
//   * `a + b` with both operands the same binder: the class decides, and the result
//     is the binder.
//   * `a + b` with two *different* binders: not a constraint question at all. There
//     is no conversion between two type parameters, so the operands are simply not
//     two of one type.
//   * `a + 1`: a deferred literal beside a binder, and the one place a class is
//     consulted for something other than "may this operator be used". `Integer`
//     admits `1` and `Number` does not, because for one instantiation `1` is `1i32`
//     and for another the body would have to mean `1.0` -- and a body that means two
//     things is not a body (`generics.md`, decision 11).
TypeId Checker::checkBinaryOnParameter(ast::AstId expr, Tag kind, ast::AstId lhs, ast::AstId rhs,
                                       TypeId left, TypeId right, ExprInfo& info) {
  const support::Operation operation = binaryOperationOf(kind);
  const std::string written = quotedOperator(kind);

  const bool leftIsBinder = types_.isParam(left);
  const TypeId binder = leftIsBinder ? left : right;
  const TypeId other = leftIsBinder ? right : left;
  const ast::AstId at = leftIsBinder ? lhs : rhs;
  const ast::AstId otherAt = leftIsBinder ? rhs : lhs;
  const std::uint8_t binderOperand = leftIsBinder ? 0 : 1;
  const std::uint8_t otherOperand = leftIsBinder ? 1 : 0;

  if (types_.isParam(other) && other != binder) {
    error(expr, SemaErrorCode::InvalidOperands,
          written + " needs two operands of one type; `" + types_.spelling(binder) + "` and `" +
              types_.spelling(other) +
              "` are two different type parameters, and nothing converts between them");
    return kTypeError;
  }

  if (refuseOperation(at, binder, operation, written)) {
    return kTypeError;
  }

  if (types_.isDeferred(other)) {
    // The literal rule. The class's own literal class is the whole test, and it is
    // read from the table rather than re-derived here, so `Integer` and `Float`
    // cannot come to disagree with the rest of the compiler about what `1` means.
    //
    // `&&`/`||` cannot reach this line: no class grants them, so `refuseOperation`
    // above has already answered. A comparison can, and `a < 1.0` under `Float` is
    // the case it exists for.
    const support::LiteralClass admitted =
        support::constraintLiteralClass(types_.binderClass(binder));
    const bool literalIsFloat = types_.get(other).kind == TypeKind::FloatLiteral;
    if (!support::literalAdmittedBy(admitted, literalIsFloat)) {
      refuseLiteralInBinder(otherAt, binder, other);
      return kTypeError;
    }
    // The literal is **decided** as the binder, and that is what makes one body mean
    // the right value per instance: the text is `1` in every instance, and the type
    // is substituted at the boundary like every other type of the node.
    setType(otherAt, binder);
  } else if (other.valid() && !types_.isParam(other) && other != binder) {
    // A concrete operand beside a binder. Assignability is the question, and it has
    // one honest answer: the body is checked once, so a value is only usable here if
    // *every* type the class admits can take it -- which is a question
    // `checkAssignable` already answers with the sentence that fits.
    checkAssignable(other, binder, otherAt, SemaErrorCode::InvalidOperands, " as an operand");
  }

  // The operation happens **at the binder**, and both operands are recorded against
  // it -- for a comparison too, because `T == T` is an `icmp` at the instance's type
  // and the record is what tells the lowering which width that is. A conversion that
  // changes nothing is not stored (`recordOperationOperand`), so an operand that
  // already is the binder writes nothing.
  recordOperationOperand(expr, binderOperand, at, binder);
  recordOperationOperand(expr, otherOperand, otherAt, binder);
  // **Nothing is written to `info.opType`**, and that is deliberate rather than an
  // omission: `opType` exists for the one expression where the store's type is not
  // the operation's -- a compound assignment `x <<= n` on a `u16`, which shifts at
  // `i32` (`ir.md`). A binary expression is performed at its operands' type, the
  // lowering reads that from the left operand (`lowerBinary`), and a second copy here
  // would be a fact with two sources. Storing the binder would be worse than useless:
  // a `Param` is not a type `src/ir` may ever see, and an unused field is exactly
  // where one would sit until something read it.
  info.isLvalue = false;
  info.isConstant = false;
  info.hasIntValue = false;
  return isComparison(kind) ? kTypeBool : binder;
}

bool Checker::isUnparenthesizedComparison(ast::AstId operand) const {
  // A parenthesis is its own node (`ParenExpr`), so it is enough to ask what the
  // operand *is*: a chain is a comparison node whose left operand is a comparison
  // node, and `(a < b) > c` has `ParenExpr` where this looks for `BinaryExpr`.
  if (kindOf(operand) != ast::NodeKind::BinaryExpr) {
    return false;
  }
  const ast::AstId op = tokenOf(operand);
  return op.valid() && isComparison(tagOf(kindOf(op)));
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

  // A **binder** operand before any operation rule, and the order is the point:
  // every rule below compares two concrete kinds, and what a binder earns is a
  // sentence about the *class* it declared -- not the arithmetic rules' one about a
  // type the reader never wrote (`generics.md`, § 6).
  if (types_.isParam(left) || types_.isParam(right)) {
    return checkBinaryOnParameter(expr, kind, lhs, rhs, left, right, info);
  }

  // **A comparison does not chain.** `a < b > c` is `(a < b) > c` by
  // associativity, and the rules below already refuse that -- with a sentence about
  // an operand, `>` needs arithmetic operands; got `bool` and `i32`, that never
  // names the thing the reader wrote. This names it, and it is asked of the two
  // facts a chain can never satisfy: this operator orders, and the operand on its
  // left is a comparison that no parenthesis came between.
  //
  // `(a < b) > c` is the reader saying they meant it, so it is not a chain and it
  // gets the operand sentence instead -- because that is what is wrong with it.
  // The parenthesis is the whole difference, and it is why this is not a rule about
  // the operator (`casts.md`, decision 20).
  if (isOrderingComparison(kind) && isUnparenthesizedComparison(lhs)) {
    error(expr, SemaErrorCode::ComparisonChain,
          "comparison does not chain: `a < b > c` compares `(a < b)` with `c`. Write `(a < b) > "
          "c` for that, or two comparisons joined by `&&`");
    return kTypeError;
  }

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
      // Equality is defined for arithmetic values and for two `bool`s, and for
      // nothing else. A `str` is *not* one of them: this operator would be C's
      // `s1 == s2`, an address comparison, and a program that wrote it meant the
      // contents (`sema.md`, decision 8 — the design record is where the sentence
      // comes from). A product is not comparable either, and its sentence names
      // the members to compare: `==` on two products would have to mean "every
      // member `==`", which would be a built-in rule for an operator the
      // language gives per type through a declared interface (`tuples.md`,
      // decision 17).
      if (types_.isTuple(left) || types_.isTuple(right)) {
        error(expr, SemaErrorCode::InvalidOperands,
              "`" + std::string(opText(kind)) +
                  "` has no meaning for a product: compare the members, as in `a.0 == b.0 && a.1 "
                  "== b.1`");
        return kTypeError;
      }
      const bool arithmetic = types_.isArithmetic(left) && types_.isArithmetic(right);
      const bool bothBool = left == right && types_.get(left).kind == TypeKind::Bool;
      if (!arithmetic && !bothBool) {
        // `str` has a sentence of its own, because it is the one refused type a
        // reader is likely to have meant something by: the comparison is right
        // there in the source, and what it does is not what it looks like.
        const bool str =
            types_.get(left).kind == TypeKind::Str || types_.get(right).kind == TypeKind::Str;
        error(expr, SemaErrorCode::InvalidOperands,
              str ? "`" + std::string(opText(kind)) +
                        "` on a `str` would compare addresses, not contents, so it is refused: "
                        "compare the bytes with a library call (a `str` is a pointer to bytes)"
                  : "`" + std::string(opText(kind)) +
                        "` needs two arithmetic values or two `bool`s; got `" +
                        types_.spelling(left) + "` and `" + types_.spelling(right) + "`");
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

TypeId Checker::checkCall(ast::AstId expr, TypeId expected, ExprInfo& info) {
  const std::vector<ast::AstId> operands = operandsOf(expr);
  if (operands.empty()) {
    return kTypeError;
  }
  (void)info;
  const ast::AstId callee = operands.front();

  // A builtin before anything else, because its callee has no type *yet*: what
  // `clz` is depends on the argument that has not been checked. Everything from
  // here on is a function call, and the row's checker reuses this file's
  // argument machinery so a builtin's diagnostics read exactly like a function's.
  if (const builtins::BuiltinInfo* row = builtinCallee(callee)) {
    return checkBuiltinCall(expr, info, *row);
  }

  // A **generic** callee before the ordinary path, because it is the one call
  // whose signature is not the callee's type: a template holds `Param`s, and what
  // a call needs is the instance an argument list names (`generics.cc`). The def
  // is what decides -- not the spelling, so a `let identity = 1;` in an inner
  // scope still shadows the name and calls the binding.
  if (const std::optional<resolve::DefId> def = defOfPath(callee); def.has_value()) {
    if (const auto generic = genericFunctionByDef_.find(def->index);
        generic != genericFunctionByDef_.end()) {
      return checkGenericCall(expr, callee, generic->second, expected, info);
    }
  }

  const TypeId calleeType = checkExpr(callee, kInvalidType);

  // The arguments, by **kind**: a call that wrote `::<...>` has a third child, so
  // the value list is not simply the second operand.
  const ast::AstId argList = childOf(expr, ast::NodeKind::ArgList);
  const std::vector<ast::AstId> args =
      argList.valid() ? operandsOf(argList) : std::vector<ast::AstId>{};

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
    error(expr, SemaErrorCode::ArgumentCount,
          argumentCountText(params.size(), args.size(), variadic));
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
