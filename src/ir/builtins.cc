// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// A builtin, as an instruction. The one place a row's *name* becomes an
// `llvm::Intrinsic::ID`, and the only place in this module that knows a builtin
// exists at all.
//
// ### What this stage reads, and what it refuses to restate
//
// The row is data: a name to look up, the constant operand the intrinsic's
// contract requires, and which argument supplies the overloaded type. Nothing
// else. In particular the *attributes* are not written here, and that is a rule
// with a reason: `Intrinsic::getAttributes` derives `memory(none)`, `noreturn`,
// `speculatable`, `nocallback`, `nofree`, `nosync` and the rest from
// `Intrinsics.td`, so a hand-written `addFnAttr(NoReturn)` next to `llvm.trap`
// would be a second statement of a fact LLVM already made -- and the second one
// is the one that can be wrong. `ir.md` states the rule for functions: the two
// attributes this project emits by hand are `noreturn` and `sret` on a function
// whose *type* says it, and a builtin has no type of its own.
//
// ### Why the language's answer and LLVM's contract differ here, and what closes
// ### the gap
//
// The project's promise is "if the checker lets it pass, it must run". LLVM's
// contract for several of these intrinsics is *poison* for inputs the hardware
// cannot answer, so a row that lowered one as-is would have adopted undefined
// behavior into the language. Each is closed where the row says so:
//
//   - `llvm.ctlz`/`llvm.cttz` take an `is_zero_poison` flag. The language defines
//     `clz(0)` as the *width*, which is exactly what the flag being `false` means,
//     so the row passes `false` (`Lowering::tail`) and no guard is needed. This is
//     the whole reason the field exists.
//   - `llvm.fshl`/`llvm.fshr` are poison for a count at or past the width, while
//     the language defines a rotate as the count *modulo* the width. The `urem` is
//     therefore not an optimization: it is the difference between the language's
//     answer and a value LLVM is free to make anything of.
//   - `llvm.bswap` is refused a whole stage up for a width it does not exist for,
//     because LLVM's verifier rejects the *module* rather than the program.
//
// So there is no `definedness` field beside these facts: the flag and the `urem`
// *are* the answers, and a row that carried a second copy of the same statement
// would be a second thing to keep in step.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Intrinsics.h"

#include "builtins/builtin.h"
#include "lowering.h"
#include "sema/type.h"
#include "sema/typed_ast.h"

