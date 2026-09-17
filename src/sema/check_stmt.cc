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

// The noun an aggregate is called by in a boundary sentence. One function, so the
// two `extern` messages cannot describe one kind two ways -- and so a kind added
// to `isAggregate` is described here rather than falling through to "an array",
// which is what a product used to be called (`tuples.md`, decision 12).
[[nodiscard]] std::string_view aggregateNoun(const TypeStore& types, TypeId type) {
  if (types.isSlice(type)) {
    return "a slice";
  }
  if (types.isTuple(type)) {
    return "a product";
  }
  return "an array";
}

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
  std::size_t i = 0;
  typePartsInto(file_.childrenOf(typeNode), i, /*depth=*/0, parts);
  return parts;
}

// One *run* of a type position, starting at `i` and stopping at a `,`, a `)` or
// the end of the children. A tuple's members are runs, which is the whole of why
// this is a function: `([3]i32, *(i32, bool))` is a group whose second member is
// a group, and each level reads with the same rules as the top one -- an element
// count is folded, a `*` is a part, a name is a word.
//
// `depth` is the parser's own nesting bound applied to the *parts*: a type run is
// one syntax node, so a deeply nested product does not deepen the tree the
// parser's `DepthGuard` measures, and the bound has to be enforced where the
// nesting is created (`tuples.md`, decision 15). A group that goes past it is
// marked and refused by the reader, which is a sentence and not a stack overflow.
void Checker::typePartsInto(std::span<const ast::AstId> children, std::size_t& i,
                            std::uint32_t depth, std::vector<TypePart>& parts) const {
  for (; i < children.size();) {
    const ast::AstId child = children[i];
    // A word first, and before the token test below: an `Identifier` *is* a token
    // (leaves and interior nodes share one tag space), so asking "is it a token"
    // first would throw every word away.
    if (file_.at(child).is(kIdentifierNode)) {
      TypePart word;
      word.word = file_.spellingOf(child);
      parts.push_back(word);
      ++i;
      continue;
    }
    // `A<i32, bool>`: the arguments of the word to the left (`generics.md`). The
    // list is a node of its own in the tree -- which is what keeps a use's `<...>`
    // apart from a product or a parenthesised group -- so this is where it is
    // attached to the word it belongs to, and the reader below sees one *part* per
    // use. Each argument is a type position like any other, so it is read by this
    // same function.
    if (kindOf(child) == ast::NodeKind::TypeArgList) {
      TypePart use;
      use.hasArgs = true;
      for (const ast::AstId argument : file_.childrenOf(child)) {
        if (kindOf(argument) != ast::NodeKind::Type) {
          continue;
        }
        std::vector<TypePart> one;
        std::size_t at = 0;
        typePartsInto(file_.childrenOf(argument), at, depth + 1, one);
        use.args.push_back(std::move(one));
      }
      // Attached to the word on its left, which the grammar guarantees is there.
      // A list that arrived alone is still read -- as a part with no word, which
      // the reader answers with a sentence rather than with a lookup of the empty
      // name.
      if (!parts.empty() && !parts.back().word.empty() && !parts.back().hasArgs &&
          !parts.back().isTuple) {
        parts.back().hasArgs = true;
        parts.back().args = std::move(use.args);
      } else {
        parts.push_back(std::move(use));
      }
      ++i;
      continue;
    }
    // The punctuators a type position can hold: `*`, `!` and one `[N]` group.
    // Anything else the builder left inside the type node is not part of a type,
    // and the grammar accepted nothing else here either -- so it is skipped
    // rather than guessed at.
    if (!file_.at(child).isToken()) {
      ++i;
      continue;
    }
    const Tag tag = tagOf(kindOf(child));
    // The end of this run, which only a member of a product can reach: a `,`
    // separates members and a `)` closes the group. At the top level neither can
    // appear, and stopping on one anyway is what makes this function total over
    // the tree it was handed rather than over the tree it expects.
    if (tag == kTokComma || tag == kTokRParen) {
      return;
    }
    if (tag == kTokStar || tag == kTokBang) {
      TypePart punctuation;
      // Exactly one of the two, which is what `readType` reads them by: a `*` is
      // a prefix over the words, and a `!` is a whole type on its own.
      punctuation.isStar = tag == kTokStar;
      punctuation.isBang = tag == kTokBang;
      parts.push_back(punctuation);
      ++i;
      continue;
    }
    if (tag == kTokLParen) {
      // `(T, U)`: a product, and a *base* of the run rather than a prefix over
      // it, which is exactly how `readType` reads the part this builds.
      TypePart tuple;
      tuple.isTuple = true;
      ++i; // `(`
      if (depth >= support::kMaxNestingDepth) {
        // The bound, marked on the group whose members were not read. The reader
        // turns this into one sentence; nothing recurses further, so a file that
        // nests a thousand products deep costs one diagnostic and not a stack.
        tuple.tooDeep = true;
        parts.push_back(std::move(tuple));
        return;
      }
      while (i < children.size() && tagOf(kindOf(children[i])) != kTokRParen) {
        std::vector<TypePart> member;
        typePartsInto(children, i, depth + 1, member);
        // A member the reader cannot make sense of is still a member: the group
        // is what the source wrote, and the sentences come from the reader over
        // this structure rather than from a second scan here.
        tuple.members.push_back(std::move(member));
        if (i < children.size() && tagOf(kindOf(children[i])) == kTokComma) {
          ++i;
          continue;
        }
        break;
      }
      // Closed by what is *found*, like the `[N]` group above: a missing `)` is
      // the parser's finding, and the group this loop read is the group the tree
      // holds.
      if (i < children.size() && tagOf(kindOf(children[i])) == kTokRParen) {
        ++i;
      }
      parts.push_back(std::move(tuple));
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
      // `cursor` is the token after the `[`, and it walks to just past the `]`:
      // one place advances the reader's position, so a group with a count and a
      // group without one cannot disagree about where they ended.
      std::size_t cursor = i + 1;
      if (cursor < children.size() && kindOf(children[cursor]) == kIdentifierNode &&
          file_.spellingOf(children[cursor]) == parse::kInferredCount) {
        // `[_]`: the count is the initializer's, so nothing is folded here and
        // `hasCount` stays false -- which is what makes `[_]` a *different part*
        // from `[]` and not the same one with a flag missing.
        array.countInferred = true;
        ++cursor;
      } else if (cursor < children.size() &&
                 tagOf(kindOf(children[cursor])) == kTokIntegerLiteral) {
        array.hasCount = true;
        const support::IntegerLiteral count = support::parseIntegerLiteral(
            file_.spellingOf(children[cursor]), support::IntegerBaseRule::DecimalLeadingZero);
        // `bits` rather than a cast of the value: the count is a `uint64_t` and
        // the reader's negative case is its own error, which the same reader
        // reports here as "does not fit".
        if (count.ok) {
          array.count = count.value.bits;
        } else {
          array.countOverflow = true;
        }
        ++cursor;
      }
      // A `]` that is not there is the parser's finding, which is why the group
      // is closed by what is found rather than by what is expected: this loop
      // reads the tree it was given, not the tree that should have been built.
      if (cursor < children.size() && tagOf(kindOf(children[cursor])) == kTokRBracket) {
        ++cursor;
      }
      i = cursor;
      parts.push_back(array);
      continue;
    }
    // Anything else the builder left inside the type node is not part of a type,
    // and the grammar accepted nothing else here either -- so it is skipped
    // rather than guessed at.
    ++i;
  }
}

