// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// `type Name = T;`: the pass that decides what each name stands for.
//
// An alias is a *name*, and this file is the whole of what that costs. There is no
// type store entry, no `TypeKind`, and no new identity anywhere: the pass answers
// one question per declaration -- which `TypeId` does this name stand for -- and
// then every consumer of a type accepts either spelling for free
// (`type_alias.md`, decision 2).
//
// The shape of the pass is `checkGlobals`'s, deliberately and in full:
//
//   * **File scope is order-independent.** `type A = *B;` before `type B = i32;`
//     is legal, because a name for a type is not storage and there is no
//     initializer that has to be evaluated before the name can exist. The
//     declarations are collected first and then decided in *dependency* order.
//   * **The walk is an explicit stack with three marks.** A chain of names is as
//     long as the unit has declarations and a unit is somebody else's file, so
//     this is not recursion; and a dependency reached while it is still being
//     decided *is* the cycle, which is what the marks are for.
//   * **The messages come out in source order.** The walk visits in dependency
//     order, which is not the order a file is read in, so the range this pass
//     reported is sorted at the end exactly as the globals pass sorts its own.
//
// One thing is its own and not the globals pass's: an alias's dependency is a
// **word inside a type**, not a name inside an expression. So the pass reads the
// `Type` node's parts, asks which of those words are names of this unit, decides
// those first, and only then hands the same parts to the type reader along with
// the table it has built.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "checker.h"
#include "sema/sema_error.h"
#include "sema/typespec.h"
#include "support/typenames/type_name.h"

