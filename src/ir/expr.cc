// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Expressions, and the two things an expression can denote.
//
// The `Value`/`Place` split (`values.h`) is what keeps this file honest. A
// consumer that wants a value asks for one; the two nodes that are places
// through a pointer (`*p`, `p[i]`) are lowered through `lowerPlace`, which is
// where the access record is consulted -- and nowhere else. An assignment target
// is a `Place`, so `*p = v` and `p[i] = v` reach the same store, and `&x` is a
// `Place` read as an address.
//
// Every conversion here comes from `sema`'s record. `applyBinary` picks the
// instruction for an operator the checker already typed; it never asks whether
// the operator applies, because that question was answered one stage up and the
// answer is in the tree.
#include "lowering.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "llvm/ADT/APInt.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"

#include "ast/node.h"
#include "lex/token_kind.h"
#include "sema/type.h"
#include "sema/type_store.h"
#include "sema/typed_ast.h"
#include "support/consteval/literal.h"
#include "support/intern/sym_id.h"
#include "support/limits.h"

namespace minc::ir {
namespace {

[[nodiscard]] bool isComparison(Tag kind) {
  switch (kind) {
  case kTokEqualEqual:
  case kTokBangEqual:
  case kTokLess:
  case kTokLessEqual:
  case kTokGreater:
  case kTokGreaterEqual:
    return true;
  default:
    return false;
  }
}

// The operator a compound assignment performs. One function, so `x += y` and
// `x + y` cannot choose two different instructions.
[[nodiscard]] Tag plainOperator(Tag assignOp) {
  switch (assignOp) {
  case kTokPlusEqual:
    return kTokPlus;
  case kTokMinusEqual:
    return kTokMinus;
  case kTokStarEqual:
    return kTokStar;
  case kTokSlashEqual:
    return kTokSlash;
  case kTokPercentEqual:
    return kTokPercent;
  case kTokAmpEqual:
    return kTokAmp;
  case kTokPipeEqual:
    return kTokPipe;
  case kTokCaretEqual:
    return kTokCaret;
  case kTokLessLessEqual:
    return kTokLessLess;
  case kTokGreaterGreaterEqual:
    return kTokGreaterGreater;
  default:
    return kTokEqual;
  }
}

} // namespace

// --- the entry point -----------------------------------------------------------

Value Lowering::lowerExpr(ast::AstId expr) {
  if (!expr.valid() || failed_ || inError(expr)) {
    return {};
  }
  // The parser already bounded the tree's depth, and the checker bounded the
  // type store; this is the third belt, and it exists because lowering recurses
  // where the checker did. A refused expression is an internal error and a
  // module that is thrown away, never a stack overflow.
  if (depth_ >= support::kMaxNestingDepth) {
    fatal(spanOf(expr), IRDiagnosticCode::Internal,
          "the expression in this unit nests too deeply to lower");
    return {};
  }
  ++depth_;
  // The node's own line, before anything below it runs: a nested expression
  // overwrites this with its own, so the *innermost* expression that produced an
  // instruction owns its location, which is what a stepping debugger expects.
  locate(expr);

  Value result;
  switch (kindOf(expr)) {
  case ast::NodeKind::LiteralExpr:
    result = lowerLiteral(expr);
    break;
  case ast::NodeKind::PathExpr:
    result = lowerPath(expr);
    break;
  case ast::NodeKind::ParenExpr: {
    const std::vector<ast::AstId> operands = operandsOf(expr);
    result = operands.empty() ? Value{} : lowerExpr(operands.front());
    break;
  }
  case ast::NodeKind::PrefixExpr:
    result = lowerPrefix(expr);
    break;
  case ast::NodeKind::PostfixExpr:
    result = lowerPostfix(expr);
    break;
  case ast::NodeKind::BinaryExpr:
    result = lowerBinary(expr);
    break;
  case ast::NodeKind::ConditionalExpr:
    result = lowerConditional(expr);
    break;
  case ast::NodeKind::AssignExpr:
    result = lowerAssign(expr);
    break;
  case ast::NodeKind::CallExpr:
    result = lowerCall(expr);
    break;
  case ast::NodeKind::IndexExpr:
    result = lowerDerefOrIndex(expr);
    break;
  default:
    fatal(spanOf(expr), IRDiagnosticCode::UnsupportedNode,
          "this expression is not lowered yet: " + std::string(parse::toString(kindOf(expr))));
    break;
  }

  --depth_;
  if (result.v == nullptr) {
    return result;
  }
  // The node's own type is the answer. A worker may compute at a wider operation
  // type (`u8 + u8` is an `add i32`), but the *expression* is the type the
  // checker gave it, and every consumer reads that from the record.
  result.type = typeOf(expr);
  return result;
}

// --- literals ------------------------------------------------------------------

Value Lowering::lowerLiteral(ast::AstId expr) {
  const ast::AstId token = tokenOf(expr);
  if (!token.valid()) {
    fatal(spanOf(expr), IRDiagnosticCode::Internal, "a literal has no token");
    return {};
  }
  const sema::TypeId type = typeOf(expr);
  switch (tagOf(kindOf(token))) {
  case kTokIntegerLiteral:
  case kTokCharLiteral: {
    if (!types_.isInteger(type)) {
      fatal(spanOf(expr), IRDiagnosticCode::Internal,
            "an integer literal reached lowering with a non-integer type");
      return {};
    }
    const std::uint16_t bits = bitsOf(type);
    if (bits == 0) {
      fatal(spanOf(expr), IRDiagnosticCode::Internal, "an integer literal has no width");
      return {};
    }
    if (bits > 64) {
      // Wider than the core's 64 bits: `sema` accepted the spelling against a
      // 128-bit context and deliberately kept no `ConstInt`, so the digits are
      // read here. The reader cannot fail on a checked tree.
      const std::optional<llvm::APInt> wide = wideInteger(expr);
      if (!wide.has_value()) {
        return {};
      }
      return Value{llvm::ConstantInt::get(context_, *wide), type};
    }
    const sema::ExprInfo& info = infoOf(expr);
    if (!info.hasIntValue) {
      fatal(spanOf(expr), IRDiagnosticCode::Internal,
            "an integer literal reached lowering with no value");
      return {};
    }
    // The bits `sema` folded, taken at the target width. `ConstInt::bits` is the
    // two's-complement pattern, so truncation is the right read for a signed
    // literal and the identity for an unsigned one -- no branch on signedness,
    // which is the property that makes `-1` an `i32` of all ones.
    const llvm::APInt raw(bits, info.value.bits);
    return Value{llvm::ConstantInt::get(context_, raw), type};
  }
  case kTokFloatLiteral: {
    const std::optional<llvm::APFloat> value = floatValue(expr);
    if (!value.has_value()) {
      return {};
    }
    return Value{llvm::ConstantFP::get(context_, *value), type};
  }
  case kTokStringLiteral: {
    llvm::Constant* bytes = stringGlobal(expr);
    if (bytes == nullptr) {
      return {};
    }
    return Value{bytes, type};
  }
  default:
    fatal(spanOf(expr), IRDiagnosticCode::Internal, "a literal token of an unknown kind");
    return {};
  }
}

Value Lowering::lowerPath(ast::AstId expr) {
  const std::optional<resolve::DefId> def = defOfPath(expr);
  if (!def.has_value() || def->index >= defs_.defs.size()) {
    fatal(spanOf(expr), IRDiagnosticCode::Internal,
          "a name reached lowering with nothing it resolves to");
    return {};
  }
  const resolve::Def& declaration = defs_.defs[def->index];
  const sema::TypeId type = typeOf(expr);

  // A predefined name is a *value*, not storage: `true`, `false` and `null`
  // denote no object, which is why `&null` is refused one stage up and why
  // nothing here ever makes an alloca for one.
  if (declaration.predefined) {
    const std::string_view name = declaration.name == support::kInvalidSym
                                      ? std::string_view{}
                                      : symbols_.lookup(declaration.name);
    if (name == "true") {
      return Value{llvm::ConstantInt::getTrue(context_), type};
    }
    if (name == "false") {
      return Value{llvm::ConstantInt::getFalse(context_), type};
    }
    if (name == "null") {
      return Value{llvm::ConstantPointerNull::get(pointerType()), type};
    }
    fatal(spanOf(expr), IRDiagnosticCode::Internal,
          "a predefined name this stage does not know reached lowering: `" + std::string(name) +
              "`");
    return {};
  }

  if (declaration.kind == resolve::DefKind::Function) {
    const auto found = functions_.find(defKey(*def));
    if (found == functions_.end()) {
      fatal(spanOf(expr), IRDiagnosticCode::Internal,
            "a function was used before its declaration was lowered");
      return {};
    }
    return Value{found->second, type};
  }

  llvm::AllocaInst* slot = localOf(expr);
  if (slot == nullptr) {
    fatal(spanOf(expr), IRDiagnosticCode::Internal,
          "`" + std::string(spelling(expr)) + "` is not a binding this function owns");
    return {};
  }
  return loadPlace(Place{slot, type}, expr);
}

// --- places --------------------------------------------------------------------

Place Lowering::lowerPlace(ast::AstId expr) {
  if (!expr.valid()) {
    return {};
  }
  locate(expr);
  switch (kindOf(expr)) {
  case ast::NodeKind::PathExpr: {
    llvm::AllocaInst* slot = localOf(expr);
    if (slot == nullptr) {
      fatal(spanOf(expr), IRDiagnosticCode::Internal,
            "this place is not a binding this function owns");
      return {};
    }
    return Place{slot, typeOf(expr)};
  }
  case ast::NodeKind::ParenExpr: {
    const std::vector<ast::AstId> operands = operandsOf(expr);
    if (operands.empty()) {
      fatal(spanOf(expr), IRDiagnosticCode::Internal, "an empty parenthesised place");
      return {};
    }
    return lowerPlace(operands.front());
  }
  case ast::NodeKind::PrefixExpr: {
    // `*p`. The only `PrefixExpr` that is a place is a dereference -- `&x` is an
    // address, not a place, and unary `-x` has no address at all.
    const ast::AstId op = tokenOf(expr);
    const std::vector<ast::AstId> operands = operandsOf(expr);
    if (!op.valid() || operands.empty() || tagOf(kindOf(op)) != kTokStar) {
      fatal(spanOf(expr), IRDiagnosticCode::UnsupportedNode,
            "this expression is not a place: only `*p` and `p[i]` reach memory through a pointer");
      return {};
    }
    const Value pointer = lowerExpr(operands.front());
    if (pointer.v == nullptr) {
      return {};
    }
    const sema::TypeId pointee = types_.pointeeOf(pointer.type);
    if (!types_.known(pointee) || types_.isVoid(pointee)) {
      fatal(spanOf(expr), IRDiagnosticCode::Internal,
            "a dereference reached lowering with no pointee type");
      return {};
    }
    return Place{pointer.v, pointee};
  }
  case ast::NodeKind::IndexExpr: {
    const std::vector<ast::AstId> operands = operandsOf(expr);
    if (operands.size() < 2) {
      fatal(spanOf(expr), IRDiagnosticCode::Internal, "an index has no base or no index");
      return {};
    }
    const Value base = lowerExpr(operands[0]);
    if (base.v == nullptr) {
      return {};
    }
    // The index's conversion to the pointer index width is recorded by `sema`
    // (`checkIndex`), so this reads it rather than choosing a width.
    const Value index = lowerOperand(expr, operands[1]);
    if (index.v == nullptr) {
      return {};
    }
    const sema::TypeId pointee = types_.pointeeOf(base.type);
    if (!types_.known(pointee) || types_.isVoid(pointee)) {
      fatal(spanOf(expr), IRDiagnosticCode::Internal,
            "an index reached lowering with no element type");
      return {};
    }
    llvm::Type* element = llvmType(pointee);
    if (element == nullptr) {
      return {};
    }
    // A *plain* `getelementptr`: `inbounds` is a promise this language does not
    // make, and `ir.md`'s assumption list forbids one without a recorded proof.
    llvm::Value* address = builder_.CreateGEP(element, base.v, {index.v}, "index");
    return Place{address, pointee};
  }
  default:
    fatal(spanOf(expr), IRDiagnosticCode::UnsupportedNode,
          "this expression is not a place this stage lowers: " +
              std::string(parse::toString(kindOf(expr))));
    return {};
  }
}

Value Lowering::loadPlace(const Place& place, ast::AstId placeNode) {
  if (place.addr == nullptr) {
    return {};
  }
  // A place that came through a pointer must have an obligation: the record is
  // how the model's alignment and provenance reach the instruction, and a
  // missing one means the checker stopped recording where it used to.
  if (isAccessNode(placeNode) && obligationFor(placeNode) == nullptr) {
    fatal(spanOf(placeNode), IRDiagnosticCode::MissingObligation,
          "an access through a pointer reached lowering with no access record; the checker "
          "stopped recording where this stage reads the record");
    return {};
  }
  llvm::Type* type = storageType(place.type);
  if (type == nullptr) {
    return {};
  }
  llvm::LoadInst* load = builder_.CreateLoad(type, place.addr, "load");
  // The alignment is *stated* rather than left to the target's default, because
  // it is the number the access record's type gives and the number the scan
  // compares against it. An overestimate is undefined behaviour in LLVM, not
  // slow code, which is why it is one function for reads and writes.
  load->setAlignment(llvm::Align(alignmentOf(place.type)));
  return fromStorage(Value{load, place.type});
}

void Lowering::storePlace(const Place& place, const Value& value, ast::AstId placeNode) {
  if (place.addr == nullptr || value.v == nullptr) {
    return;
  }
  if (isAccessNode(placeNode) && obligationFor(placeNode) == nullptr) {
    fatal(spanOf(placeNode), IRDiagnosticCode::MissingObligation,
          "a store through a pointer reached lowering with no access record; the checker "
          "stopped recording where this stage reads the record");
    return;
  }
  Value stored = value.type == place.type ? value : convert(value, place.type);
  if (stored.v == nullptr) {
    return;
  }
  stored = toStorage(stored);
  if (stored.v == nullptr) {
    return;
  }
  llvm::Type* type = storageType(place.type);
  if (type == nullptr || stored.v->getType() != type) {
    fatal(spanOf(placeNode), IRDiagnosticCode::Internal,
          "a store would write a value of the wrong type");
    return;
  }
  llvm::StoreInst* store = builder_.CreateStore(stored.v, place.addr);
  store->setAlignment(llvm::Align(alignmentOf(place.type)));
}

Value Lowering::toStorage(const Value& value) {
  if (value.v == nullptr) {
    return value;
  }
  llvm::Type* type = storageType(value.type);
  if (type == nullptr || value.v->getType() == type) {
    return value;
  }
  // The one representation difference in the language: a `bool` is an `i1` as a
  // value and a byte as an object (`memory.md`, *Objects*).
  if (value.v->getType() == boolType() && type == byteType()) {
    return Value{builder_.CreateZExt(value.v, byteType(), "bool.store"), value.type};
  }
  return value;
}

Value Lowering::fromStorage(const Value& value) {
  if (value.v == nullptr) {
    return value;
  }
  // Compared against the type's *natural* shape and not against its storage
  // shape: a loaded `bool` arrives here as an `i8` and has to become an `i1`,
  // and the value's language type is the only thing that says so.
  llvm::Type* natural = llvmType(value.type);
  if (natural == nullptr || value.v->getType() == natural) {
    return value;
  }
  if (value.v->getType() == byteType() && natural == boolType()) {
    return Value{builder_.CreateTrunc(value.v, boolType(), "bool.load"), value.type};
  }
  return value;
}

// --- prefixes and postfixes ------------------------------------------------------

Value Lowering::lowerPrefix(ast::AstId expr) {
  const ast::AstId op = tokenOf(expr);
  const std::vector<ast::AstId> operands = operandsOf(expr);
  if (!op.valid() || operands.empty()) {
    fatal(spanOf(expr), IRDiagnosticCode::Internal, "a unary operator has no operand");
    return {};
  }
  const ast::AstId operand = operands.front();
  const Tag kind = tagOf(kindOf(op));

  if (kind == kTokAmp) {
    // The address of a place. This reads nothing: `&x` on an unassigned `x` is
    // legal, and the definite-assignment pass walks the operand as a place for
    // exactly this reason.
    const Place place = lowerPlace(operand);
    if (place.addr == nullptr) {
      return {};
    }
    return Value{place.addr, typeOf(expr)};
  }
  if (kind == kTokStar) {
    return lowerDerefOrIndex(expr);
  }

  if (kind == kTokPlusPlus || kind == kTokMinusMinus) {
    const Place place = lowerPlace(operand);
    if (place.addr == nullptr) {
      return {};
    }
    const Value old = loadPlace(place, operand);
    if (old.v == nullptr) {
      return {};
    }
    const bool increment = kind == kTokPlusPlus;
    Value next;
    if (types_.isPointer(old.type)) {
      const sema::TypeId isizeType = pointerIntType();
      next =
          pointerOffset(old, Value{llvm::ConstantInt::get(indexType(), 1), isizeType}, !increment);
    } else {
      llvm::Value* one = llvm::ConstantInt::get(old.v->getType(), 1);
      next = Value{increment ? static_cast<llvm::Value*>(builder_.CreateAdd(old.v, one, "inc"))
                             : static_cast<llvm::Value*>(builder_.CreateSub(old.v, one, "dec")),
                   old.type};
    }
    if (next.v == nullptr) {
      return {};
    }
    storePlace(place, next, operand);
    return next;
  }

  const Value value = lowerOperand(expr, operand);
  if (value.v == nullptr) {
    return {};
  }
  const sema::TypeId result = typeOf(expr);
  switch (kind) {
  case kTokMinus:
    return Value{builder_.CreateNeg(value.v, "neg"), result};
  case kTokPlus:
    return value;
  case kTokTilde:
    return Value{builder_.CreateNot(value.v, "not"), result};
  case kTokBang:
    return Value{builder_.CreateNot(value.v, "lnot"), result};
  default:
    fatal(spanOf(expr), IRDiagnosticCode::UnsupportedNode,
          "this unary operator is not lowered yet");
    return {};
  }
}

Value Lowering::lowerPostfix(ast::AstId expr) {
  const std::vector<ast::AstId> operands = operandsOf(expr);
  const ast::AstId op = tokenOf(expr);
  if (!op.valid() || operands.empty()) {
    fatal(spanOf(expr), IRDiagnosticCode::Internal, "a postfix operator has no operand");
    return {};
  }
  const ast::AstId operand = operands.front();
  const bool increment = tagOf(kindOf(op)) == kTokPlusPlus;

  const Place place = lowerPlace(operand);
  if (place.addr == nullptr) {
    return {};
  }
  const Value old = loadPlace(place, operand);
  if (old.v == nullptr) {
    return {};
  }
  Value next;
  if (types_.isPointer(old.type)) {
    const sema::TypeId isizeType = pointerIntType();
    next = pointerOffset(old, Value{llvm::ConstantInt::get(indexType(), 1), isizeType}, !increment);
  } else {
    llvm::Value* one = llvm::ConstantInt::get(old.v->getType(), 1);
    next = Value{increment ? static_cast<llvm::Value*>(builder_.CreateAdd(old.v, one, "inc"))
                           : static_cast<llvm::Value*>(builder_.CreateSub(old.v, one, "dec")),
                 old.type};
  }
  if (next.v == nullptr) {
    return {};
  }
  storePlace(place, next, operand);
  // The *old* value: that is the whole difference between `x++` and `++x`.
  return old;
}

Value Lowering::lowerDerefOrIndex(ast::AstId expr) {
  const Place place = lowerPlace(expr);
  if (place.addr == nullptr) {
    return {};
  }
  return loadPlace(place, expr);
}

// --- binaries --------------------------------------------------------------------

llvm::CmpInst::Predicate Lowering::unsignedPredicate(Tag op) const {
  switch (op) {
  case kTokEqualEqual:
    return llvm::CmpInst::ICMP_EQ;
  case kTokBangEqual:
    return llvm::CmpInst::ICMP_NE;
  case kTokLess:
    return llvm::CmpInst::ICMP_ULT;
  case kTokLessEqual:
    return llvm::CmpInst::ICMP_ULE;
  case kTokGreater:
    return llvm::CmpInst::ICMP_UGT;
  case kTokGreaterEqual:
    return llvm::CmpInst::ICMP_UGE;
  default:
    return llvm::CmpInst::ICMP_EQ;
  }
}

llvm::CmpInst::Predicate Lowering::signedPredicate(Tag op) const {
  switch (op) {
  case kTokEqualEqual:
    return llvm::CmpInst::ICMP_EQ;
  case kTokBangEqual:
    return llvm::CmpInst::ICMP_NE;
  case kTokLess:
    return llvm::CmpInst::ICMP_SLT;
  case kTokLessEqual:
    return llvm::CmpInst::ICMP_SLE;
  case kTokGreater:
    return llvm::CmpInst::ICMP_SGT;
  case kTokGreaterEqual:
    return llvm::CmpInst::ICMP_SGE;
  default:
    return llvm::CmpInst::ICMP_EQ;
  }
}

Value Lowering::applyBinary(Tag op, const Value& lhs, const Value& rhs, sema::TypeId opType,
                            ast::AstId at) {
  if (lhs.v == nullptr || rhs.v == nullptr) {
    return {};
  }
  const bool fp = types_.isFloat(lhs.type);

  if (isComparison(op)) {
    if (fp) {
      llvm::CmpInst::Predicate predicate = llvm::CmpInst::FCMP_OEQ;
      switch (op) {
      case kTokBangEqual:
        predicate = llvm::CmpInst::FCMP_UNE;
        break;
      case kTokLess:
        predicate = llvm::CmpInst::FCMP_OLT;
        break;
      case kTokLessEqual:
        predicate = llvm::CmpInst::FCMP_OLE;
        break;
      case kTokGreater:
        predicate = llvm::CmpInst::FCMP_OGT;
        break;
      case kTokGreaterEqual:
        predicate = llvm::CmpInst::FCMP_OGE;
        break;
      default:
        break;
      }
      return Value{builder_.CreateFCmp(predicate, lhs.v, rhs.v, "fcmp"), sema::kTypeBool};
    }
    // Pointers compare as *addresses*, with provenance ignored, which is what
    // `icmp` on a pointer does (`memory.md`, decision 16). Ordering compares the
    // unsigned address; equality is equality.
    const bool pointer = types_.isPointer(lhs.type) || types_.isPointer(rhs.type);
    const llvm::CmpInst::Predicate predicate =
        pointer ? unsignedPredicate(op)
                : (isSigned(lhs.type) ? signedPredicate(op) : unsignedPredicate(op));
    return Value{builder_.CreateICmp(predicate, lhs.v, rhs.v, pointer ? "pcmp" : "cmp"),
                 sema::kTypeBool};
  }

  switch (op) {
  case kTokPlus:
    return Value{fp ? static_cast<llvm::Value*>(builder_.CreateFAdd(lhs.v, rhs.v, "fadd"))
                    : static_cast<llvm::Value*>(builder_.CreateAdd(lhs.v, rhs.v, "add")),
                 opType};
  case kTokMinus:
    return Value{fp ? static_cast<llvm::Value*>(builder_.CreateFSub(lhs.v, rhs.v, "fsub"))
                    : static_cast<llvm::Value*>(builder_.CreateSub(lhs.v, rhs.v, "sub")),
                 opType};
  case kTokStar:
    return Value{fp ? static_cast<llvm::Value*>(builder_.CreateFMul(lhs.v, rhs.v, "fmul"))
                    : static_cast<llvm::Value*>(builder_.CreateMul(lhs.v, rhs.v, "mul")),
                 opType};
  case kTokSlash:
    if (fp) {
      return Value{builder_.CreateFDiv(lhs.v, rhs.v, "fdiv"), opType};
    }
    return checkedDiv(lhs, rhs, opType, /*isRemainder=*/false);
  case kTokPercent:
    if (fp) {
      fatal(spanOf(at), IRDiagnosticCode::Internal,
            "`%` reached lowering with float operands; the checker does not permit it");
      return {};
    }
    return checkedDiv(lhs, rhs, opType, /*isRemainder=*/true);
  case kTokAmp:
    return Value{builder_.CreateAnd(lhs.v, rhs.v, "and"), opType};
  case kTokPipe:
    return Value{builder_.CreateOr(lhs.v, rhs.v, "or"), opType};
  case kTokCaret:
    return Value{builder_.CreateXor(lhs.v, rhs.v, "xor"), opType};
  case kTokLessLess:
    return checkedShift(lhs, rhs, opType, /*left=*/true);
  case kTokGreaterGreater:
    return checkedShift(lhs, rhs, opType, /*left=*/false);
  default:
    fatal(spanOf(at), IRDiagnosticCode::UnsupportedNode, "this operator is not lowered yet");
    return {};
  }
}

Value Lowering::lowerBinary(ast::AstId expr) {
  const ast::AstId op = tokenOf(expr);
  const std::vector<ast::AstId> operands = operandsOf(expr);
  if (!op.valid() || operands.size() < 2) {
    fatal(spanOf(expr), IRDiagnosticCode::Internal, "a binary operator has no two operands");
    return {};
  }
  const Tag kind = tagOf(kindOf(op));
  const ast::AstId lhsNode = operands[0];
  const ast::AstId rhsNode = operands[1];

  if (kind == kTokAmpAmp || kind == kTokPipePipe) {
    return lowerLogical(expr, kind, lhsNode, rhsNode);
  }

  const Value left = lowerOperand(expr, lhsNode);
  const Value right = lowerOperand(expr, rhsNode);
  if (left.v == nullptr || right.v == nullptr) {
    return {};
  }

  // The pointer arms, before the arithmetic one: exactly one of the two can
  // claim the operator, and a pointer handed to `applyBinary` would hit the
  // integer path for `+`.
  if (kind == kTokMinus && types_.isPointer(left.type) && types_.isPointer(right.type)) {
    return pointerDifference(left, right);
  }
  if ((kind == kTokPlus || kind == kTokMinus) && types_.isPointer(left.type) &&
      !types_.isPointer(right.type)) {
    return pointerOffset(left, right, kind == kTokMinus);
  }
  if (kind == kTokPlus && !types_.isPointer(left.type) && types_.isPointer(right.type)) {
    // `n + p` and `p + n` are the same operation; the language just spells the
    // pointer first when it is written that way round.
    return pointerOffset(right, left, /*negate=*/false);
  }

  // The operation type `sema` decided for this operator. For an arithmetic,
  // bitwise or comparison operator it is the operands' common type; the
  // conversions to it are already recorded, so both sides arrive here at it.
  return applyBinary(kind, left, right, left.type, expr);
}

Value Lowering::lowerLogical(ast::AstId expr, Tag kind, ast::AstId lhsNode, ast::AstId rhsNode) {
  // `&&` and `||` short-circuit, and short-circuiting is *semantics*, not an
  // optimisation: `f() && g()` calls `g` only when `f` is true, and the module
  // has to say so. A plain `and i1` would evaluate both, which is a different
  // program -- and the one thing an optimizer is allowed to notice here is the
  // evaluation order, so it is written with blocks.
  const Value left = lowerOperand(expr, lhsNode);
  // Captured *after* the left operand: the operand is an expression like any
  // other and may itself be a `&&` that ends in a block of its own, so the
  // predecessor of the join is wherever the left's evaluation actually stopped.
  llvm::BasicBlock* entry = builder_.GetInsertBlock();
  if (left.v == nullptr || entry == nullptr) {
    return {};
  }
  llvm::Value* test = left.v;
  if (test->getType() != boolType()) {
    test = builder_.CreateICmpNE(test, llvm::ConstantInt::get(test->getType(), 0), "truth");
  }

  llvm::BasicBlock* rhsBB = llvm::BasicBlock::Create(context_, "logic.rhs", current_);
  llvm::BasicBlock* endBB = llvm::BasicBlock::Create(context_, "logic.end", current_);
  const bool isAnd = kind == kTokAmpAmp;
  builder_.CreateCondBr(test, isAnd ? rhsBB : endBB, isAnd ? endBB : rhsBB);

  builder_.SetInsertPoint(rhsBB);
  const Value right = lowerOperand(expr, rhsNode);
  llvm::BasicBlock* rhsEnd = builder_.GetInsertBlock();
  if (right.v == nullptr) {
    return {};
  }
  branchTo(endBB);

  builder_.SetInsertPoint(endBB);
  llvm::PHINode* phi = builder_.CreatePHI(boolType(), 2, "logic");
  // The value when the right side is not evaluated: `false` for `&&`, `true`
  // for `||`. Both are the `i1` the language's `bool` maps to.
  phi->addIncoming(llvm::ConstantInt::get(boolType(), isAnd ? 0 : 1), entry);
  phi->addIncoming(right.v, rhsEnd);
  return Value{phi, sema::kTypeBool};
}

Value Lowering::lowerConditional(ast::AstId expr) {
  const std::vector<ast::AstId> operands = operandsOf(expr);
  if (operands.size() < 3) {
    fatal(spanOf(expr), IRDiagnosticCode::Internal, "`?:` has fewer than three operands");
    return {};
  }
  const ast::AstId condition = operands[0];
  const ast::AstId thenExpr = operands[1];
  const ast::AstId elseExpr = operands[2];

  llvm::BasicBlock* thenBB = llvm::BasicBlock::Create(context_, "cond.then", current_);
  llvm::BasicBlock* elseBB = llvm::BasicBlock::Create(context_, "cond.else", current_);
  llvm::BasicBlock* endBB = llvm::BasicBlock::Create(context_, "cond.end", current_);

  const Value conditionValue = lowerExpr(condition);
  llvm::Value* test = conditionValue.v;
  if (test != nullptr && test->getType() != boolType()) {
    test = builder_.CreateICmpNE(test, llvm::ConstantInt::get(test->getType(), 0), "truth");
  }
  if (test != nullptr) {
    builder_.CreateCondBr(test, thenBB, elseBB);
  }

  builder_.SetInsertPoint(thenBB);
  const Value thenValue = lowerOperand(expr, thenExpr);
  llvm::BasicBlock* thenEnd = builder_.GetInsertBlock();
  branchTo(endBB);

  builder_.SetInsertPoint(elseBB);
  const Value elseValue = lowerOperand(expr, elseExpr);
  llvm::BasicBlock* elseEnd = builder_.GetInsertBlock();
  branchTo(endBB);

  builder_.SetInsertPoint(endBB);
  if (thenValue.v == nullptr || elseValue.v == nullptr) {
    return {};
  }
  llvm::Type* type = llvmType(typeOf(expr));
  if (type == nullptr || type->isVoidTy()) {
    // A `void` conditional has no value to merge; the arms were lowered for
    // their effects and that is the whole expression.
    return {};
  }
  llvm::PHINode* phi = builder_.CreatePHI(type, 2, "cond");
  phi->addIncoming(thenValue.v, thenEnd);
  phi->addIncoming(elseValue.v, elseEnd);
  return Value{phi, typeOf(expr)};
}

// --- assignment ------------------------------------------------------------------

Value Lowering::lowerAssign(ast::AstId expr) {
  const ast::AstId op = tokenOf(expr);
  const std::vector<ast::AstId> operands = operandsOf(expr);
  if (!op.valid() || operands.size() < 2) {
    fatal(spanOf(expr), IRDiagnosticCode::Internal, "an assignment has no two operands");
    return {};
  }
  const Tag kind = tagOf(kindOf(op));
  const ast::AstId lhsNode = operands[0];
  const ast::AstId rhsNode = operands[1];
  const sema::TypeId target = typeOf(expr);

  const Place place = lowerPlace(lhsNode);
  if (place.addr == nullptr) {
    return {};
  }

  if (kind == kTokEqual) {
    const Value value = lowerOperand(expr, rhsNode);
    if (value.v == nullptr) {
      return {};
    }
    storePlace(place, value, lhsNode);
    // The value of an assignment is the stored value, at the target's type --
    // which is why the conversion result is what is returned and not the raw
    // operand.
    const Value stored = value.type == target ? value : convert(value, target);
    return stored;
  }

  if (types_.isPointer(target)) {
    // `p += n` / `p -= n`: the pointer keeps its type, and the offset is already
    // materialised at the index width by the record.
    const Value current = loadPlace(place, lhsNode);
    const Value offset = lowerOperand(expr, rhsNode);
    if (current.v == nullptr || offset.v == nullptr) {
      return {};
    }
    const Value next = pointerOffset(current, offset, kind == kTokMinusEqual);
    if (next.v == nullptr) {
      return {};
    }
    storePlace(place, next, lhsNode);
    return next;
  }

  // The operation type. It is *not* the assignment's type: `x <<= 9` on a `u16`
  // is defined at `i32`, and an `AssignExpr`'s own type is the `u16` the store
  // truncates back to -- a lowering that read the assignment's type would emit an
  // out-of-range shift, which is a poison value and not a crash.
  const sema::TypeId opType = infoOf(expr).opType;
  if (!types_.known(opType) || types_.isError(opType)) {
    fatal(spanOf(expr), IRDiagnosticCode::Internal,
          "a compound assignment reached lowering with no operation type");
    return {};
  }
  const Value current = loadPlace(place, lhsNode);
  if (current.v == nullptr) {
    return {};
  }
  const Value currentOp = convert(current, opType);
  const Value rhsOp = lowerOperand(expr, rhsNode);
  if (currentOp.v == nullptr || rhsOp.v == nullptr) {
    return {};
  }
  const Value result = applyBinary(plainOperator(kind), currentOp, rhsOp, opType, expr);
  if (result.v == nullptr) {
    return {};
  }
  // Back to the binding's type, which is where the language's wrapping lives:
  // the operation happened at `opType` and the *store* truncates.
  const Value stored = convert(result, target);
  if (stored.v == nullptr) {
    return {};
  }
  storePlace(place, stored, lhsNode);
  return stored;
}

// --- calls -----------------------------------------------------------------------

Value Lowering::lowerCall(ast::AstId expr) {
  const std::vector<ast::AstId> operands = operandsOf(expr);
  if (operands.empty()) {
    fatal(spanOf(expr), IRDiagnosticCode::Internal, "a call has no callee");
    return {};
  }
  const ast::AstId callee = operands.front();
  const Value calleeValue = lowerExpr(callee);
  if (calleeValue.v == nullptr) {
    return {};
  }
  llvm::Type* mapped = llvmFunctionType(typeOf(callee));
  auto* functionType = llvm::dyn_cast_or_null<llvm::FunctionType>(mapped);
  if (functionType == nullptr) {
    fatal(spanOf(expr), IRDiagnosticCode::Internal,
          "the callee of this call is not a function type");
    return {};
  }

  std::vector<llvm::Value*> arguments;
  if (operands.size() > 1) {
    for (const ast::AstId argument : operandsOf(operands[1])) {
      const Value value = lowerOperand(expr, argument);
      if (value.v == nullptr) {
        return {};
      }
      arguments.push_back(value.v);
    }
  }
  // A call to a `void` function provides no value, and LLVM refuses an
  // instruction that has a name but no value -- so the name is written only when
  // there is something to name.
  llvm::CallInst* call = functionType->getReturnType()->isVoidTy()
                             ? builder_.CreateCall(functionType, calleeValue.v, arguments)
                             : builder_.CreateCall(functionType, calleeValue.v, arguments, "call");
  return Value{call, typeOf(expr)};
}

// --- pointer arithmetic -----------------------------------------------------------

Value Lowering::pointerOffset(const Value& pointer, const Value& offset, bool negate) {
  if (pointer.v == nullptr || offset.v == nullptr) {
    return {};
  }
  const sema::TypeId pointee = types_.pointeeOf(pointer.type);
  if (!types_.known(pointee) || types_.isVoid(pointee)) {
    fatal(support::Span{}, IRDiagnosticCode::Internal,
          "a pointer step reached lowering with no stride");
    return {};
  }
  llvm::Type* element = llvmType(pointee);
  if (element == nullptr) {
    return {};
  }
  Value index = offset;
  if (index.v->getType() != indexType()) {
    index = convert(index, pointerIntType());
    if (index.v == nullptr) {
      return {};
    }
  }
  if (negate) {
    index = Value{builder_.CreateNeg(index.v, "neg"), index.type};
  }
  llvm::Value* address = builder_.CreateGEP(element, pointer.v, {index.v}, "ptr.offset");
  return Value{address, pointer.type};
}

Value Lowering::pointerDifference(const Value& lhs, const Value& rhs) {
  if (lhs.v == nullptr || rhs.v == nullptr) {
    return {};
  }
  const sema::TypeId pointee = types_.pointeeOf(lhs.type);
  const sema::TypeId isizeType = pointerIntType();
  // The byte distance between two addresses, then divided by the stride. The
  // model defines the result only when both pointers name one allocation -- the
  // checked build is what finds the other case -- and the type is `isize`
  // either way.
  llvm::Value* left = builder_.CreatePtrToInt(lhs.v, indexType(), "p2i");
  llvm::Value* right = builder_.CreatePtrToInt(rhs.v, indexType(), "p2i");
  llvm::Value* bytes = builder_.CreateSub(left, right, "pbytes");
  llvm::Type* element = llvmType(pointee);
  const std::uint64_t stride =
      element == nullptr ? 1 : layout_.getTypeAllocSize(element).getFixedValue();
  if (stride <= 1) {
    return Value{bytes, isizeType};
  }
  llvm::Value* divisor = llvm::ConstantInt::get(indexType(), stride);
  return Value{builder_.CreateSDiv(bytes, divisor, "pdiff"), isizeType};
}

} // namespace minc::ir