namespace minc::ir {

const builtins::BuiltinInfo* Lowering::builtinCallee(ast::AstId callee) const {
  // The same shape as `sema`'s: a builtin is a *def*, and which builtin it is
  // comes from `resolve`'s field and never from the name. A row is looked up by
  // id, so a spelling that appears here would be a second copy of the table --
  // and the source scan in `tests/unit/builtins/table_test.cc` is what keeps one
  // from appearing.
  const std::optional<resolve::DefId> def = defOfPath(callee);
  if (!def.has_value() || def->index >= defs_.defs.size()) {
    return nullptr;
  }
  const builtins::BuiltinId id = defs_.defs[def->index].builtin;
  return id == builtins::BuiltinId::None ? nullptr : builtins::lookup(id);
}

llvm::Value* Lowering::lowerBuiltinArgument(ast::AstId call, ast::AstId argument,
                                            sema::TypeId type) {
  const Value value = lowerOperand(call, argument);
  if (value.v == nullptr) {
    return nullptr;
  }
  if (!types_.isAggregate(type)) {
    return value.v;
  }
  // No row in the table takes an aggregate today, and the refusal is here rather
  // than in a comment: an aggregate crosses this language's functions as a
  // pointer to a copy the *caller* makes (`arrays.md` decision 3), and an
  // intrinsic that expected the first-class value would be handed a pointer --
  // a wrong-typed module, which LLVM refuses to verify. A future row that wants
  // an aggregate says so here, with the copy, and this line is that decision
  // point.
  fatal(spanOf(argument), IRDiagnosticCode::Internal,
        "a builtin argument is an aggregate, which no builtin row takes");
  return nullptr;
}

Value Lowering::lowerBuiltinCall(ast::AstId expr) {
  const std::vector<ast::AstId> operands = operandsOf(expr);
  if (operands.empty()) {
    fatal(spanOf(expr), IRDiagnosticCode::Internal, "a call has no callee");
    return {};
  }
  const ast::AstId callee = operands.front();
  const builtins::BuiltinInfo* row = builtinCallee(callee);
  if (row == nullptr) {
    fatal(spanOf(callee), IRDiagnosticCode::Internal,
          "a call reached the builtin lowering with no row for its callee");
    return {};
  }

  std::vector<ast::AstId> args;
  if (operands.size() > 1) {
    args = operandsOf(operands[1]);
  }

  // The arguments, converted exactly as a function's are: `lowerOperand` applies
  // the conversion `sema` recorded, so the value the intrinsic sees is the value
  // the checker proved the type of.
  std::vector<llvm::Value*> arguments;
  arguments.reserve(args.size());
  const std::span<const sema::TypeId> params = types_.paramsOf(typeOf(callee));
  for (std::size_t i = 0; i < args.size(); ++i) {
    const sema::TypeId param = i < params.size() ? params[i] : sema::kInvalidType;
    llvm::Value* value = lowerBuiltinArgument(expr, args[i], param);
    if (value == nullptr) {
      return {};
    }
    arguments.push_back(value);
  }

  const llvm::Intrinsic::ID id = llvm::Intrinsic::lookupIntrinsicID(row->lowering.name);
  if (id == llvm::Intrinsic::not_intrinsic) {
    fatal(spanOf(expr), IRDiagnosticCode::Internal,
          "the builtin table names an intrinsic LLVM does not have: " +
              std::string(row->lowering.name));
    return {};
  }

  // The overloaded types, in the order `Intrinsics.td` declares them. Asked of
  // the *row* for `None` and of the argument for `FirstArgument`, which is the
  // one field that says which of the two this intrinsic is.
  llvm::SmallVector<llvm::Type*, 1> overloads;
  if (row->lowering.overload == builtins::Lowering::Overload::FirstArgument) {
    if (args.empty()) {
      fatal(spanOf(expr), IRDiagnosticCode::Internal,
            "`" + std::string(row->spelling) +
                "` is overloaded on its first argument and has none");
      return {};
    }
    llvm::Type* shape = llvmType(typeOf(args.front()));
    if (shape == nullptr) {
      return {};
    }
    overloads.push_back(shape);
  }
  llvm::Function* intrinsic = llvm::Intrinsic::getOrInsertDeclaration(&module_, id, overloads);
  if (intrinsic == nullptr) {
    fatal(spanOf(expr), IRDiagnosticCode::Internal,
          "LLVM has no declaration for `" + std::string(row->lowering.name) + "`");
    return {};
  }

  switch (row->lowering.kind) {
  case builtins::Lowering::Kind::Intrinsic:
    break;
  case builtins::Lowering::Kind::RotateLeft:
  case builtins::Lowering::Kind::RotateRight:
    return lowerRotate(*row, expr, args, arguments, intrinsic);
  }

  // The constant operand the intrinsic's contract requires after the arguments
  // the program wrote. `false` for `clz`/`ctz` is the language's answer for a
  // zero argument, and it is the reason no guard is emitted.
  switch (row->lowering.tail) {
  case builtins::TailOperand::None:
    break;
  case builtins::TailOperand::I1False:
    arguments.push_back(llvm::ConstantInt::getFalse(context_));
    break;
  case builtins::TailOperand::I1True:
    arguments.push_back(llvm::ConstantInt::getTrue(context_));
    break;
  }

  // A `void` intrinsic takes no name, and LLVM refuses one: the same rule the
  // call lowering above follows, and the same reason (`llvm.trap` is the row that
  // reaches it).
  llvm::CallInst* call =
      intrinsic->getReturnType()->isVoidTy()
          ? builder_.CreateCall(intrinsic->getFunctionType(), intrinsic, arguments)
          : builder_.CreateCall(intrinsic->getFunctionType(), intrinsic, arguments, "builtin");
  return Value{call, typeOf(expr)};
}

Value Lowering::lowerRotate(const builtins::BuiltinInfo& row, ast::AstId expr,
                            std::span<const ast::AstId> args, std::span<llvm::Value*> arguments,
                            llvm::Function* intrinsic) {
  if (args.size() < 2 || arguments.size() < 2) {
    fatal(spanOf(expr), IRDiagnosticCode::Internal,
          "`" + std::string(row.spelling) + "` reached lowering without a value and a count");
    return {};
  }
  const sema::TypeId valueType = typeOf(args[0]);
  if (!types_.known(valueType) || !types_.isInteger(valueType)) {
    fatal(spanOf(expr), IRDiagnosticCode::Internal,
          "`" + std::string(row.spelling) + "` was typed with a value that is not an integer");
    return {};
  }
  llvm::Type* shape = llvmType(valueType);
  if (shape == nullptr) {
    return {};
  }

  // The count, wrapped into the value's width and reduced into `[0, width)`.
  //
  // The order is `wrap`, then `urem`, and it is safe for a reason rather than by
  // luck: both widths are powers of two, so reducing modulo the value's width
  // commutes with wrapping the count into it -- which means a count wider than
  // the value, and a *negative* one, both land on the number the language says
  // (the count modulo the width, read as the two's-complement pattern).
  const std::uint16_t bits = types_.get(valueType).bits;
  llvm::Value* count = builder_.CreateZExtOrTrunc(arguments[1], shape, "count");
  count = builder_.CreateURem(
      count, llvm::ConstantInt::get(shape, static_cast<std::uint64_t>(bits)), "rotation");

  llvm::Value* rotated[3] = {arguments[0], arguments[0], count};
  llvm::CallInst* call =
      builder_.CreateCall(intrinsic->getFunctionType(), intrinsic, rotated, "rotate");
  return Value{call, typeOf(expr)};
}

} // namespace minc::ir
