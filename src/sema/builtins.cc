// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The builtin call: an abstract row, resolved into the types one call site asks
// for.
//
// ### Why the callee is not typed like a function
//
// A user function's type exists before its call does, so `checkCall` can type the
// callee, read its parameters and check the arguments against them. A family's
// does not: `clz` of an `i32` is not the same type as `clz` of a `u8`, and which
// one this call is depends on the *argument*, which has not been checked yet. So
// the order is inverted here -- the arguments decide the parameter list -- and
// everything after that is deliberately the same machinery a user function's call
// uses: `checkOperand` for the argument, `checkAssignable` for the conversion,
// and the same sentence builder for a wrong count. A reader cannot tell the two
// apart in a diagnostic, which is the point.
//
// ### What this stage may not do
//
// It may not ask whether the intrinsic is valid for the types it chose: that is
// LLVM's knowledge and `sema` does not include LLVM. What it *can* do is refuse
// the cases the row already knows about -- a width the operation does not exist
// for (`MatchedWidths`) -- because those are facts about the *language's*
// surface, decided here, checkable here, and testable without a module.
#include "builtins/builtin.h"

#include "checker.h"

#include <cstddef>
#include <string>
#include <vector>

namespace minc::sema {
namespace {

// LLVM's `bswap` needs a whole number of bytes, and at least two. Stated as the
// rule the verifier states ("bswap must be an even number of bytes") rather than
// as a list, so a width this language adds later is covered by the same sentence.
[[nodiscard]] bool evenByteWidth(std::uint16_t bits) {
  return bits >= 16 && bits % 8 == 0 && (bits / 8) % 2 == 0;
}

} // namespace

std::string Checker::argumentCountText(std::size_t expected, std::size_t given, bool variadic) {
  // Written once because two callers produce it: a user function's call and a
  // builtin's. A builtin is never variadic -- a row states its whole parameter
  // list -- so the flag is false from that side, and the sentence is the same one
  // either way, which is what makes a builtin's mistake read like a function's.
  //
  // "at least", because for a variadic function the sentence is about a
  // *minimum*: telling the reader the count is wrong when they passed too few is
  // the whole point.
  std::string message = "this function takes ";
  if (variadic) {
    message += "at least " + std::to_string(expected) + " argument(s)";
  } else {
    message += expected == 0 ? "no arguments" : std::to_string(expected) + " argument(s)";
  }
  message += ", but " + std::to_string(given) + " were given";
  return message;
}

const builtins::BuiltinInfo* Checker::builtinCallee(ast::AstId node) const {
  if (!node.valid() || kindOf(node) != ast::NodeKind::PathExpr) {
    return nullptr;
  }
  const std::optional<resolve::DefId> def = defOfPath(node);
  if (!def.has_value() || def->index >= defs_.defs.size()) {
    return nullptr;
  }
  const builtins::BuiltinId id = defs_.defs[def->index].builtin;
  return id == builtins::BuiltinId::None ? nullptr : builtins::lookup(id);
}

TypeId Checker::builtinType(builtins::BuiltinType type) const {
  switch (type) {
  case builtins::BuiltinType::Void:
    return kTypeVoid;
  case builtins::BuiltinType::Bool:
    return kTypeBool;
  case builtins::BuiltinType::Char:
    return kTypeChar;
  case builtins::BuiltinType::Str:
    return kTypeStr;
  case builtins::BuiltinType::I8:
    return types_.signedInt(8);
  case builtins::BuiltinType::I16:
    return types_.signedInt(16);
  case builtins::BuiltinType::I32:
    return types_.signedInt(32);
  case builtins::BuiltinType::I64:
    return types_.signedInt(64);
  case builtins::BuiltinType::I128:
    return types_.signedInt(128);
  case builtins::BuiltinType::Isize:
    return types_.signedInt(types_.target().pointerBits);
  case builtins::BuiltinType::U8:
    return types_.unsignedInt(8);
  case builtins::BuiltinType::U16:
    return types_.unsignedInt(16);
  case builtins::BuiltinType::U32:
    return types_.unsignedInt(32);
  case builtins::BuiltinType::U64:
    return types_.unsignedInt(64);
  case builtins::BuiltinType::U128:
    return types_.unsignedInt(128);
  case builtins::BuiltinType::Usize:
    return types_.unsignedInt(types_.target().pointerBits);
  case builtins::BuiltinType::F32:
    return types_.floatOf(32);
  case builtins::BuiltinType::F64:
    return types_.floatOf(64);
  case builtins::BuiltinType::VoidPtr:
    return types_.pointerTo(kTypeVoid);
  case builtins::BuiltinType::Never:
    return kTypeNever;
  case builtins::BuiltinType::AnyInteger:
  case builtins::BuiltinType::MatchArg:
  case builtins::BuiltinType::MatchArgPtr:
    // Not a type, a *shape*: it is resolved from an argument, and a row that
    // reached here with one in a position where no argument can establish it is a
    // table bug the table's own test refuses (`tests/unit/builtins/table_test.cc`,
    // "a row with a hole fills it"). The error type keeps the caller total.
    return kTypeError;
  }
  return kTypeError;
}

TypeId Checker::checkIntegerArgument(ast::AstId node) {
  // No expectation is passed, and that is the family's whole point: the operation
  // is on the bit pattern, so the argument's own type *is* the parameter's. What
  // a deferred literal does here is what it does at a variadic argument and
  // anywhere else nothing decides it: it takes its default (`1` is an `i32`).
  //
  // A conversion is recorded against the argument for the same reason a user
  // function's argument gets one -- the table says these types are accepted, so
  // `clz(x: u8)` must be a `u8` argument and not a promoted one.
  (void)checkExpr(node, kInvalidType);
  const TypeId type = defaultValue(decideAt(node, kInvalidType));
  if (!type.valid() || types_.isError(type)) {
    return kTypeError;
  }
  if (types_.isInteger(type)) {
    return type;
  }
  error(node, SemaErrorCode::InvalidOperands,
        "this argument must be an integer type, and this is `" + types_.spelling(type) + "`");
  return kTypeError;
}

TypeId Checker::checkBuiltinCall(ast::AstId expr, ExprInfo& info,
                                 const builtins::BuiltinInfo& row) {
  (void)info;
  const std::vector<ast::AstId> operands = operandsOf(expr);
  if (operands.empty()) {
    return kTypeError;
  }
  const ast::AstId callee = operands.front();
  std::vector<ast::AstId> args;
  if (operands.size() > 1) {
    args = operandsOf(operands[1]);
  }

  const std::span<const builtins::BuiltinType> shape = row.signature.params;

  // The count first, and with the sentence a function's wrong count gets. A
  // builtin cannot be variadic -- a row states its whole parameter list -- so the
  // comparison is the exact one.
  if (args.size() != shape.size()) {
    error(expr, SemaErrorCode::ArgumentCount,
          argumentCountText(shape.size(), args.size(), /*variadic=*/false));
  }

  // The concrete parameter list, built as the arguments arrive. `matched` is the
  // type another argument established -- the value of a rotate, the value an
  // out-parameter points at -- and it stays invalid until one does.
  std::vector<TypeId> params;
  params.reserve(args.size());
  TypeId matched = kInvalidType;

  for (std::size_t i = 0; i < args.size(); ++i) {
    if (i >= shape.size()) {
      // Past the row's arguments there is nothing to check against, and the
      // argument is still typed: a wrong count must not hide a wrong argument.
      (void)checkVariadicArgument(expr, static_cast<std::uint8_t>(i + 1), args[i]);
      continue;
    }

    const builtins::BuiltinType param = shape[i];
    TypeId expected = kInvalidType;
    if (param == builtins::BuiltinType::AnyInteger) {
      // Typed by itself: this *is* the arm that decides the parameter's type.
      const TypeId argumentType = checkIntegerArgument(args[i]);
      if (!types_.known(matched) && argumentType.valid() && !types_.isError(argumentType)) {
        matched = argumentType;
      }
      params.push_back(argumentType);
      // The match is established here and nowhere else: no conversion was
      // recorded for this argument, and the argument's own type *is* the
      // parameter's -- there is nothing to check assignability against.
      continue;
    }
    if (param == builtins::BuiltinType::MatchArg || param == builtins::BuiltinType::MatchArgPtr) {
      if (types_.known(matched) && !types_.isError(matched)) {
        expected = param == builtins::BuiltinType::MatchArg ? matched : types_.pointerTo(matched);
      }
    } else {
      expected = builtinType(param);
    }

    const TypeId argumentType =
        checkOperand(expr, static_cast<std::uint8_t>(i + 1), args[i], expected);
    params.push_back(argumentType);
    // The **first** parameter that is not a hole establishes the row's matched
    // type, and it is established once: a concrete parameter states it, an
    // `any-int` takes it from its argument. A later argument filling the same hole
    // must not restate it, or `rotl(x, n)` -- where the count is an `any-int` of
    // its own -- would answer with the *count's* width, which is a rotate of a
    // number nobody wrote.
    if (!types_.known(matched) && expected.valid() && !types_.isError(expected) &&
        param != builtins::BuiltinType::MatchArg && param != builtins::BuiltinType::MatchArgPtr) {
      matched = expected;
    }
    if (expected.valid() && !types_.isError(expected)) {
      checkAssignable(argumentType, expected, args[i], SemaErrorCode::InvalidAssignment,
                      " as this argument");
    }
  }

  // The widths the operation exists for, before the call is given a type: a
  // refusal here is a sentence about the argument the reader wrote, and the
  // alternative -- LLVM refusing to verify the module -- is a failure the program
  // cannot be told about and the compiler cannot explain.
  TypeId matchedType = matched;
  if (row.signature.matchedWidths == builtins::MatchedWidths::EvenBytes && types_.known(matched) &&
      !types_.isError(matched) && types_.isInteger(matched)) {
    const std::uint16_t bits = types_.get(matched).bits;
    if (!evenByteWidth(bits)) {
      error(callee, SemaErrorCode::BuiltinWidth,
            "`" + std::string(row.spelling) +
                "` needs a whole number of bytes, at least two, and `" + types_.spelling(matched) +
                "` is one byte");
      matchedType = kTypeError;
    }
  }

  TypeId result = kTypeError;
  switch (row.signature.result) {
  case builtins::BuiltinType::MatchArg:
    result = types_.known(matchedType) && !types_.isError(matchedType) ? matchedType : kTypeError;
    break;
  case builtins::BuiltinType::MatchArgPtr:
    result = types_.known(matchedType) && !types_.isError(matchedType)
                 ? types_.pointerTo(matchedType)
                 : kTypeError;
    break;
  default:
    result = builtinType(row.signature.result);
    break;
  }

  // The callee gets the *concrete* function type of this call. Nothing reads it
  // to lower the call -- `ir` switches on the row before it looks at the callee --
  // and it is recorded anyway, because a name in a call position that had no type
  // at all would be the one node in the tree that breaks the invariant every other
  // stage assumes.
  if (result.valid() && !types_.isError(result)) {
    setType(callee, types_.function(result, params, /*variadic=*/false));
  }
  return result;
}

} // namespace minc::sema
