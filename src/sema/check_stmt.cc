// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Declarations, statements, types, and the lookup the checker does into the
// resolution below it.
//
// The order here is the order the language needs, and it is the same argument
// `resolve.md` makes one stage down: **signatures first, bodies second**. A
// body may call a function written after it, so every declaration's type has to
// exist before the first statement is checked. That is `runSignatures`, and it
// is why an unresolved forward reference is impossible rather than merely
// unlikely.
#include "checker.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sema/convert.h"
#include "sema/typespec.h"
#include "support/text/edit_distance.h"

namespace minc::sema {
namespace {

[[nodiscard]] std::uint64_t keyOf(support::FileId file, std::uint32_t begin) {
  return (static_cast<std::uint64_t>(file) << 32U) | begin;
}

} // namespace

Checker::Checker(const ast::LoweredFile& file, const resolve::DefMap& defs,
                 const support::Interner& symbols, TypeStore& types, SemaOptions options)
    : file_(file), defs_(defs), symbols_(symbols), types_(types), options_(options) {
  // The type budget is enforced by the store, because the store is where the
  // allocation happens; the option is how a caller lowers it for this check.
  types_.setMaxTypes(options_.maxTypes);
  defTypes_.assign(defs_.defs.size(), kTypeError);
  defConstValues_.assign(defs_.defs.size(), support::ConstInt{});
  defHasConstValue_.assign(defs_.defs.size(), false);
  defIsConst_.assign(defs_.defs.size(), false);

  // Name uses, indexed by where they were written in the unit's text. The
  // reference array is already in source order; this turns "which declaration
  // does this `PathExpr` denote?" into one lookup instead of a scan per node.
  refByOffset_.reserve(defs_.refs.size());
  for (std::size_t i = 0; i < defs_.refs.size(); ++i) {
    const resolve::NameRef& ref = defs_.refs[i];
    const auto [it, inserted] =
        refByOffset_.emplace(keyOf(ref.unitSpan.file, ref.unitSpan.begin), i);
    // Two references starting at one offset cannot happen for a well-formed
    // unit (a byte belongs to one token); if it ever did, the first is the one
    // written first, and the answer stays a function of the input.
    (void)it;
    (void)inserted;
  }

  // Declarations, indexed for the reverse direction: from a declaration's
  // `Name` node to the def the resolver created for it.
  //
  // This is the indexed form of `resolve::defOfNameNode` -- the same rule, keyed
  // on the *unit* offset instead of scanned, because this stage asks it once per
  // declaration and the scan would be quadratic in the unit's size. The rule:
  // the unit offset is one token each, while the written span is not, because a
  // macro can give two names one written location (see `Def::unitSpan`). If the
  // rule changes, both places change.
  //
  // A predefined name has no declaration to point at, so it is left out rather
  // than parked at offset 0 where a real name could land.
  for (std::size_t i = 0; i < defs_.defs.size(); ++i) {
    const resolve::Def& def = defs_.defs[i];
    if (def.predefined) {
      continue;
    }
    defByNameOffset_.emplace(keyOf(def.unitSpan.file, def.unitSpan.begin),
                             resolve::DefId{def.unitSpan.file, static_cast<std::uint32_t>(i)});
  }

  // The language's predefined names. `resolve` bound them; their *types* are
  // this stage's to decide, and they are `bool`.
  for (std::size_t i = 0; i < defs_.defs.size(); ++i) {
    const resolve::Def& def = defs_.defs[i];
    if (!def.predefined) {
      continue;
    }
    defTypes_[i] = kTypeBool;
    defIsConst_[i] = true;
    if (symbols_.lookup(def.name) == "true" || symbols_.lookup(def.name) == "false") {
      defHasConstValue_[i] = true;
      defConstValues_[i] =
          support::ConstInt::fromSigned(symbols_.lookup(def.name) == "true" ? 1 : 0);
    }
  }
}

// --- text --------------------------------------------------------------------

std::string Checker::valueText(support::ConstInt value) {
  return value.isUnsigned ? std::to_string(value.bits) : std::to_string(value.signedValue());
}

// --- artifact writes ---------------------------------------------------------

void Checker::setType(ast::AstId id, TypeId type) {
  if (!id.valid() || id.index >= out_.typed.typeTable.size()) {
    return;
  }
  out_.typed.typeTable[id.index] = type;
}

void Checker::setExpr(ast::AstId id, const ExprInfo& info) {
  if (!id.valid() || id.index >= out_.typed.exprFacts.size()) {
    return;
  }
  out_.typed.exprFacts[id.index] = info;
}

// --- diagnostics -------------------------------------------------------------

void Checker::error(ast::AstId at, SemaErrorCode code, std::string message) {
  errorAt(origin(at), code, std::move(message));
}

void Checker::errorAt(support::Span span, SemaErrorCode code, std::string message) {
  // The bag caps retention one layer up; the vector is capped here for the same
  // reason, so a pathological unit costs bounded memory in the stage too.
  if (out_.errors.size() >= support::kMaxDiagnostics) {
    return;
  }
  SemaError issue;
  issue.span = span;
  issue.message = std::move(message);
  issue.code = code;
  out_.errors.push_back(std::move(issue));
}

void Checker::warning(ast::AstId at, SemaErrorCode code, std::string message) {
  if (out_.warnings.size() >= support::kMaxDiagnostics) {
    return;
  }
  SemaError issue;
  issue.span = origin(at);
  issue.message = std::move(message);
  issue.code = code;
  out_.warnings.push_back(std::move(issue));
}

void Checker::attachNote(SemaErrorCode code, support::Span span, std::string note) {
  for (auto it = out_.errors.rbegin(); it != out_.errors.rend(); ++it) {
    if (it->code == code && it->note.empty()) {
      it->note = std::move(note);
      it->noteSpan = span;
      return;
    }
  }
}

void Checker::reportLimit(ast::AstId at) {
  if (limitReported_) {
    return;
  }
  limitReported_ = true;
  error(at, SemaErrorCode::LimitTypes,
        "the unit has too many distinct types to check (" + std::to_string(options_.maxTypes) +
            " allowed)");
}

// --- tree access -------------------------------------------------------------

std::vector<ast::AstId> Checker::operandsOf(ast::AstId id) const {
  std::vector<ast::AstId> out;
  if (!id.valid()) {
    return out;
  }
  for (const ast::AstId child : file_.childrenOf(id)) {
    if (!file_.at(child).isToken()) {
      out.push_back(child);
    }
  }
  return out;
}

ast::AstId Checker::tokenOf(ast::AstId id) const {
  if (!id.valid()) {
    return ast::AstId{};
  }
  for (const ast::AstId child : file_.childrenOf(id)) {
    if (file_.at(child).isToken()) {
      return child;
    }
  }
  return ast::AstId{};
}

bool Checker::enterDepth() {
  if (depth_ >= support::kMaxNestingDepth) {
    if (!limitReported_) {
      limitReported_ = true;
      errorAt(support::Span{}, SemaErrorCode::LimitTypes,
              "the expression nests too deeply to check");
    }
    return false;
  }
  ++depth_;
  return true;
}

// --- declaration lookup ------------------------------------------------------

std::optional<resolve::DefId> Checker::defAtName(ast::AstId nameNode) const {
  if (!nameNode.valid()) {
    return std::nullopt;
  }
  // The unit offset first: it is one token each, so it identifies the
  // declaration even when the preprocessor gave two names one written location.
  const support::Span unit = file_.at(nameNode).unit;
  const auto found = defByNameOffset_.find(keyOf(unit.file, unit.begin));
  if (found != defByNameOffset_.end()) {
    return found->second;
  }
  // A synthetic declaration (an inserted token) may have no unit range; fall
  // back to the written span, which is exact whenever nothing was expanded.
  const support::Span span = origin(nameNode);
  for (std::size_t i = 0; i < defs_.defs.size(); ++i) {
    const resolve::Def& def = defs_.defs[i];
    if (def.predefined || def.nameSpan.file != span.file) {
      continue;
    }
    if (def.nameSpan.begin >= span.begin && def.nameSpan.end <= span.end) {
      return resolve::DefId{span.file, static_cast<std::uint32_t>(i)};
    }
  }
  return std::nullopt;
}

const resolve::Def* Checker::defFor(resolve::DefId id) const {
  if (!id.valid() || id.index >= defs_.defs.size()) {
    return nullptr;
  }
  return &defs_.defs[id.index];
}

TypeId Checker::typeOfDef(resolve::DefId id) const {
  if (!id.valid() || id.index >= defTypes_.size()) {
    return kTypeError;
  }
  return defTypes_[id.index];
}

bool Checker::isConstDef(resolve::DefId id) const {
  if (!id.valid() || id.index >= defIsConst_.size()) {
    return false;
  }
  return defIsConst_[id.index];
}

std::optional<resolve::DefId> Checker::defOfPath(ast::AstId pathExpr) const {
  const support::Span unit = file_.at(pathExpr).unit;
  const auto found = refByOffset_.find(keyOf(unit.file, unit.begin));
  if (found == refByOffset_.end()) {
    return std::nullopt;
  }
  const resolve::NameRef& ref = defs_.refs[found->second];
  if (!ref.resolved()) {
    return std::nullopt;
  }
  return ref.target;
}

std::optional<resolve::DefId> Checker::defOfPlace(ast::AstId expr) const {
  ast::AstId current = expr;
  while (current.valid()) {
    switch (kindOf(current)) {
    case ast::NodeKind::PathExpr:
      return defOfPath(current);
    case ast::NodeKind::ParenExpr: {
      const std::vector<ast::AstId> operands = operandsOf(current);
      if (operands.empty()) {
        return std::nullopt;
      }
      current = operands.front();
      break;
    }
    default:
      return std::nullopt;
    }
  }
  return std::nullopt;
}

std::string Checker::nameOf(ast::AstId expr) const {
  // Parentheses around a name are not part of the name; a message that says
  // "`(c)` is a `const`" would be pointing at a spelling the source never
  // really used for the declaration.
  ast::AstId current = expr;
  while (current.valid() && kindOf(current) == ast::NodeKind::ParenExpr) {
    const std::vector<ast::AstId> operands = operandsOf(current);
    if (operands.empty()) {
      return "this expression";
    }
    current = operands.front();
  }
  if (current.valid() && kindOf(current) == ast::NodeKind::PathExpr) {
    return std::string(spelling(current));
  }
  return "this expression";
}

// --- types -------------------------------------------------------------------

std::vector<std::string_view> Checker::typeWords(ast::AstId typeNode) const {
  std::vector<std::string_view> words;
  if (!typeNode.valid()) {
    return words;
  }
  for (const ast::AstId child : file_.childrenOf(typeNode)) {
    if (file_.at(child).is(kIdentifierNode)) {
      words.push_back(file_.spellingOf(child));
    }
  }
  return words;
}

std::string Checker::suggestTypeName(std::string_view word) const {
  std::string best;
  std::uint32_t bestDistance = support::kMaxSuggestionDistance + 1;
  for (const std::string_view candidate : typeNames()) {
    if (candidate == word) {
      continue;
    }
    const std::size_t diff = candidate.size() > word.size() ? candidate.size() - word.size()
                                                            : word.size() - candidate.size();
    if (diff > support::kMaxSuggestionDistance) {
      continue;
    }
    const std::uint32_t distance =
        support::boundedEditDistance(word, candidate, support::kMaxSuggestionDistance);
    // Strictly closer wins; the table order breaks a tie, so the answer is a
    // function of the input and not of a container's order.
    if (distance < bestDistance) {
      bestDistance = distance;
      best = std::string(candidate);
    }
  }
  return best;
}

TypeId Checker::resolveTypeNode(ast::AstId typeNode) {
  if (!typeNode.valid() || inError(typeNode)) {
    return kTypeError;
  }
  const std::vector<std::string_view> words = typeWords(typeNode);
  const TypeSpecResult spec = readTypeSpec(words, types_);
  if (!spec.ok) {
    if (spec.unknownWord.empty()) {
      error(typeNode, SemaErrorCode::MalformedType, spec.message);
    } else {
      error(typeNode, SemaErrorCode::UnknownType, spec.message);
      const std::string suggestion = suggestTypeName(spec.unknownWord);
      if (!suggestion.empty()) {
        // Point the note at the word that was wrong, not at the whole run.
        support::Span wordSpan = origin(typeNode);
        for (const ast::AstId child : file_.childrenOf(typeNode)) {
          if (file_.at(child).is(kIdentifierNode) && file_.spellingOf(child) == spec.unknownWord) {
            wordSpan = origin(child);
            break;
          }
        }
        attachNote(SemaErrorCode::UnknownType, wordSpan, "did you mean `" + suggestion + "`?");
      }
    }
    setType(typeNode, kTypeError);
    return kTypeError;
  }
  if (!spec.type.valid()) {
    reportLimit(typeNode);
    setType(typeNode, kTypeError);
    return kTypeError;
  }
  setType(typeNode, spec.type);
  return spec.type;
}

TypeId Checker::defaultValue(TypeId type) {
  return types_.isDeferred(type) ? types_.defaultOf(type) : type;
}

TypeId Checker::adaptTo(TypeId type, TypeId expected, ast::AstId at, const ExprInfo& info) {
  if (!expected.valid() || types_.isError(expected) || types_.isError(type)) {
    return type;
  }
  const Type& shape = types_.get(type);
  if (shape.kind == TypeKind::IntLiteral) {
    if (types_.isFloat(expected)) {
      // `let x: f64 = 1;` -- the conversion is the assignment's, but taking the
      // context's type here keeps the tree's answer the one that will be used.
      return expected;
    }
    if (types_.isInteger(expected)) {
      if (info.hasIntValue && !fitsIn(types_, expected, info.value)) {
        // Two mistakes, two sentences. A literal too large for its type is one
        // token the reader wrote; a value that came out of folding
        // (`2147483647 + 1`) is a constant expression, and saying "this integer
        // literal" about it would blame a token the program never wrote. Both
        // are errors: the runtime operation is defined to wrap, but a constant
        // the compiler can see does not fit is a mistake it can point at, and
        // every language that defines wrapping still refuses this one.
        if (kindOf(at) == ast::NodeKind::LiteralExpr) {
          error(at, SemaErrorCode::LiteralOutOfRange,
                "this integer literal does not fit in `" + types_.spelling(expected) + "`");
        } else {
          error(at, SemaErrorCode::ConstantOutOfRange,
                "this constant expression evaluates to `" + valueText(info.value) +
                    "`, which does not fit in `" + types_.spelling(expected) + "`");
        }
        return kTypeError;
      }
      return expected;
    }
    return type;
  }
  if (shape.kind == TypeKind::FloatLiteral) {
    return types_.isFloat(expected) ? expected : type;
  }
  return type;
}

void Checker::checkAssignable(TypeId from, TypeId to, ast::AstId at, SemaErrorCode code,
                              std::string_view what) {
  if (types_.isError(from) || types_.isError(to)) {
    return;
  }
  if (convertible(types_, from, to)) {
    if (options_.warnConversion && narrows(types_, from, to)) {
      warning(at, SemaErrorCode::ImplicitConversion,
              "implicit conversion from `" + types_.spelling(from) + "` to `" +
                  types_.spelling(to) + "` may lose information");
    }
    return;
  }
  if (types_.isVoid(to) || types_.isVoid(from)) {
    error(at, code, "`void` is not a value");
    return;
  }
  const bool toBool = types_.get(to).kind == TypeKind::Bool;
  if (toBool && types_.isArithmetic(from)) {
    error(at, code,
          "`" + types_.spelling(from) + "` does not convert to `bool`; write `" +
              std::string(what) + " != 0`");
    return;
  }
  error(at, code,
        "`" + types_.spelling(from) + "` cannot be used as `" + types_.spelling(to) + "`" +
            std::string(what));
}

// --- the unit ----------------------------------------------------------------

SemaOutput Checker::run() {
  out_.typed.typeTable.assign(file_.nodeCount(), kTypeError);
  out_.typed.exprFacts.assign(file_.nodeCount(), ExprInfo{});

  runSignatures();
  for (const FunctionInfo& info : out_.typed.functionTable) {
    checkFunction(info);
  }
  return std::move(out_);
}

void Checker::runSignatures() {
  for (const ast::AstId decl : operandsOf(file_.root())) {
    if (kindOf(decl) != ast::NodeKind::FnDecl || inError(decl)) {
      continue;
    }
    const ast::AstId typeNode = childOf(decl, ast::NodeKind::Type);
    const ast::AstId nameNode = childOf(decl, ast::NodeKind::Name);
    const ast::AstId body = childOf(decl, ast::NodeKind::Block);

    TypeId returnType = kTypeError;
    if (typeNode.valid()) {
      returnType = resolveTypeNode(typeNode);
    }

    // Parameters. The type is read here, in the signature pass, and written to
    // the parameter's own definition, so a use anywhere in the body -- and a
    // call, which compares against the function's type -- sees a real type
    // rather than whatever the walk happened to reach first.
    std::vector<TypeId> params;
    const ast::AstId paramList = childOf(decl, ast::NodeKind::ParamList);
    if (paramList.valid() && !inError(paramList)) {
      for (const ast::AstId param : operandsOf(paramList)) {
        if (kindOf(param) != ast::NodeKind::Param) {
          continue; // the `Error` slot for a parameter the parser could not read
        }
        const ast::AstId paramType = childOf(param, ast::NodeKind::Type);
        TypeId declared = paramType.valid() ? resolveTypeNode(paramType) : kTypeError;
        if (declared.valid() && types_.isVoid(declared)) {
          // A parameter is an object, so `void` names nothing it can be. The
          // message cannot be written by the type reader, which has no idea
          // where the type was written.
          error(paramType.valid() ? paramType : param, SemaErrorCode::TypeNotValue,
                "`void` is not a type a parameter can have");
          declared = kTypeError;
        }
        params.push_back(declared);
        if (const std::optional<resolve::DefId> def =
                defAtName(childOf(param, ast::NodeKind::Name))) {
          if (def->index < defTypes_.size()) {
            defTypes_[def->index] = declared;
            // A parameter is a mutable binding: a function that cannot assign to
            // its own parameter would have to copy it into a `let` first.
            defIsConst_[def->index] = false;
          }
        }
      }
    }

    TypeId functionType = types_.function(returnType, params);
    if (!functionType.valid()) {
      reportLimit(decl);
      functionType = kTypeError;
    }

    FunctionInfo info;
    info.decl = decl;
    info.functionType = functionType;
    info.returnType = returnType;
    info.body = body;
    info.name = nameNode.valid() ? file_.at(nameNode).name : support::kInvalidSym;
    out_.typed.functionTable.push_back(info);

    setType(decl, functionType);
    if (nameNode.valid()) {
      setType(nameNode, functionType);
    }
    if (const std::optional<resolve::DefId> def = defAtName(nameNode)) {
      if (def->index < defTypes_.size()) {
        defTypes_[def->index] = functionType;
        defIsConst_[def->index] = false;
      }
    }

    // `main` is the one name the language reserves a shape for. A program with
    // no `main` is not an error here: this stage sees one translation unit, and
    // the entry point is a program property, which is `link`'s.
    if (info.name != support::kInvalidSym && symbols_.lookup(info.name) == "main" &&
        !types_.isError(returnType) && returnType != kTypeI32) {
      error(nameNode, SemaErrorCode::MainSignature,
            "`main` must be declared `fn i32 main()`; this one returns `" +
                types_.spelling(returnType) + "`");
    }
  }
}

void Checker::checkFunction(const FunctionInfo& info) {
  if (!info.body.valid() || inError(info.body)) {
    return;
  }
  currentReturn_ = info.returnType;
  currentFunctionName_ = info.name;
  checkBody(info.body, info.returnType);

  // Definite assignment runs over the body the walk just finished, because the
  // one thing it needs from the walk -- the folded value of a condition, to know
  // whether a loop can fall out of the bottom -- is only complete now. It is a
  // separate pass and not a thread through the walk: see `check_flow.cc`.
  checkDefiniteAssignment(info.body);

  if (!types_.isVoid(info.returnType) && !types_.isError(info.returnType) &&
      !terminates(info.body)) {
    const ast::AstId nameNode = childOf(info.decl, ast::NodeKind::Name);
    const std::string name = info.name == support::kInvalidSym
                                 ? std::string("this function")
                                 : std::string(symbols_.lookup(info.name));
    error(nameNode.valid() ? nameNode : info.decl, SemaErrorCode::MissingReturn,
          "`" + name + "` returns `" + types_.spelling(info.returnType) +
              "`, so it cannot reach the end without returning a value");
  }
  currentReturn_ = kTypeError;
  currentFunctionName_ = support::kInvalidSym;
}

bool Checker::terminates(ast::AstId stmt) const {
  if (!stmt.valid()) {
    return false;
  }
  if (inError(stmt)) {
    // The parser already reported this region; saying the function "falls off
    // the end" as well would be a second diagnostic for one mistake.
    return true;
  }
  switch (kindOf(stmt)) {
  case ast::NodeKind::ReturnStmt:
    return true;
  case ast::NodeKind::Block:
    // A block terminates when *some* statement in it does: the statements after
    // that one are unreachable, so they cannot change the answer.
    for (const ast::AstId child : file_.childrenOf(stmt)) {
      if (!file_.at(child).isToken() && terminates(child)) {
        return true;
      }
    }
    return false;
  case ast::NodeKind::IfStmt: {
    // Both arms, or it is not a guarantee. `if (x) return 1;` falls through when
    // `x` is false, and pretending otherwise would accept a function that can
    // reach its end without a value.
    const ast::AstId thenBlock = childOf(stmt, ast::NodeKind::Block);
    const ast::AstId elseClause = childOf(stmt, ast::NodeKind::ElseClause);
    if (!thenBlock.valid() || !elseClause.valid() || !terminates(thenBlock)) {
      return false;
    }
    for (const ast::AstId arm : file_.childrenOf(elseClause)) {
      if (!file_.at(arm).isToken()) {
        return terminates(arm);
      }
    }
    return false;
  }
  case ast::NodeKind::WhileStmt:
  case ast::NodeKind::ForStmt:
    return loopsForever(stmt);
  default:
    // An expression statement, a binding, an empty statement, a jump: control
    // always reaches the next one.
    return false;
  }
}

bool Checker::loopsForever(ast::AstId stmt) const {
  ast::AstId condition;
  if (kindOf(stmt) == ast::NodeKind::WhileStmt) {
    for (const ast::AstId operand : operandsOf(stmt)) {
      if (kindOf(operand) != ast::NodeKind::Block) {
        condition = operand;
        break;
      }
    }
  } else {
    const std::vector<ast::AstId> clause = operandsOf(childOf(stmt, ast::NodeKind::ForCondition));
    if (!clause.empty()) {
      condition = clause.front();
    }
  }

  // No condition at all is the language's spelling of `true`: `for ;; {}` and
  // `while true {}` are the same loop, and both leave only through `return`.
  if (condition.valid()) {
    const std::optional<bool> value = constantCondition(condition);
    if (!value.has_value() || !*value) {
      return false;
    }
  }

  // ... and only if nothing in the body can leave it early. A `break` aimed at
  // this loop makes falling out of the bottom possible, and control then
  // continues after the loop.
  const ast::AstId body = childOf(stmt, ast::NodeKind::Block);
  return body.valid() && !hasBreakForThisLoop(body);
}

bool Checker::hasBreakForThisLoop(ast::AstId node) const {
  if (!node.valid() || inError(node)) {
    return false;
  }
  switch (kindOf(node)) {
  case ast::NodeKind::BreakStmt:
    return true;
  case ast::NodeKind::WhileStmt:
  case ast::NodeKind::ForStmt:
    // A `break` inside belongs to that loop, not to the one we are asking about,
    // so the scan stops here rather than descending.
    return false;
  case ast::NodeKind::IfStmt:
  case ast::NodeKind::ElseClause:
  case ast::NodeKind::Block:
    for (const ast::AstId child : file_.childrenOf(node)) {
      if (!file_.at(child).isToken() && hasBreakForThisLoop(child)) {
        return true;
      }
    }
    return false;
  default:
    return false;
  }
}

std::optional<bool> Checker::constantCondition(ast::AstId condition) const {
  if (!condition.valid()) {
    return std::nullopt;
  }
  const ExprInfo& facts = out_.typed.infoOf(condition);
  if (!facts.isConstant || !facts.hasIntValue) {
    // A float condition is impossible (`bool` is required) and a `bool` constant
    // folds to 0 or 1, so "a constant with no integer value" is a condition this
    // stage could not resolve -- and saying so is better than guessing.
    return std::nullopt;
  }
  return std::optional<bool>(facts.value.truthy());
}

// --- control flow ------------------------------------------------------------

void Checker::checkCondition(ast::AstId condition, std::string_view what) {
  const TypeId type = checkExpr(condition, kInvalidType);
  if (types_.isError(type)) {
    return;
  }
  if (types_.get(type).kind != TypeKind::Bool) {
    error(condition, SemaErrorCode::ConditionNotBool,
          "the condition of `" + std::string(what) + "` must be `bool`; `" + types_.spelling(type) +
              "` is not one");
  }
}

void Checker::checkIf(ast::AstId stmt, TypeId returnType) {
  const ast::AstId thenBlock = childOf(stmt, ast::NodeKind::Block);
  const ast::AstId elseClause = childOf(stmt, ast::NodeKind::ElseClause);

  // The condition is whichever direct child is neither of those two, which is
  // how the expression is found without counting children.
  for (const ast::AstId operand : operandsOf(stmt)) {
    if (operand != thenBlock && operand != elseClause) {
      checkCondition(operand, "if");
      break;
    }
  }
  if (thenBlock.valid()) {
    checkBlock(thenBlock, returnType);
  }
  // An `else` arm is a block or another `if`; nothing else can get in, because
  // that is the only thing the grammar accepts after `else`.
  for (const ast::AstId arm : operandsOf(elseClause)) {
    if (kindOf(arm) == ast::NodeKind::Block) {
      checkBlock(arm, returnType);
    } else if (kindOf(arm) == ast::NodeKind::IfStmt) {
      checkIf(arm, returnType);
    }
  }
}

void Checker::checkWhile(ast::AstId stmt, TypeId returnType) {
  const ast::AstId body = childOf(stmt, ast::NodeKind::Block);
  for (const ast::AstId operand : operandsOf(stmt)) {
    if (operand != body) {
      checkCondition(operand, "while");
      break;
    }
  }
  if (body.valid()) {
    // `break` and `continue` are checked against this counter, so one declared
    // outside any loop is an error rather than a jump to nowhere.
    ++loopDepth_;
    checkBlock(body, returnType);
    --loopDepth_;
  }
}

void Checker::checkFor(ast::AstId stmt, TypeId returnType) {
  // The clauses are read by kind: the initializer is a statement, and the
  // condition and the step are wrapped in their own kinds precisely so that a
  // reader does not have to rely on their order.
  ast::AstId init;
  ast::AstId body;
  const std::vector<ast::AstId> condition = operandsOf(childOf(stmt, ast::NodeKind::ForCondition));
  const std::vector<ast::AstId> step = operandsOf(childOf(stmt, ast::NodeKind::ForStep));
  for (const ast::AstId child : operandsOf(stmt)) {
    switch (kindOf(child)) {
    case ast::NodeKind::LetStmt:
    case ast::NodeKind::ConstStmt:
    case ast::NodeKind::ExprStmt:
    case ast::NodeKind::EmptyStmt:
      init = child;
      break;
    case ast::NodeKind::Block:
      body = child;
      break;
    default:
      break;
    }
  }

  if (init.valid()) {
    checkStatement(init, returnType);
  }
  if (!condition.empty()) {
    checkCondition(condition.front(), "for");
  }
  if (!step.empty()) {
    // The step's value is discarded, so any type is acceptable -- the same rule
    // an expression statement follows.
    (void)checkExpr(step.front(), kInvalidType);
  }
  if (body.valid()) {
    ++loopDepth_;
    checkBlock(body, returnType);
    --loopDepth_;
  }
}

void Checker::checkBody(ast::AstId block, TypeId returnType) {
  checkBlock(block, returnType);
}

void Checker::checkBlock(ast::AstId block, TypeId returnType) {
  bool terminated = false;
  bool reported = false;
  for (const ast::AstId stmt : operandsOf(block)) {
    if (terminated && !reported && !inError(stmt)) {
      reported = true;
      // "does not complete" rather than "returns": a `return` is one way a
      // statement fails to fall through, and control flow now has more than one
      // -- a loop that cannot leave, and an `if` whose every arm is one of these.
      warning(stmt, SemaErrorCode::UnreachableCode,
              "this statement can never be reached, because the statement before it never "
              "completes");
    }
    checkStatement(stmt, returnType);
    if (terminates(stmt)) {
      terminated = true;
    }
  }
}

void Checker::checkStatement(ast::AstId stmt, TypeId returnType) {
  if (!stmt.valid() || inError(stmt)) {
    return;
  }
  switch (kindOf(stmt)) {
  case ast::NodeKind::LetStmt:
  case ast::NodeKind::ConstStmt: {
    const bool isConst = kindOf(stmt) == ast::NodeKind::ConstStmt;
    const ast::AstId nameNode = childOf(stmt, ast::NodeKind::Name);
    const ast::AstId typeNode = childOf(stmt, ast::NodeKind::Type);

    TypeId declared = kInvalidType;
    if (typeNode.valid()) {
      declared = resolveTypeNode(typeNode);
    }
    // `void` is refused *before* the initializer is checked, and only once: a
    // value that "cannot be assigned to `void`" would be a second sentence about
    // the same mistake, and the mistake is the declaration, not the value.
    bool voidDeclared = false;
    if (declared.valid() && types_.isVoid(declared)) {
      error(typeNode.valid() ? typeNode : stmt, SemaErrorCode::TypeNotValue,
            "`void` is not a type an object can have");
      declared = kInvalidType;
      voidDeclared = true;
    }

    ast::AstId init = ast::AstId{};
    for (const ast::AstId operand : operandsOf(stmt)) {
      if (operand != nameNode && operand != typeNode) {
        init = operand;
      }
    }
    TypeId initType = kInvalidType;
    if (init.valid()) {
      initType = checkExpr(init, declared);
    }

    TypeId binding = kTypeError;
    if (declared.valid()) {
      binding = declared;
      if (init.valid() && initType.valid()) {
        checkAssignable(initType, declared, init, SemaErrorCode::InvalidAssignment,
                        " in this initializer");
      }
    } else if (initType.valid() && !voidDeclared) {
      // A binding whose type is *inferred* can still turn out to be `void`:
      // `let x = f();` where `f` returns nothing. It is the same mistake as
      // `let x: void`, caught one step later, and it is reported once.
      if (types_.isVoid(initType)) {
        error(init, SemaErrorCode::TypeNotValue,
              "this expression is `void`, so it is not a value an object can have");
      } else {
        binding = defaultValue(initType);
      }
    }

    setType(stmt, binding);
    if (nameNode.valid()) {
      setType(nameNode, binding);
    }
    if (const std::optional<resolve::DefId> def = defAtName(nameNode)) {
      if (def->index < defTypes_.size()) {
        defTypes_[def->index] = binding;
        defIsConst_[def->index] = isConst;
        if (isConst && init.valid()) {
          const ExprInfo& facts = out_.typed.infoOf(init);
          if (facts.isConstant && facts.hasIntValue) {
            defConstValues_[def->index] = facts.value;
            defHasConstValue_[def->index] = true;
          }
        }
      }
    }
    return;
  }
  case ast::NodeKind::ReturnStmt: {
    const std::vector<ast::AstId> operands = operandsOf(stmt);
    const ast::AstId expr = operands.empty() ? ast::AstId{} : operands.front();
    if (expr.valid()) {
      if (types_.isVoid(currentReturn_)) {
        error(stmt, SemaErrorCode::ReturnVoidValue,
              "this function returns `void`, so `return` takes no value");
        (void)checkExpr(expr, kInvalidType);
        return;
      }
      const TypeId type = checkExpr(expr, currentReturn_);
      checkAssignable(type, currentReturn_, expr, SemaErrorCode::ReturnMismatch,
                      " as the return value of a function returning `" +
                          types_.spelling(currentReturn_) + "`");
      return;
    }
    if (!types_.isVoid(currentReturn_) && !types_.isError(currentReturn_)) {
      error(stmt, SemaErrorCode::ReturnMissingValue,
            "this function returns `" + types_.spelling(currentReturn_) +
                "`, so `return` needs a value");
    }
    return;
  }
  case ast::NodeKind::ExprStmt: {
    const std::vector<ast::AstId> operands = operandsOf(stmt);
    if (!operands.empty()) {
      (void)checkExpr(operands.front(), kInvalidType);
    }
    return;
  }
  case ast::NodeKind::Block:
    checkBlock(stmt, returnType);
    return;
  case ast::NodeKind::IfStmt:
    checkIf(stmt, returnType);
    return;
  case ast::NodeKind::WhileStmt:
    checkWhile(stmt, returnType);
    return;
  case ast::NodeKind::ForStmt:
    checkFor(stmt, returnType);
    return;
  case ast::NodeKind::BreakStmt:
    if (loopDepth_ == 0) {
      error(stmt, SemaErrorCode::BreakOutsideLoop,
            "`break` is only valid inside a loop: there is nothing here to leave");
    }
    return;
  case ast::NodeKind::ContinueStmt:
    if (loopDepth_ == 0) {
      error(stmt, SemaErrorCode::ContinueOutsideLoop,
            "`continue` is only valid inside a loop: there is nothing here to "
            "continue");
    }
    return;
  case ast::NodeKind::EmptyStmt:
    return;
  default:
    // An `Error` node was filtered above; anything else at statement position is
    // grammar the checker does not know, and silently yielding the poison is
    // better than a crash or a diagnostic about a node the parser invented.
    setType(stmt, kTypeError);
    return;
  }
}

} // namespace minc::sema