std::string Checker::suggestTypeName(std::string_view word) const {
  std::string best;
  std::uint32_t bestDistance = support::kMaxSuggestionDistance + 1;
  for (const std::string_view candidate : typeNames()) {
    if (candidate == word) {
      continue;
    }
    // A name the *target* refuses is not a suggestion: answering a typo with a
    // spelling that then fails is the worst possible answer to one, and it is
    // exactly the failure the suggestion table's own test is named for. `f80` is
    // the one entry this can exclude (`typespec.h`), which is to say the rule
    // lives there and not here.
    if (!typeNameOnTarget(candidate, types_.target())) {
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
  const TypeSpecResult spec = readType(parts, types_, names());
  if (!spec.ok) {
    if (spec.unknownWord.empty()) {
      error(typeNode, SemaErrorCode::MalformedType, spec.message);
    } else {
      error(typeNode, SemaErrorCode::UnknownType, spec.message);
      const std::string suggestion = suggestTypeName(spec.unknownWord);
      // "did you mean `T`?" about `T` is not a suggestion. It happens when the
      // name *is* declared in this unit and this use is above it -- legal at file
      // scope, where every name is decided before anything is read, and not in a
      // block, which is read top to bottom (`type_alias.md`, decision 5). The
      // message stays the unknown-name one; a note that says "declared below"
      // would be the next improvement, and it needs the position of the
      // declaration this use is *before*, which is a reader this does not have.
      if (!suggestion.empty() && suggestion != spec.unknownWord) {
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
  if (spec.brokenName) {
    // The run is a name of this unit whose expansion already failed, and that was
    // reported at the declaration. Blaming this position would print the same
    // mistake twice, so the type is error and the range stays silent.
    setType(typeNode, kTypeError);
    return kTypeError;
  }
  if (!spec.type.valid()) {
    reportLimit(typeNode);
    setType(typeNode, kTypeError);
    return kTypeError;
  }
  setType(typeNode, spec.type);
  // Record the alias a position *is*, when it is one -- not when it is built out
  // of one (`*MyInt` is a pointer, and there is no name on the position to point
  // at). This is the fact the debug info needs to name a variable's type the way
  // the source did, and the fact an editor's hover needs; recording it where it is
  // decided is what keeps either of them from re-deriving it (`type_alias.md`).
  // A product is excluded for the same reason a `*` is: there is no single name
  // on the position to point at, and the members' own names were recorded where
  // *their* runs were read (`tuples.md`, `type_alias.md`).
  if (parts.size() == 1 && !parts.front().isStar && !parts.front().isArray &&
      !parts.front().isBang && !parts.front().isTuple) {
    // The *row that answered*, not a second lookup by spelling: the table is a
    // stack, so a block's name and a file's name of the same spelling are two
    // rows, and the one that decided this position's type is the one this points
    // at (`type_alias.md`, decision 5).
    if (const TypeName* const row = findTypeName(names(), parts.front().word);
        row != nullptr && row->alias != kNoAliasRow) {
      out_.typed.setAliasAt(typeNode, row->alias);
    }
  }
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

std::string Checker::typeAsWritten(TypeId type, ast::AstId typeNode) const {
  // The name the source used, when the source used one, and what it stands for:
  // `B2 (aka [8]u8)`. Measured from clang, and the reason to copy the shape is
  // that expanding a name is right for *identity* and wrong for *reading* -- a
  // reader repairs a message about a word they wrote, not about a type they may
  // never have spelled out (`type_alias.md`, decision 8).
  const std::uint32_t index = out_.typed.aliasAt(typeNode);
  if (index == TypedFile::kNoAlias || index >= out_.typed.aliases().size()) {
    return types_.spelling(type);
  }
  const TypeAliasInfo& alias = out_.typed.aliases()[index];
  if (!alias.nameNode.valid()) {
    return types_.spelling(type);
  }
  const std::string name(spelling(alias.nameNode));
  if (name.empty()) {
    return types_.spelling(type);
  }
  // Not `const`: the equal case returns it, and a const local would force a copy
  // where the move is the point (`performance-no-automatic-move`).
  std::string expansion = types_.spelling(type);
  if (name == expansion) {
    return expansion;
  }
  return name + " (aka `" + expansion + "`)";
}

void Checker::checkAssignable(TypeId from, TypeId to, ast::AstId at, SemaErrorCode code,
                              std::string_view what, ast::AstId expectedAt) {
  if (types_.isError(from) || types_.isError(to)) {
    return;
  }
  // A **binder**, and this is the whole of what is known about one: it is an
  // object, and a value is assignable to it exactly when it already *is* of that
  // type (`generics.md`, § 6). Anything else -- `let y: i32 = x;` where `x: T`, or
  // a literal in a binder's position -- would need the declaration to say which
  // types it takes, which is what a constraint is. The sentence says so, and it is
  // this one rather than the arithmetic rules' "`T` cannot be used as `i32`",
  // which is a sentence about a type the reader never wrote.
  //
  // `*T` is not this case: a pointer is an object with a width, so `*void` and
  // `*T` keep converting as they always did.
  if (from != to && (types_.isParam(from) || types_.isParam(to))) {
    const TypeId parameter = types_.isParam(to) ? to : from;
    error(at, SemaErrorCode::GenericOperation,
          "`" + types_.spelling(from) + "` cannot be used as `" + types_.spelling(to) + "`" +
              std::string(what) + ": `" + std::string(types_.get(parameter).paramSpelling) +
              "` is a type parameter, and what may be stored in one is what its constraint "
              "allows (`T: Num`). Constraints are the next stage");
    return;
  }
  const std::string target = typeAsWritten(to, expectedAt);
  // An integer and a float, before the general rule: this is not a narrowing to
  // be warned about, it is a conversion the language does not have, and the
  // sentence has to say what to write instead.
  if (mixedNumberPair(types_, from, to)) {
    error(at, code,
          "`" + types_.spelling(from) + "` does not convert to `" + target + "`" +
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
          "`" + types_.spelling(from) + "` does not convert to `" + target + "`" +
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
          "`" + types_.spelling(from) + "` does not convert to `" + target + "`" +
              std::string(what) + ": write the comparison you mean, as in `value != 0`");
    return;
  }
  // A pointer and a non-pointer, in either direction. The message is its own
  // because this is the model's central refusal rather than a type mismatch:
  // `memory.md` states that a pointer is not an integer, that neither conversion
  // is implicit, and that the two operations which do join them are named and
  // counted. Until they are in the grammar, the refusal is the whole rule.
  if (types_.isPointer(from) != types_.isPointer(to)) {
    error(at, SemaErrorCode::PointerInteger,
          "`" + types_.spelling(from) + "` does not convert to `" + target + "`" +
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
          "`" + types_.spelling(from) + "` cannot be used as `" + target + "`" + std::string(what) +
              ": pointers convert implicitly only to the same pointee type, or through "
              "`*void`");
    return;
  }
  error(at, code,
        "`" + types_.spelling(from) + "` cannot be used as `" + target + "`" + std::string(what));
}

// --- the unit ----------------------------------------------------------------

SemaOutput Checker::run() {
  out_.typed.typeTable.assign(file_.nodeCount(), kTypeError);
  out_.typed.exprFacts.assign(file_.nodeCount(), ExprInfo{});

  // The unit's type names first, before anything that reads a type: a signature
  // may be written with a name declared below it, and a pass that ran after the
  // signatures would have to make them all wait for it (`type_alias.md`,
  // decision 5).
  runAliases();
  runSignatures();
  // The file scope next, and before any body, for the reason the signature pass
  // comes first: it is decided once, in dependency order, and everything that
  // reads it reads the same answer. A body checked before it would fold nothing
  // and could not disagree -- it would simply miss what it was allowed to do.
  checkGlobals();
  // The bodies. A generic body is checked **once**, abstractly, under its
  // binders, and it is lowered once per instance (`generics.md`, decision 7): the
  // context below is what makes a call inside it record a *site* instead of
  // creating the instance it cannot know yet.
  for (std::size_t index = 0; index < out_.typed.functionTable.size(); ++index) {
    currentBody_ = static_cast<std::uint32_t>(index);
    currentGenericOwner_ = out_.typed.functionTable[index].owner;
    currentGenericBinders_ = out_.typed.functionTable[index].binders;
    if (currentGenericBinders_ != 0) {
      std::vector<TypeName> rows;
      pushBinderRows(out_.typed.functionTable[index].genericParams, currentGenericOwner_, rows);
      enterBinders(rows);
    }
    checkFunction(out_.typed.functionTable[index]);
    leaveBinders();
  }
  currentBody_ = 0;
  currentGenericOwner_ = 0;
  currentGenericBinders_ = 0;
  // The worklist, after every body: the instances a generic body *asks* for are
  // only known once the body has been read, and each of them may ask for more.
  runInstantiations();
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

    // The **binders**, if the declaration wrote any, and they are in scope for
    // everything below: the return type, every parameter's type, and -- for the
    // length of this function's body -- the body itself (`generics.md`). The two
    // refusals first, because neither declaration can be a template in any
    // useful sense and both are decisions about *this* signature.
    //
    // A binder list is read once per declaration and not once per instance: the
    // body is checked here, abstractly, under the binders, and what the worklist
    // does afterwards is build the instances that body is lowered for. That is
    // why the signature published here is a **template** -- it holds `Param`s and
    // nothing below this stage ever sees it (decision 20).
    const ast::AstId genericParams = childOf(decl, ast::NodeKind::GenericParams);
    std::uint32_t binders = 0;
    const std::uint32_t owner = genericParams.valid() ? genericParams.index : 0;
    if (genericParams.valid()) {
      std::vector<TypeName> rows;
      binders = pushBinderRows(genericParams, owner, rows);
      enterBinders(rows);
      if (!body.valid()) {
        // The boundary, and it is a boundary rather than a gap: `extern` says the
        // definition is somewhere this compiler is not looking, and a template is
        // not one signature -- it is a family of them, each with its own symbol
        // (`generics.md`, § 9).
        error(genericParams, SemaErrorCode::GenericExtern,
              "a binder list and `extern` cannot both be true: `extern` promises one C symbol " +
                  std::string("for one signature, and a generic declaration has one signature per "
                              "instantiation -- give the function a body, or write the types"));
      }
    }

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
        // Two aggregates, two sentences, because the two are refused for
        // different reasons and the repair is different. An array crosses as a
        // *shape this compiler sets up* (a pointer to a copy, an `sret`
        // destination), which no foreign callee knows; a slice crosses as a
        // `{ptr, len}` descriptor whose layout this compiler chose and has not
        // promised to anybody. Both are a boundary and not a gap, and the second
        // sentence says what to write instead (`slices.md` decision 13).
        const std::string answer = types_.isSlice(returnType)
                                       ? "a pointer and a length, which are the two words the "
                                         "caller can pass and read"
                                       : "pass a pointer instead, and the address is what "
                                         "crosses";
        error(typeNode.valid() ? typeNode : decl, SemaErrorCode::ExternAggregate,
              "`extern` says this function is defined somewhere this compiler is not looking, "
              "and " +
                  std::string(aggregateNoun(types_, returnType)) + " `" +
                  types_.spelling(returnType) +
                  "` returns here as a shape this compiler "
                  "chose: " +
                  answer);
      }
      for (std::size_t i = 0; i < params.size() && i < paramTypeNodes.size(); ++i) {
        const TypeId declared = params[i];
        if (!types_.isAggregate(declared)) {
          continue;
        }
        // One string, built in place: the sentence is three parts -- what the
        // parameter is, which aggregate it is, and what to write instead -- and
        // the middle one is the type.
        std::string message =
            "`extern` says this function is defined somewhere this compiler is not looking, "
            "and ";
        message += aggregateNoun(types_, declared);
        message += " `";
        message += types_.spelling(declared);
        message += types_.isSlice(declared)
                       ? "` is passed here as a descriptor whose layout this compiler chose: "
                         "pass a pointer and a length, which are the two words the callee can "
                         "read"
                       : "` is passed here as a copy the caller makes: pass a pointer instead, "
                         "`*" +
                             types_.spelling(declared) + "`, and the address crosses";
        error(paramTypeNodes[i], SemaErrorCode::ExternAggregate, std::move(message));
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
    info.genericParams = genericParams;
    info.binders = binders;
    info.owner = owner;
    leaveBinders();

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

    // The declaration is **generic**, so this stage has to answer for it twice:
    // the template it just published (never read below), and every instance a use
    // asks for. The map is keyed on the def because a call reaches a declaration
    // the way every other lookup in this stage does.
    if (info.binders != 0 && def.has_value()) {
      genericFunctionByDef_.emplace(def->index, static_cast<std::uint32_t>(functionIndex));
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
    // ... and the entry point is one function with one signature, so it is not a
    // family of them (`generics.md`, § 9).
    if (genericParams.valid() && info.name != support::kInvalidSym &&
        symbols_.lookup(info.name) == "main") {
      error(genericParams, SemaErrorCode::GenericMain,
            "`main` is the entry point: it is one function with one signature, so it cannot be "
            "generic");
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
  // The file's own reader, and not a second walk here: the same question is asked
  // by the validator, the flow pass, this stage and the lowering, and a shape read
  // four times is a shape that disagrees with itself once a form is added.
  return file_.initializerOf(stmt);
}

// `let (a, b) = t;`: **real bindings, each a copy** of its member (`tuples.md`,
// decision 6). The value is taken apart after it is evaluated -- one expression,
// one evaluation, one member per name -- and the two numbers a reader has to get
// right (how many names, how many members) are compared here, where both are
// known, rather than discovered by the lowering.
void Checker::checkDestructuring(ast::AstId stmt, ast::AstId pattern, bool isConst) {
  const std::vector<ast::AstId> names = file_.bindingNamesOf(stmt);
  const std::size_t arity = names.size();
  const ast::AstId typeNode = childOf(stmt, ast::NodeKind::Type);
  const ast::AstId init = file_.initializerOf(stmt);

  // The members the names take their types from: the annotation's when one was
  // written, the value's otherwise. `kTypeError` is what a name keeps when its
  // type could not be established, which is the same poison a failed binding gets.
  std::vector<TypeId> bound(arity, kTypeError);
  TypeId declared = kInvalidType;
  // Read once, where the value is checked: an annotation that was already refused
  // must not be used as the thing the value is checked *against*.
  bool blocked = false;

  if (typeNode.valid()) {
    declared = resolveTypeNode(typeNode);
    if (types_.isError(declared)) {
      blocked = true; // the sentence was the type reader's
    } else if (!types_.isTuple(declared)) {
      // A container the language can *iterate* is not the same thing as a product
      // it can take apart, and this is the boundary: `[]i32` and `[3]i32` have one
      // element type and a runtime length, which is a loop's business.
      error(typeNode, SemaErrorCode::DestructuringNotProduct,
            "this binds " + std::to_string(arity) +
                " names, so the annotation has to be a "
                "product of " +
                std::to_string(arity) + ": `" + types_.spelling(declared) +
                "` cannot be taken apart");
      blocked = true;
    } else {
      const std::span<const TypeId> members = types_.membersOf(declared);
      if (members.size() != arity) {
        error(pattern, SemaErrorCode::DestructuringArity,
              "this pattern has " + std::to_string(arity) + " names, and `" +
                  types_.spelling(declared) + "` has " + std::to_string(members.size()) +
                  " members");
        blocked = true;
      } else {
        for (std::size_t i = 0; i < arity; ++i) {
          bound[i] = members[i];
        }
      }
    }
  }

  TypeId initType = kInvalidType;
  if (!blocked && init.valid()) {
    if (declared.valid()) {
      // The annotation is what the value is *against*, so each member is typed
      // against its own position -- `let (x, y): (f64, f64) = (1, 2);` is two
      // `f64`s and not two `i32`s converted afterwards (`tuples.md`, decision 8).
      initType = checkOperand(stmt, 0, init, declared);
      if (!types_.isError(initType)) {
        checkAssignable(initType, declared, init, SemaErrorCode::InvalidAssignment,
                        " in this destructuring", typeNode);
      }
    } else {
      // `checkOperand` and not `checkExpr`: a deferred literal with nothing to
      // decide it becomes its class's default here, exactly as `let x = 1;` does --
      // `(1, 2)` is a `(i32, i32)` and its members are decided *inside* the literal,
      // while a bare `5` on the right of a pattern is decided by this line. Without
      // it the deferred type would leak to the arity check below and the sentence
      // would name `<integer literal>`.
      //
      // None of the refusals below sets `blocked`, because nothing reads it again:
      // `bound` starts as the poison and is written only where a member's type was
      // established, so a refusal *is* the absence of a write. A flag that is set
      // and never read looks like a rule that decides something.
      initType = checkOperand(stmt, 0, init, kInvalidType);
      if (types_.isVoid(initType)) {
        error(init, SemaErrorCode::TypeNotValue,
              "this expression is `void`, so it is not a value an object can have");
      } else if (types_.isNever(initType)) {
        error(init, SemaErrorCode::TypeNotValue,
              "this expression never produces a value, so there is nothing to bind; "
              "write the type the value would have had, as in `let (a, b): (i32, bool) = ...`");
      } else if (!types_.isError(initType) && !types_.isTuple(initType)) {
        // The two ways to write this that the reader probably meant, and neither is
        // guessing: bind the value whole, or take a product apart.
        error(init, SemaErrorCode::DestructuringNotProduct,
              "`" + types_.spelling(initType) +
                  "` has no members to bind: a pattern takes a "
                  "**product** apart. Write `let x: " +
                  types_.spelling(initType) + " = ...` to bind it whole");
      } else if (types_.isTuple(initType)) {
        const std::span<const TypeId> members = types_.membersOf(initType);
        if (members.size() != arity) {
          error(pattern, SemaErrorCode::DestructuringArity,
                "this pattern has " + std::to_string(arity) + " names, and `" +
                    types_.spelling(initType) + "` has " + std::to_string(members.size()) +
                    " members");
        } else {
          for (std::size_t i = 0; i < arity; ++i) {
            bound[i] = members[i];
          }
        }
      }
    }
  }

  // Each name gets its member's type, published where every other answer about a
  // declaration lives (`defTypes_`), so no later stage has to walk the pattern to
  // ask what `a` is.
  //
  // `_` is the position nobody wanted: no definition was made for it (`resolve`),
  // so there is nothing to publish -- and the member's value is simply not taken.
  std::vector<ast::AstId> initMembers;
  if (isConst && init.valid() && kindOf(init) == ast::NodeKind::TupleExpr) {
    // A `const` pattern whose value is a literal, all of whose members are
    // literals, publishes the same compile-time values a single `const` does -- so
    // `const (W, H) = (16, 9);` can size a `[W]i32`. Only the *literal* case: a
    // member of a value that was computed has no value this stage can name, which
    // is exactly what a constant has to have.
    for (const ast::AstId member : operandsOf(init)) {
      initMembers.push_back(member);
    }
  }

  for (std::size_t i = 0; i < arity; ++i) {
    const ast::AstId name = names[i];
    setType(name, bound[i]);
    const std::optional<resolve::DefId> def = defAtName(name);
    if (!def.has_value() || def->index >= defTypes_.size()) {
      continue;
    }
    defTypes_[def->index] = bound[i];
    defIsConst_[def->index] = isConst;
    if (isConst && i < initMembers.size()) {
      const ExprInfo& facts = out_.typed.infoOf(initMembers[i]);
      if (facts.isConstant && facts.hasIntValue) {
        defConstValues_[def->index] = facts.value;
        defHasConstValue_[def->index] = true;
      }
    }
  }

  // The statement's own type is the value it took apart, which is the only honest
  // answer for a node that binds several: every *binding* has its own type, and
  // this is what a consumer sees when it asks about the statement.
  setType(stmt, initType.valid() ? initType : kTypeError);
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
  // A binder is refused before the rule about `bool`, because "`T` is not a
  // `bool`" is a sentence about a type nobody wrote: what a condition needs is the
  // declaration to say that every type it takes is a `bool`, which is what a
  // constraint is (`generics.md`, § 6).
  if (refuseParameter(condition, type,
                      "the condition of `" + std::string(what) +
                          "` needs `bool`, and a binder is not one")) {
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
  // A block is a scope for *names of types* as well as for values: a `type` among
  // the statements is visible from where it is written to the end of the block, and
  // gone when the block ends. The published name table is a stack, so entering is
  // one size and leaving is one resize -- and a name the file already has is
  // hidden for exactly as long as this block is being checked, because the row in
  // scope is the last one (`type_alias.md`, decision 5).
  const std::size_t namesBefore = aliasNames_.size();
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
  aliasNames_.resize(namesBefore);
  refreshNames();
}

void Checker::checkStatement(ast::AstId stmt, TypeId returnType) {
  if (!stmt.valid() || inError(stmt)) {
    return;
  }
  switch (kindOf(stmt)) {
  case ast::NodeKind::TypeAliasDecl:
    // The in-order half of the alias rule, and the only statement that declares a
    // *name for a type* (`type_alias.md`, decision 5).
    checkBlockAlias(stmt);
    return;
  case ast::NodeKind::LetStmt:
  case ast::NodeKind::ConstStmt: {
    const bool isConst = kindOf(stmt) == ast::NodeKind::ConstStmt;
    // The pattern, when the left-hand side was a `(...)`: the same statement, the
    // same annotation and the same initializer, with several bindings instead of
    // one. Split out because *nothing* below is shared -- a single binding has one
    // type to be, and a pattern has one per name (`tuples.md`, decision 6).
    if (const ast::AstId pattern = childOf(stmt, ast::NodeKind::TuplePattern); pattern.valid()) {
      checkDestructuring(stmt, pattern, isConst);
      return;
    }
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
        // The annotation's own node, so the message can name the type the way the
        // source did (`typeAsWritten`).
        checkAssignable(initType, declared, init, SemaErrorCode::InvalidAssignment,
                        " in this initializer", typeNode);
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