namespace minc::sema {

namespace {

// Three colours, as in the globals pass: white is "not looked at yet", grey is
// "being decided" and black is "decided". A grey dependency is the cycle.
enum class Mark : std::uint8_t { White, Grey, Black };

// Source order, for the messages this pass produces. Same reasoning and the same
// comparison as `global.cc`; kept local rather than shared, because one helper
// both passes call is one more thing the two have to agree about.
[[nodiscard]] bool sourceOrdered(const SemaError& a, const SemaError& b) {
  if (a.span.file != b.span.file) {
    return a.span.file < b.span.file;
  }
  if (a.span.begin != b.span.begin) {
    return a.span.begin < b.span.begin;
  }
  return a.span.end < b.span.end;
}

} // namespace

void Checker::collectAliases() {
  // The file scope's type names are the unit root's `type` children, in source
  // order. Source order and not walk order, because it is what makes the
  // published table deterministic -- and the index into this vector is what the
  // published table and `aliasAt` are keyed on.
  for (const ast::AstId decl : operandsOf(file_.root())) {
    if (kindOf(decl) != ast::NodeKind::TypeAliasDecl || inError(decl)) {
      continue;
    }
    AliasBinding binding;
    binding.decl = decl;
    binding.nameNode = childOf(decl, ast::NodeKind::Name);
    binding.target = childOf(decl, ast::NodeKind::Type);
    if (const std::optional<resolve::DefId> def = defAtName(binding.nameNode)) {
      binding.def = *def;
    }
    // A name the language keeps is *not* a dependency and not a name: the
    // declaration is refused by `resolve` (below), and until it is, `i32` must go
    // on meaning `i32` for every other type in the unit.
    if (binding.nameNode.valid() && !spelling(binding.nameNode).empty() &&
        !support::isTypeNameWord(spelling(binding.nameNode))) {
      // The first entry wins for a repeated spelling, which is the canonical
      // declaration: `resolve` gives one name one def and reports the repeat, so
      // two entries for one spelling cannot come out of a unit `resolve` accepted.
      aliasIndexBySpelling_.emplace(spelling(binding.nameNode), aliases_.size());
    }
    aliases_.push_back(binding);
  }
}

bool Checker::decideAlias(AliasBinding& binding) {
  const std::string_view word =
      binding.nameNode.valid() ? spelling(binding.nameNode) : std::string_view{};
  // **The refusal is `resolve`'s**, and this asks the same table it does
  // (`support::isTypeNameWord`): the reserved class reports there, with the
  // sentence that says *why* a type word is reserved -- it is the half of `(T)x`
  // that is not about casts. So this pass only makes sure the name does not become
  // usable, and stays quiet about it: one fault, one diagnostic.
  if (!word.empty() && support::isTypeNameWord(word)) {
    binding.type = kInvalidType;
    return false;
  }
  if (!binding.target.valid()) {
    // The parser already reported the missing type; a `type` without one has no
    // expansion and no second diagnostic to give.
    binding.type = kInvalidType;
    return true;
  }
  // `type Pair<T, K> = (T, K);`: a **generic** name (`generics.md`).
  //
  // A binder is a name that stands for a type, which is exactly what a row of
  // this table already is -- so the target is read with one extra row per binder
  // pushed on the stack the shadowing rule already has, and nothing else about
  // this pass changes. What comes out is the *template*: the target with `Param`s
  // in it. A use with arguments substitutes into it, and the substituted type is
  // a type the store already has, so `Pair<i32, bool>` *is* `(i32, bool)` and the
  // check is an id equality (decision 8).
  //
  // The owner of those parameters is the binder list's own node, which is unique
  // in the unit and stable for as long as it is being checked -- and which is what
  // keeps two declarations that both call their binder `T` from sharing a type.
  std::vector<TypeName> scope;
  if (const ast::AstId params = childOf(binding.decl, ast::NodeKind::GenericParams);
      params.valid()) {
    binding.owner = params.index;
    scope.assign(names().begin(), names().end());
    // The rows come from the one builder the function half uses, so a binder is
    // the same thing wherever it is declared: `pairBinderRows` decides the
    // spelling, the position and the two names `resolve` has already refused.
    binding.binders = pushBinderRows(params, binding.owner, scope);
  }
  const std::span<const TypeName> names =
      binding.binders == 0 ? std::span<const TypeName>() : std::span<const TypeName>(scope);
  const std::span<const TypeName> table = binding.binders == 0 ? this->names() : names;
  const TypeSpecResult spec = readType(typeParts(binding.target), types_, table, std::nullopt);
  binding.type = spec.ok ? spec.type : kInvalidType;
  if (!spec.ok) {
    // The reader's own sentence, reported at the type position it is about: an
    // array of no elements, a `_` with no initializer, a pointer with nothing to
    // point at. `ok` with an invalid type is the other answer -- "understood, the
    // store refused it" (the budget has already reported that one), or "the name
    // is one whose expansion failed", which was reported where it failed.
    error(binding.target,
          spec.unknownWord.empty() ? SemaErrorCode::MalformedType : SemaErrorCode::UnknownType,
          spec.message);
  }
  return true;
}

void Checker::checkBlockAlias(ast::AstId decl) {
  AliasBinding binding;
  binding.decl = decl;
  binding.nameNode = childOf(decl, ast::NodeKind::Name);
  binding.target = childOf(decl, ast::NodeKind::Type);
  if (const std::optional<resolve::DefId> def = defAtName(binding.nameNode)) {
    binding.def = *def;
  }

  const std::string_view word =
      binding.nameNode.valid() ? spelling(binding.nameNode) : std::string_view{};
  // A block is read top to bottom, so the name is not in scope until its
  // declaration -- and that answers the one question a target mentioning its own
  // name can ask. With an outer name of the same spelling in scope it is a
  // *shadow* (`type T = *T;` inside a block that already has a `T` means the
  // outer one, exactly as `typedef T T;` does in C), and with none it is a
  // circle, because there is no other name it could mean. The file scope never
  // has this question: there, every name is decided before anything is read.
  // The words of the target, with a product's members opened and their words in
  // order (`typeRunWords`): a name written inside `(T, U)` is a name this
  // declaration mentions, and missing it would let `type T = (T, i32);` inside a
  // block read the outer `T` as if the circle were not there.
  bool self = false;
  if (!word.empty() && binding.target.valid() && findTypeName(names(), word) == nullptr) {
    for (const std::string_view mentioned : typeRunWords(typeParts(binding.target))) {
      if (mentioned == word) {
        self = true;
        break;
      }
    }
  }

  bool publishable = false;
  if (self) {
    binding.type = kInvalidType;
    error(binding.target, SemaErrorCode::TypeAliasCycle,
          "the type `" + std::string(word) +
              "` is defined in terms of itself: a name for a type is an abbreviation, and an "
              "abbreviation that contains itself has no expansion: recursion needs a type "
              "that names *itself*, and this name is not in scope yet");
  } else {
    publishable = decideAlias(binding);
  }

  // Published as a declaration either way -- the table is what a dump, a hover,
  // and the debug info read (`type_alias.md`) -- and as a *name in scope* only
  // when the name is one this unit owns.
  const std::size_t index = aliases_.size();
  aliases_.push_back(binding);
  publishAlias(binding);
  if (publishable && !word.empty()) {
    addTypeName(TypeName{word, binding.type, static_cast<std::uint32_t>(index), binding.binders,
                         binding.owner});
  }
}

void Checker::publishAlias(const AliasBinding& binding) {
  TypeAliasInfo info;
  info.decl = binding.decl;
  info.nameNode = binding.nameNode;
  info.target = binding.target;
  info.type = binding.type;
  out_.typed.addAlias(info);
}

void Checker::runAliases() {
  aliases_.clear();
  aliasIndexBySpelling_.clear();
  aliasNames_.clear();
  refreshNames();
  collectAliases();
  if (aliases_.empty()) {
    return;
  }

  // Everything this pass reports is put back in source order at the end.
  const std::size_t firstError = out_.errors.size();

  // One frame per alias being decided: which one, how far into its words the walk
  // has read, and the words themselves -- read once, when the frame starts.
  //
  // The words are **flattened**, with a product's members spliced in where the
  // group sits (`typeRunWords`), because a dependency is a name and a name is in
  // the members as much as beside a `*`: `type A = (B, i32);` above
  // `type B = i32;` is legal, and a walk over the top level only would decide `A`
  // first and then fail to read it.
  struct Frame {
    std::size_t index;
    std::size_t nextWord;
    std::vector<std::string_view> words;
  };

  std::vector<Mark> mark(aliases_.size(), Mark::White);
  std::vector<Frame> stack;

  const auto nameOf = [&](std::size_t index) -> std::string {
    const ast::AstId nameNode = aliases_[index].nameNode;
    const std::string_view written = nameNode.valid() ? spelling(nameNode) : std::string_view{};
    return written.empty() ? std::string("?") : std::string(written);
  };

  // The cycle as a sentence. The *path* is the message: "defined in terms of
  // itself" without the chain leaves a reader hunting a file for which of the
  // names went in a circle. The frames on the stack are exactly the grey names,
  // so the chain is read out of them -- from the alias that was reached again to
  // the one that reached it.
  const auto cyclePath = [&](std::size_t closing) -> std::string {
    std::string text;
    bool started = false;
    for (const Frame& frame : stack) {
      if (!started && frame.index != closing) {
        continue;
      }
      started = true;
      if (!text.empty()) {
        text += "` -> `";
      }
      text += nameOf(frame.index);
    }
    if (!text.empty()) {
      text += "` -> `";
    }
    return text + nameOf(closing);
  };

  // A name becomes usable the moment it is decided, and for the rest of the unit:
  // the reader looks the unit's own names up before anything else, so this is what
  // makes `type A = *B;` work however the two were ordered -- and what makes a use
  // of a *failed* name answer "understood, no type" instead of "unknown word".
  const auto publish = [&](std::size_t index) {
    const ast::AstId nameNode = aliases_[index].nameNode;
    if (!nameNode.valid()) {
      return;
    }
    const std::string_view written = spelling(nameNode);
    if (!written.empty()) {
      // The row carries the declaration it came from, and the index is this
      // pass's own: the table published below is this vector in this order. A
      // generic name also carries its binder count and the id of the declaration
      // those binders belong to, which is what a *use* substitutes with.
      addTypeName(TypeName{written, aliases_[index].type, static_cast<std::uint32_t>(index),
                           aliases_[index].binders, aliases_[index].owner});
    }
  };

  for (std::size_t start = 0; start < aliases_.size(); ++start) {
    if (mark[start] != Mark::White) {
      continue;
    }
    mark[start] = Mark::Grey;
    stack.push_back(Frame{start, 0, typeRunWords(typeParts(aliases_[start].target))});

    while (!stack.empty()) {
      Frame& frame = stack.back();
      AliasBinding& binding = aliases_[frame.index];

      // Walk the words once, looking for the first name that has to be decided
      // before this one can be -- and for the one that closes a circle.
      std::size_t dependency = aliases_.size();
      std::size_t cycle = aliases_.size();
      while (frame.nextWord < frame.words.size()) {
        const std::string_view word = frame.words[frame.nextWord];
        ++frame.nextWord;
        const auto found = aliasIndexBySpelling_.find(word);
        if (found == aliasIndexBySpelling_.end()) {
          continue; // not a name of this unit
        }
        const std::size_t named = found->second;
        if (mark[named] == Mark::Grey) {
          // `type A = A;` lands here too, and needs no case of its own: the frame
          // that is being decided is grey while it is being decided.
          cycle = named;
          break;
        }
        if (mark[named] == Mark::White) {
          dependency = named;
          break;
        }
        // Black: already decided, so the word is simply read below.
      }

      if (cycle < aliases_.size()) {
        error(binding.target, SemaErrorCode::TypeAliasCycle,
              "the type `" + nameOf(cycle) + "` is defined in terms of itself: `" +
                  cyclePath(cycle) +
                  "`. A name for a type is an abbreviation, and an abbreviation that contains "
                  "itself has no expansion: recursion needs a type that names *itself*, and a "
                  "name that stands for another type never does");
        binding.type = kInvalidType;
        // Published, so the *uses* of a name in the circle stay silent: an alias
        // whose target mentions `B` reads it as "understood, no type" rather than
        // as an unknown word, and one circle prints one sentence.
        publish(frame.index);
        mark[frame.index] = Mark::Black;
        stack.pop_back();
        continue;
      }

      if (dependency < aliases_.size()) {
        mark[dependency] = Mark::Grey;
        stack.push_back(Frame{dependency, 0, typeRunWords(typeParts(aliases_[dependency].target))});
        continue;
      }

      // Every name this one mentions is decided, so its target can be read for
      // real -- with the unit's table, which is what makes `type A = *B;` work
      // however the two were ordered. Published for a name this unit owns, and the
      // invalid case is deliberate: the reader answers "understood, no type" for a
      // name in `aliasNames_` whose expansion failed, so a name that depended on a
      // broken one is silent about it. A *reserved* name is the exception --
      // publishing it would take a word of the language away from the unit.
      if (decideAlias(binding)) {
        publish(frame.index);
      }
      mark[frame.index] = Mark::Black;
      stack.pop_back();
    }
  }

  // The published table, one entry per `type` declaration, in source order: the
  // order the file was read, which is what a dump and an editor want, and not the
  // order the walk happened to decide them. Block-scope declarations are appended
  // by `checkBlockAlias` as their blocks are checked, so this loop covers the file
  // scope and the indices it hands out are the file scope's.
  for (const AliasBinding& binding : aliases_) {
    publishAlias(binding);
  }

  std::stable_sort(out_.errors.begin() + static_cast<std::ptrdiff_t>(firstError), out_.errors.end(),
                   sourceOrdered);
}

} // namespace minc::sema
