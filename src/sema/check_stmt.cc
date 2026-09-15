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
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sema/convert.h"
#include "sema/typespec.h"
#include "support/consteval/literal.h"
#include "support/text/edit_distance.h"

namespace minc::sema {
namespace {

// The word that makes a type unfit for an object, or empty when the type is fit.
//
// Two words, one rule. `void` names no value at all, `!` names a value that
// never arrives, and either way there is nothing for an object to hold -- so both
// are refused in the two positions that make an object (a parameter and a
// binding) with the word in the sentence, because the reader wrote one of the
// two and has to know which. The rule lives here rather than at either call site
// so the two cannot come to different answers about the same type.
[[nodiscard]] std::string_view notAnObjectWord(const TypeStore& types, TypeId type) {
  if (types.isVoid(type)) {
    return "void";
  }
  if (types.isNever(type)) {
    return "!";
  }
  return {};
}

} // namespace

Checker::Checker(const ast::LoweredFile& file, const resolve::DefMap& defs,
                 const support::Interner& symbols, TypeStore& types, SemaOptions options)
    : file_(file), defs_(defs), symbols_(symbols), types_(types), options_(options), index_(defs) {
  // The type budget is enforced by the store, because the store is where the
  // allocation happens; the option is how a caller lowers it for this check.
  types_.setMaxTypes(options_.maxTypes);
  defTypes_.assign(defs_.defs.size(), kTypeError);
  defConstValues_.assign(defs_.defs.size(), support::ConstInt{});
  defHasConstValue_.assign(defs_.defs.size(), false);
  defIsConst_.assign(defs_.defs.size(), false);

  // The language's predefined names. `resolve` bound them; their *types* are
  // this stage's to decide -- and the switch is over *which* name it is, not
  // over its spelling, so the list has one home (`resolve/predefined.h`) and a
  // name added there fails to compile here until its type is decided.
  for (std::size_t i = 0; i < defs_.defs.size(); ++i) {
    const resolve::Def& def = defs_.defs[i];
    switch (def.predefined) {
    case resolve::Predefined::None:
      continue;
    case resolve::Predefined::Null:
      // `null` is `*void`, and that is not a shortcut: `*void` is the one
      // pointer type that converts to every other one, so `null` is usable
      // wherever a pointer is wanted without a nullable-pointer type, a special
      // literal type or a rule that makes an integer zero a pointer. It is the
      // same shape the model gives the untyped pointer for `malloc`
      // (`memory.md`, *The surface*).
      defTypes_[i] = types_.pointerTo(kTypeVoid);
      break;
    case resolve::Predefined::False:
    case resolve::Predefined::True:
      defTypes_[i] = kTypeBool;
      defHasConstValue_[i] = true;
      defConstValues_[i] =
          support::ConstInt::fromSigned(def.predefined == resolve::Predefined::True ? 1 : 0);
      break;
    }
    // Every predefined name denotes a value and not storage, so none of them can
    // be assigned to. That is one property of the whole class and not of a row:
    // it is written once, outside the switch.
    defIsConst_[i] = true;
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
  return index_.defAtName(file_, nameNode);
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
  return index_.targetAt(file_.at(pathExpr).unit);
}

std::optional<resolve::DefId> Checker::defOfStoreTarget(ast::AstId expr) const {
  ast::AstId current = expr;
  while (current.valid() && kindOf(current) == ast::NodeKind::ParenExpr) {
    const std::vector<ast::AstId> operands = operandsOf(current);
    current = operands.empty() ? ast::AstId{} : operands.front();
  }
  return current.valid() && kindOf(current) == ast::NodeKind::PathExpr ? defOfPath(current)
                                                                       : std::nullopt;
}

std::optional<resolve::DefId> Checker::defOfPlace(ast::AstId expr) const {
  // The path half first, parentheses included.
  if (const std::optional<resolve::DefId> direct = defOfStoreTarget(expr); direct.has_value()) {
    return direct;
  }
  ast::AstId current = expr;
  while (current.valid() && kindOf(current) == ast::NodeKind::ParenExpr) {
    const std::vector<ast::AstId> operands = operandsOf(current);
    current = operands.empty() ? ast::AstId{} : operands.front();
  }
  if (!current.valid() || kindOf(current) != ast::NodeKind::IndexExpr) {
    return std::nullopt;
  }
  // `a[i]` is a place **inside** the object `a` names, so it answers with `a`'s
  // declaration: that is what makes `TABLE[0] = 1` and `&TABLE[0]` refused on the
  // same grounds `TABLE = ...` is (`arrays.md` decision 24).
  //
  // The chain stops at a **pointer**, and that asymmetry is the model's and not an
  // oversight: `p[i]` is `*(p + i)`, a write through a pointer value, and
  // `memory.md` decision 15 already says a `const` pointer's bytes may be written
  // through it. The base's type decides which of the two this is -- and the
  // recursion is what makes `a[0][1]` belong to `a` as well.
  const std::vector<ast::AstId> operands = operandsOf(current);
  if (operands.empty() || !types_.isArray(out_.typed.typeOf(operands.front()))) {
    return std::nullopt;
  }
  return defOfPlace(operands.front());
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

std::vector<TypePart> Checker::typeParts(ast::AstId typeNode) const {
  std::vector<TypePart> parts;
  if (!typeNode.valid()) {
    return parts;
  }
  const std::span<const ast::AstId> children = file_.childrenOf(typeNode);
  for (std::size_t i = 0; i < children.size(); ++i) {
    const ast::AstId child = children[i];
    // A word first, and before the token test below: an `Identifier` *is* a token
    // (leaves and interior nodes share one tag space), so asking "is it a token"
    // first would throw every word away.
    if (file_.at(child).is(kIdentifierNode)) {
      TypePart word;
      word.word = file_.spellingOf(child);
      parts.push_back(word);
      continue;
    }
    // The punctuators a type position can hold: `*`, `!` and one `[N]` group.
    // Anything else the builder left inside the type node is not part of a type,
    // and the grammar accepted nothing else here either -- so it is skipped
    // rather than guessed at.
    if (!file_.at(child).isToken()) {
      continue;
    }
    const Tag tag = tagOf(kindOf(child));
    if (tag == kTokStar || tag == kTokBang) {
      TypePart punctuation;
      // Exactly one of the two, which is what `readType` reads them by: a `*` is
      // a prefix over the words, and a `!` is a whole type on its own.
      punctuation.isStar = tag == kTokStar;
      punctuation.isBang = tag == kTokBang;
      parts.push_back(punctuation);
      continue;
    }
    if (tag == kTokLBracket) {
      // `[`, an optional count, `]` -- **one** part, because the group is the
      // constructor and the count belongs to it. The count is folded here, where
      // its spelling is, so that everything downstream compares a number: two
      // spellings of one count are one type, and the store never has to know how
      // the source wrote it (`arrays.md` decision 19).
      TypePart array;
      array.isArray = true;
      if (i + 1 < children.size() && kindOf(children[i + 1]) == kIdentifierNode &&
          file_.spellingOf(children[i + 1]) == parse::kInferredCount) {
        // `[_]`: the count is the initializer's, so nothing is folded here and
        // `hasCount` stays false -- which is what makes `[_]` a *different part*
        // from `[]` and not the same one with a flag missing.
        array.countInferred = true;
        ++i;
      } else if (i + 1 < children.size() && tagOf(kindOf(children[i + 1])) == kTokIntegerLiteral) {
        array.hasCount = true;
        const support::IntegerLiteral count = support::parseIntegerLiteral(
            file_.spellingOf(children[i + 1]), support::IntegerBaseRule::DecimalLeadingZero);
        // `bits` rather than a cast of the value: the count is a `uint64_t` and
        // the reader's negative case is its own error, which the same reader
        // reports here as "does not fit".
        if (count.ok) {
          array.count = count.value.bits;
        } else {
          array.countOverflow = true;
        }
        ++i;
      }
      // A `]` that is not there is the parser's finding, which is why the group
      // is closed by what is found rather than by what is expected: this loop
      // reads the tree it was given, not the tree that should have been built.
      if (i + 1 < children.size() && tagOf(kindOf(children[i + 1])) == kTokRBracket) {
        ++i;
      }
      parts.push_back(array);
    }
  }
  return parts;
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
  const std::vector<TypePart> parts = typeParts(typeNode);
  const TypeSpecResult spec = readType(parts, types_);
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
      // An integer literal in a **float** context keeps its own class: the class
      // of a number is the class of its spelling, so the `1` stays an integer and
      // the *consumer* reports that it cannot be an `f64` (`convertible`).
      //
      // Returning the context's type here would be worse than useless: the literal
      // node would name a floating value, the consumer would see two `f64`s and
      // say nothing, and the value would then have to be rounded from its digits
      // by the lowering -- a rule that stage does not own (`ir.md`). One answer
      // for both directions, and it is this one.
      return type;
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

std::string Checker::mixingAdvice(TypeId from) const {
  // The rule first, in one clause, and then the fix for *this* direction. The
  // sentence is appended to a prefix that already names the two types (`\`i32\`
  // does not convert to \`f64\`` / "`+` cannot combine `i32` and `f64`"), so it
  // names the classes and not the types: repeating them is how a message becomes
  // something to skim instead of something to read.
  const std::string rule =
      "an integer and a float are different classes of number and do not convert into each "
      "other";
  if (types_.isFloat(from)) {
    return rule +
           " -- write the integer you mean (`1`, not `1.0`), or a cast once the language has "
           "one";
  }
  return rule + " -- write the value in the class you want, as in `1.0` for a float";
}

void Checker::checkAssignable(TypeId from, TypeId to, ast::AstId at, SemaErrorCode code,
                              std::string_view what) {
  if (types_.isError(from) || types_.isError(to)) {
    return;
  }
  // An integer and a float, before the general rule: this is not a narrowing to
  // be warned about, it is a conversion the language does not have, and the
  // sentence has to say what to write instead.
  if (mixedNumberPair(types_, from, to)) {
    error(at, code,
          "`" + types_.spelling(from) + "` does not convert to `" + types_.spelling(to) + "`" +
              std::string(what) + ": " + mixingAdvice(from));
    return;
  }
  // An array and a pointer, refused before the general rule for the same reason
  // the two number classes are: it is not a narrowing to warn about, it is the
  // conversion this language deliberately does not have, and the sentence has to
  // say what to write instead (`arrays.md` decision 2). Decay is the mechanism
  // behind `sizeof a` being a pointer's size in a C function and behind no C
  // compiler being able to refuse `a[10]`; the cost here is two characters.
  if (types_.isArray(from) && types_.isPointer(to)) {
    error(at, code,
          "`" + types_.spelling(from) + "` does not convert to `" + types_.spelling(to) + "`" +
              std::string(what) +
              ": an array does not decay to a pointer -- write `&a[0]` for a pointer "
              "to its first element, or `&a` for the whole array");
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
    // Where the mistake is comes *before* the advice, as it does in the two
    // refusals below: "in this initializer" is part of the sentence about this
    // conversion, and a sentence that trails off into advice reads as if the
    // advice were the mistake's subject.
    //
    // The message cannot name the expression -- the checker has a type and a
    // node, not the text the reader wrote -- so it shows the *shape* of the fix
    // instead of a placeholder with a hole in it.
    error(at, code,
          "`" + types_.spelling(from) + "` does not convert to `bool`" + std::string(what) +
              ": write the comparison you mean, as in `value != 0`");
    return;
  }
  // A pointer and a non-pointer, in either direction. The message is its own
  // because this is the model's central refusal rather than a type mismatch:
  // `memory.md` states that a pointer is not an integer, that neither conversion
  // is implicit, and that the two operations which do join them are named and
  // counted. Until they are in the grammar, the refusal is the whole rule.
  if (types_.isPointer(from) != types_.isPointer(to)) {
    error(at, SemaErrorCode::PointerInteger,
          "`" + types_.spelling(from) + "` does not convert to `" + types_.spelling(to) + "`" +
              std::string(what) +
              ": a pointer is not an integer, and the language has no implicit conversion "
              "between the two");
    return;
  }
  // Two pointer types that do not meet: the same refusal `p == q` makes, so the
  // same code and the same rule. `memory.md` *defines* the reinterpretation --
  // memory has no effective type, so punning is not undefined -- but it is never
  // implicit. The one crossing point that is implicit is `*void`, and anything
  // else would make the type of a store depend on a silent reinterpretation
  // rather than on one the source wrote.
  if (types_.isPointer(from) && types_.isPointer(to)) {
    error(at, SemaErrorCode::PointerMismatch,
          "`" + types_.spelling(from) + "` cannot be used as `" + types_.spelling(to) + "`" +
              std::string(what) +
              ": pointers convert implicitly only to the same pointee type, or through "
              "`*void`");
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
  // The file scope next, and before any body, for the reason the signature pass
  // comes first: it is decided once, in dependency order, and everything that
  // reads it reads the same answer. A body checked before it would fold nothing
  // and could not disagree -- it would simply miss what it was allowed to do.
  checkGlobals();
  for (const FunctionInfo& info : out_.typed.functionTable) {
    checkFunction(info);
  }
  // The artifact leaves this stage with no deferred type anywhere in it. It is
  // the property the IR is built on -- a deferred type has no width and no LLVM
  // mapping -- and it is a *sweep* rather than a per-seam promise so that a path
  // nobody has written yet cannot break it. The per-seam decisions came first and
  // are what make the sweep a no-op for everything already handled.
  decideDeferredTypes();
  // The coercion list is sorted and indexed last, once, over the final table:
  // `from` has to be the operand's *final* type, and the per-node index is what
  // makes the lowering's lookup a constant-time question.
  out_.typed.buildCoercionIndex(file_.nodeCount());
  return std::move(out_);
}

void Checker::runSignatures() {
  // The first declaration seen for each function, by canonical def -- the index
  // into `out_.typed.functionTable` of the declaration that got there first.
  //
  // Local to the pass, because it is the pass's own bookkeeping: the answer it
  // exists to produce (the signature a name has) is published in `defTypes_`,
  // and nothing outside these few hundred lines asks "which declaration came
  // first".
  std::unordered_map<std::uint32_t, std::size_t> firstDeclaration;

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
    // Where each parameter's type was written, in the same order as `params`, so
    // the `extern` rule below can point at the type instead of at the whole
    // declaration -- and so it does not have to resolve a type node twice, which
    // would be a second chance to report the same mistake.
    std::vector<ast::AstId> paramTypeNodes;
    const ast::AstId paramList = childOf(decl, ast::NodeKind::ParamList);
    // `...` is part of the signature and not an extra: the marker is a child of
    // the list (the grammar only lets a declaration have one), and the type it
    // produces is a *different* type from the same parameters without it.
    //
    // A variadic list cannot be in an error region while its parameters are
    // readable, so the marker is asked for unconditionally: the parser reported
    // a bad marker in a bad list, and this stage does not repeat it.
    const bool variadic =
        paramList.valid() && childOf(paramList, ast::NodeKind::VariadicParam).valid();
    if (paramList.valid() && !inError(paramList)) {
      for (const ast::AstId param : operandsOf(paramList)) {
        if (kindOf(param) != ast::NodeKind::Param) {
          continue; // the `Error` slot for a parameter the parser could not read
        }
        const ast::AstId paramType = childOf(param, ast::NodeKind::Type);
        TypeId declared = paramType.valid() ? resolveTypeNode(paramType) : kTypeError;
        // A parameter is an object, so neither of the two value-less types names
        // anything it can be. The message cannot be written by the type reader,
        // which has no idea where the type was written.
        if (const std::string_view word = notAnObjectWord(types_, declared); !word.empty()) {
          error(paramType.valid() ? paramType : param, SemaErrorCode::TypeNotValue,
                "`" + std::string(word) + "` is not a type a parameter can have");
          declared = kTypeError;
        }
        params.push_back(declared);
        paramTypeNodes.push_back(paramType.valid() ? paramType : param);
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

    // **The boundary, refused by name where it cannot be honoured**
    // (`arrays.md` decision 11). An array crosses this language's functions by
    // *value*: the caller copies it and passes a pointer to the copy, and a
    // return writes into a destination the caller hands over. That shape is this
    // compiler's own -- nothing outside promises it -- so a declaration whose
    // definition lives somewhere this compiler is not looking must not claim it.
    //
    // Refusing here rather than at the call site is the difference between a
    // diagnostic and wrong code: the callee was compiled by a C compiler, the
    // argument would be a pointer to a copy it is not expecting, and the program
    // would link, run, and read the wrong bytes.
    //
    // A declaration with no body *is* the `extern` spelling: the parser refuses a
    // function with neither a body nor the word, so the missing block is the one
    // fact, and asking the AST for a flag it does not carry would be a second
    // spelling of the same thing.
    if (!body.valid()) {
      if (types_.isAggregate(returnType)) {
        error(typeNode.valid() ? typeNode : decl, SemaErrorCode::ExternAggregate,
              "`extern` says this function is defined somewhere this compiler is not looking, "
              "and an array `" +
                  types_.spelling(returnType) +
                  "` returns here as a shape the caller sets up: pass a pointer instead, and "
                  "the address is what crosses");
      }
      for (std::size_t i = 0; i < params.size() && i < paramTypeNodes.size(); ++i) {
        const TypeId declared = params[i];
        if (types_.isAggregate(declared)) {
          error(paramTypeNodes[i], SemaErrorCode::ExternAggregate,
                "`extern` says this function is defined somewhere this compiler is not looking, "
                "and an array `" +
                    types_.spelling(declared) +
                    "` is passed here as a copy the caller makes: pass a pointer instead, "
                    "`*" +
                    types_.spelling(declared) + "`, and the address crosses");
        }
      }
    }

    TypeId functionType = types_.function(returnType, params, variadic);
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

    const std::optional<resolve::DefId> def = defAtName(nameNode);
    const std::size_t functionIndex = out_.typed.functionTable.size();
    out_.typed.functionTable.push_back(info);

    // The declarations of one function, checked against each other -- and this
    // is the first stage that *can*, because what has to agree is a type. A name
    // may be declared more than once: `extern fn i32 f();` above
    // `fn i32 f() { }` is the ordinary pair, and a header included twice is the
    // same pair. Every declaration of one name answers to one `DefId`, which is
    // the identity everything below this stage keys on, so the chain has to be
    // *one* function here -- otherwise one name comes out with two types, and
    // the lowering ends up with two symbols where the program has one.
    if (def.has_value()) {
      const auto earlier = firstDeclaration.find(def->index);
      if (earlier == firstDeclaration.end()) {
        firstDeclaration.emplace(def->index, functionIndex);
      } else {
        const FunctionInfo& first = out_.typed.functionTable[earlier->second];
        const ast::AstId firstNameNode = childOf(first.decl, ast::NodeKind::Name);
        const std::string name = std::string(symbols_.lookup(info.name));
        if (first.body.valid() && body.valid()) {
          error(nameNode.valid() ? nameNode : decl, SemaErrorCode::FunctionRedefinition,
                "`" + name +
                    "` is defined twice; a function has one definition, and the "
                    "second one does not replace the first");
          attachNote(SemaErrorCode::FunctionRedefinition, origin(firstNameNode),
                     "the first definition is here");
        } else if (first.functionType != functionType) {
          error(nameNode.valid() ? nameNode : decl, SemaErrorCode::SignatureMismatch,
                "`" + name + "` is declared with two signatures: `" +
                    types_.spelling(functionType) + "` here, `" +
                    types_.spelling(first.functionType) + "` earlier");
          attachNote(SemaErrorCode::SignatureMismatch, origin(firstNameNode),
                     "the earlier declaration is here");
        }
      }
    }

    setType(decl, functionType);
    if (nameNode.valid()) {
      setType(nameNode, functionType);
    }
    if (def.has_value()) {
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
  const bool returnsNever = types_.isNever(info.returnType);
  currentReturn_ = info.returnType;
  currentFunctionName_ = info.name;
  checkBody(info.body, info.returnType);

  // Definite assignment runs over the body the walk just finished, because the
  // one thing it needs from the walk -- the folded value of a condition, to know
  // whether a loop can fall out of the bottom -- is only complete now. It is a
  // separate pass and not a thread through the walk: see `check_flow.cc`.
  checkDefiniteAssignment(info.body);

  const ast::AstId nameNode = childOf(info.decl, ast::NodeKind::Name);
  const std::string name = info.name == support::kInvalidSym
                               ? std::string("this function")
                               : std::string(symbols_.lookup(info.name));

  // The promise a `!` return type makes, checked against the one body that has
  // to keep it. A declaration with no body has nothing to check -- its definition
  // is elsewhere, and taking the promise on the caller's word is exactly what an
  // `extern` prototype is for.
  //
  // This is the half of `!` that a type cannot check by itself. Everywhere else
  // the type does the work: a call to a `!` function *is* `!`, and that is how
  // control flow and conversions fall out of it. A body is the one place where a
  // type has to be *proved*, and the proof is the two ways a body can end --
  // reported separately because the fixes are different, and reported at the
  // statement rather than at the signature, because a `return` is a thing the
  // reader wrote.
  if (returnsNever) {
    const ast::AstId returning = reachableReturn(info.body);
    if (returning.valid()) {
      error(returning, SemaErrorCode::NeverReturns,
            "`" + name +
                "` returns `!`, so control never comes back to its caller, and a "
                "`return` is control coming back");
    } else if (!terminates(info.body)) {
      // The message names both shapes that keep the promise, because neither is
      // obvious from the type alone: a body cannot simply *stop* being divergent.
      error(nameNode.valid() ? nameNode : info.decl, SemaErrorCode::NeverBodyCompletes,
            "`" + name +
                "` returns `!`, so its body cannot reach its end -- it has to loop "
                "forever, or call another function that never returns");
    }
  }

  // Skipped for a `!`-returning function: it has no value to return, so "falls
  // off the end without returning a value" is not a second mistake on top of the
  // promise -- it is the same one, already reported with a better sentence.
  if (!returnsNever && !types_.isVoid(info.returnType) && !types_.isError(info.returnType) &&
      !terminates(info.body)) {
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
  case ast::NodeKind::ExprStmt: {
    // The second way a statement fails to fall through, and the one that makes
    // `!` worth having: an expression that never produces a value leaves control
    // nowhere to go. Everything after it is unreachable, which is what the
    // optimizer has to be told for such a call to be worth anything.
    const std::vector<ast::AstId> operands = operandsOf(stmt);
    return !operands.empty() && diverges(operands.front());
  }
  case ast::NodeKind::LetStmt:
  case ast::NodeKind::ConstStmt:
    // `let x: i32 = die();` never gets as far as the binding. The annotation is
    // what makes that legal -- and the program does not run, so there is nothing
    // after it either.
    return diverges(initializerOf(stmt));
  default:
    // An empty statement, a jump: control always reaches the next one.
    return false;
  }
}

bool Checker::diverges(ast::AstId expr) const {
  // An expression that never produces a value is one whose *type* is `!`.
  //
  // There is no second rule and no table to consult: `!` reaches an expression
  // exactly when the expression cannot finish, because that is what the type
  // means. This line is the whole reason the bottom type is better than an
  // annotation beside the signature -- `aborts()` had to know the callee, the
  // shape of the call and the meaning of the extra word, and this knows the
  // answer the checker already computed and wrote down.
  return expr.valid() && types_.isNever(out_.typed.typeOf(expr));
}

ast::AstId Checker::initializerOf(ast::AstId stmt) const {
  // A `let`/`const`'s operands are its name, its annotation and its value, in
  // source order, so "neither of the first two" is the value. One reader for the
  // two places that need it -- the declaration's own check and the flow rule
  // above -- so they cannot come to different answers about the same statement.
  const ast::AstId nameNode = childOf(stmt, ast::NodeKind::Name);
  const ast::AstId typeNode = childOf(stmt, ast::NodeKind::Type);
  ast::AstId init;
  for (const ast::AstId operand : operandsOf(stmt)) {
    if (operand != nameNode && operand != typeNode) {
      init = operand;
    }
  }
  return init;
}

ast::AstId Checker::reachableReturn(ast::AstId node) const {
  if (!node.valid() || inError(node)) {
    return ast::AstId{};
  }
  switch (kindOf(node)) {
  case ast::NodeKind::ReturnStmt: {
    const std::vector<ast::AstId> operands = operandsOf(node);
    // `return die();` is not a return: the operand never produces a value, so
    // control never gets as far as handing one back. The promise is kept by the
    // same rule that decided the operand's type, one level down.
    if (!operands.empty() && diverges(operands.front())) {
      return ast::AstId{};
    }
    return node;
  }
  case ast::NodeKind::Block:
    for (const ast::AstId child : file_.childrenOf(node)) {
      if (file_.at(child).isToken()) {
        continue;
      }
      if (const ast::AstId found = reachableReturn(child); found.valid()) {
        return found;
      }
      // Nothing written after a statement that never completes can run, so the
      // scan of this block is over -- including for a `return` below it, which
      // is exactly the shape the promise allows.
      if (terminates(child)) {
        return ast::AstId{};
      }
    }
    return ast::AstId{};
  case ast::NodeKind::IfStmt:
  case ast::NodeKind::ElseClause:
    // Both arms are reachable -- which one runs is a runtime question -- so a
    // `return` in either one is a `return` a caller can see.
    for (const ast::AstId child : file_.childrenOf(node)) {
      if (file_.at(child).isToken()) {
        continue;
      }
      if (const ast::AstId found = reachableReturn(child); found.valid()) {
        return found;
      }
    }
    return ast::AstId{};
  case ast::NodeKind::WhileStmt:
  case ast::NodeKind::ForStmt:
    // A `return` in the body is reachable even if the condition never goes
    // false: the loop takes its first iteration, and that is enough. The
    // statement *after* the loop is not asked about here -- a loop that cannot
    // leave has already stopped the enclosing block's scan.
    return reachableReturn(childOf(node, ast::NodeKind::Block));
  default:
    // A binding, an expression, an empty statement, a jump: none of them can
    // return, and none of them hides a statement of its own.
    return ast::AstId{};
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
    // The two value-less types are refused *before* the initializer is checked,
    // and only once: a value that "cannot be assigned to `void`" would be a
    // second sentence about the same mistake, and the mistake is the declaration,
    // not the value.
    bool voidDeclared = false;
    if (const std::string_view word = notAnObjectWord(types_, declared); !word.empty()) {
      error(typeNode.valid() ? typeNode : stmt, SemaErrorCode::TypeNotValue,
            "`" + std::string(word) + "` is not a type an object can have");
      declared = kInvalidType;
      voidDeclared = true;
    }

    const ast::AstId init = initializerOf(stmt);
    TypeId initType = kInvalidType;
    if (init.valid()) {
      // The initializer's conversion to the binding's type is recorded here:
      // `let x: i64 = a + b;` is an `i32` value stored into an `i64`, and that
      // pair is the `sext` the lowering materialises.
      initType = checkOperand(stmt, 0, init, declared);
    }

    TypeId binding = kTypeError;
    if (declared.valid()) {
      binding = declared;
      if (init.valid() && initType.valid()) {
        checkAssignable(initType, declared, init, SemaErrorCode::InvalidAssignment,
                        " in this initializer");
      }
    } else if (initType.valid() && !voidDeclared) {
      // A binding whose type is *inferred* can still turn out to be value-less:
      // `let x = f();` where `f` returns nothing. It is the same mistake as
      // `let x: void`, caught one step later, and it is reported once.
      //
      // `!` is the same refusal with a different fix, so it gets a different
      // sentence: an object of type `!` would be an object the program never
      // gets to, and the reader almost certainly meant the type the call *would*
      // have had -- which they can simply write (`never.md`, *Written where*).
      if (types_.isVoid(initType)) {
        error(init, SemaErrorCode::TypeNotValue,
              "this expression is `void`, so it is not a value an object can have");
      } else if (types_.isNever(initType)) {
        error(init, SemaErrorCode::TypeNotValue,
              "this expression never produces a value, so there is nothing to bind; "
              "write the type the value would have had, as in `let x: i32 = ...`");
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
    // A function whose return type is `!` has nothing to return and no way to
    // return it, so there is no type here to check a `return` against: the operand
    // is typed as a value on its own, and the *body* walk owns the sentence about
    // the statement itself (`never.md`, *The proof*). Without this the reader
    // would be told `i32` cannot be used as `!`, which is true and useless.
    if (types_.isNever(currentReturn_)) {
      if (expr.valid()) {
        (void)checkExpr(expr, kInvalidType);
      }
      return;
    }
    if (expr.valid()) {
      if (types_.isVoid(currentReturn_)) {
        const TypeId type = checkExpr(expr, kInvalidType);
        // `return die();` in a `void` function: the operand never produces a
        // value, so `void` is exactly what comes back -- nothing. Anything else
        // has a value, and a `void` function has nowhere to put it.
        if (!types_.isNever(type)) {
          error(stmt, SemaErrorCode::ReturnVoidValue,
                "this function returns `void`, so `return` takes no value");
        }
        return;
      }
      const TypeId type = checkOperand(stmt, 0, expr, currentReturn_);
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
