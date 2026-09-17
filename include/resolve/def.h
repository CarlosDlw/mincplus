// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// A declaration, and the identity by which everything else refers to it.
//
// The four namespaces are C's (6.2.3), and they are here from the first day even
// though only `Ordinary` is reachable: adding one later would reshape every
// scope, every lookup, every diagnostic and every cache key, while paying for it
// now is four enumerators and a comment. One of them is not like the others -- a
// `Member` name is never found by ordinary lookup (a field is looked up in the
// type of its base, not along the scope chain) -- so `Member` exists to be
// *filled* by `struct`, and the lookup that reads it arrives with `struct`.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "builtins/builtin_id.h"
#include "resolve/predefined.h"
#include "support/intern/sym_id.h"
#include "support/span/file_id.h"
#include "support/span/span.h"

namespace minc::resolve {

enum class Namespace : std::uint8_t { Ordinary, Tag, Label, Member };

inline constexpr std::size_t kNamespaceCount = 4;

[[nodiscard]] std::string_view toString(Namespace value);

// What a declaration declares. `Parameter` is reserved: parameters are not
// parsed yet, but the body walk already has the slot where they enter the
// function scope, so adding them is a loop and not a redesign.
enum class DefKind : std::uint8_t {
  Function,
  Variable,  // `let`
  Constant,  // `const`
  Parameter, // a function parameter
  // `type Name = T;` -- a *name* for a type that already exists, in the `Tag`
  // namespace. It is a definition here and not a type-store entry: the IDE wants
  // to jump to it, `-Wshadow` wants to warn about it, and the checker wants to
  // expand it, while the type it names has one identity with or without the name
  // (`type_alias.md`, decisions 2 and 11).
  TypeAlias,
  // A **binder**: the `T` of `fn T identity<T>(value: T)` or of `type Pair<T, K> =
  // (T, K);` (`generics.md`). A name for an abstract type, in the `Tag`
  // namespace, scoped to the declaration that wrote it -- so it is declared,
  // shadowed, and (not) reused by the same rules every other name follows, and
  // the IDE can answer for it.
  //
  // It is a definition here and *not* a type-store entry: the checker builds the
  // `Param` rows its type reader takes from the declaration itself, because a
  // type is read once per declaration and a def is what makes `T` a *name*.
  GenericParam,
};

[[nodiscard]] std::string_view toString(DefKind value);

// C 6.2.2. `None` is a block-scope name; a file-scope function is `External`
// (the default for a non-`static` function in C).
enum class Linkage : std::uint8_t { None, Internal, External };

[[nodiscard]] std::string_view toString(Linkage value);

// The kinds of scope C defines (6.2.1), plus the two this language will need.
// File, Function and Block are what the parser can produce today; the rest cost
// one enumerator each and are here so the design does not have to be reopened.
enum class ScopeKind : std::uint8_t {
  File,
  Function,
  Block,
  FunctionPrototype, // reserved
  Loop,              // reserved: `for`/`while` will want it
  Switch,            // reserved
  // The names **one declaration** introduces and no other declaration sees: the
  // binders of a generic `fn` or `type`. A scope rather than a list in the
  // declaration, because a binder is a name before it is anything else -- the
  // duplicate rule, the shadow rule and the reserved-word rule are the ones every
  // other name follows, and they are all keyed on a scope (`generics.md`).
  Declaration,
};

[[nodiscard]] std::string_view toString(ScopeKind value);

inline constexpr std::uint32_t kInvalidDefIndex = 0xFFFFFFFFu;
inline constexpr std::uint32_t kInvalidScope = 0xFFFFFFFFu;

// A declaration's identity.
//
// `index` addresses this unit's def array; `file` is the file the declaration
// was *written* in. Keeping both is the point: a translation unit spans several
// files, and a definition belongs to the one it was written in, so a header's
// definitions stay valid, and keep their identity, while a `.mx` body changes.
struct DefId {
  support::FileId file = support::kInvalidFile;
  std::uint32_t index = kInvalidDefIndex;

  [[nodiscard]] constexpr bool valid() const {
    return index != kInvalidDefIndex;
  }
  friend constexpr bool operator==(DefId, DefId) = default;
};

inline constexpr DefId kInvalidDef{};

// A scope's identity: an index into one unit's scope array. Scopes, unlike
// definitions, are not per file -- a translation unit's scope tree spans every
// file it includes, and it is one tree.
struct ScopeId {
  std::uint32_t index = kInvalidScope;

  [[nodiscard]] constexpr bool valid() const {
    return index != kInvalidScope;
  }
  friend constexpr bool operator==(ScopeId, ScopeId) = default;
};

inline constexpr ScopeId kInvalidScopeId{};

struct Def {
  // The whole declaration, for "declared here".
  support::Span span;
  // Just the name, as it was *written*, for a caret and for `source_to_def`.
  support::Span nameSpan;
  // The same name's range in the unit text, and the key a later stage looks a
  // declaration node up by.
  //
  // A written span is not unique, and that is not a corner case: a macro that
  // expands one argument into two names
  //
  //   #define PAIR(b) let b: i32; let CONCAT(b, 2): i32;
  //
  // gives both declarations the *same* written location -- the argument they
  // both came from -- so a map keyed on it answers with the wrong definition,
  // and it does so silently. That is a real bug this field exists to close: the
  // second name's type was never recorded, and a use of it typed as the poison
  // with no diagnostic at all. Unit offsets are one token each, so the pair is
  // unique.
  support::Span unitSpan;
  support::SymId name = support::kInvalidSym;
  DefKind kind = DefKind::Variable;
  ScopeId scope;
  Namespace ns = Namespace::Ordinary;
  Linkage linkage = Linkage::None;
  // **The identity of the thing this declaration declares.** `self` for the
  // declaration that introduced the name -- the ordinary case -- and that first
  // declaration for a repeated one.
  //
  // Two questions look alike and are not: *which def is this declaration site*
  // and *which def is this name*. A name has one answer, and it has to be one
  // answer for every stage below, because they key maps on it: `sema` stores a
  // declaration's type under it and `ir` creates one `llvm::Function` per key.
  // Two ids for one function means two types and two symbols -- and the second
  // is what LLVM renames to `f.1`, leaving the first declared and never defined.
  //
  // `self` is spelled out rather than left invalid so that "is this the canonical
  // declaration" is a comparison and not a second lookup.
  DefId canonical;
  // The next declaration of the same name in the same scope and namespace. C
  // lets a function be declared many times and lets a header be included twice,
  // so the scope table keeps the canonical definition and this chain keeps the
  // rest: the backend can see every declaration, and the IDE every site.
  //
  // The chain is for the *sites*; `canonical` is for the identity. They are
  // deliberately separate fields because they answer opposite questions and,
  // for the redeclaration path below, point in opposite directions.
  DefId nextRedundant;
  // How many name uses resolved to this definition. A count, not a list: the
  // list is derivable from the reference array and this is the question every
  // diagnostic asks.
  std::uint32_t refCount = 0;
  // True when the declaration was written inside a region the parser could not
  // understand; such a declaration is not reported again.
  bool inError = false;
  // True when a diagnostic already names this declaration -- today, a
  // redeclaration. The unused pass reads it and says nothing: one mistake, one
  // diagnostic, even when two passes could each say something about it.
  bool hasProblem = false;
  // Which name the language bound before any source was read, or `None` for an
  // ordinary declaration (`predefined.h`). These are ordinary names -- the lexer
  // deliberately does not make them keywords -- and a predefined one has no
  // declaration to point at, so it is never reported as unused and never printed
  // with a location.
  //
  // A category and not a boolean, because the three consumers ask *which* one:
  // `sema` gives `true`/`false` the boolean type and a constant value while
  // `null` is the untyped pointer, and `ir` emits a different constant for each.
  // The bool this replaces could answer "is it predefined" and nothing else,
  // which is the question none of them actually asks.
  Predefined predefined = Predefined::None;
  // Which builtin this declaration is, or `kInvalid` for an ordinary one
  // (`builtins/builtin.h`). The rows are the language's own names too, bound in
  // the file scope before anything is read, and this field is why no stage has to
  // know one by spelling: `sema` types the call from the row and `ir` lowers it,
  // both by reading *this*, so the text `__builtin_trap` appears in the tree
  // exactly once -- in the table.
  //
  // Deliberately not a `DefKind` of its own: a builtin *is* a function to every
  // stage that looks at the kind, and what differs about it -- the row -- is what
  // this field answers. A new kind would make every switch over `DefKind` ask a
  // second question it does not have an answer for.
  builtins::BuiltinId builtin = builtins::BuiltinId::None;

  [[nodiscard]] constexpr bool isFunction() const {
    return kind == DefKind::Function;
  }
};

// True for a declaration the *language* made, in the file scope, before the unit
// was read: one of the predefined names (`true`) or one of the builtin rows
// (`clz`). Neither has a declaration node or a written location, which is why
// both are parked at offset zero -- and why every position-keyed answer has to
// skip them, or a real declaration at the start of a unit would find one
// (`def_index.h`).
[[nodiscard]] constexpr bool isLanguageDef(const Def& def) {
  return isPredefined(def.predefined) || def.builtin != builtins::BuiltinId::None;
}

// The `DefId` of the declaration at `index` in a `DefMap`.
//
// The file half is the one the declaration was **written** in, and not the unit
// the compilation read: a header's declaration stays a header's declaration even
// though the unit's text is one buffer, and that is what makes "declared here"
// point at the header. `span` is the fallback for a declaration whose name has no
// written file at all -- one built by hand, in a test or a tool.
//
// One rule, in one place, and deliberately so: `resolve` builds an id this way
// when it inserts a declaration, and `source_to_def` and the offset index rebuild
// it from a position. Two spellings of "which id is this declaration" is two ids
// for one declaration -- and two ids is two types and two `llvm::Function`s, with
// LLVM renaming the loser to `f.1`.
[[nodiscard]] constexpr DefId defIdOf(const Def& def, std::uint32_t index) {
  const support::FileId owner =
      def.nameSpan.file != support::kInvalidFile ? def.nameSpan.file : def.span.file;
  return DefId{owner, index};
}

// The declaration every lookup of this name answers, from a declaration site.
//
// `self` in the ordinary case; the first declaration of the name when the one in
// hand is a repeat. The fallback exists for a `Def` built by hand -- a test, a
// stage that constructs a map of its own -- and it is here rather than at each
// call site because "which id do I key on" must have one answer in the whole
// compiler. Everything below `resolve` keys on it: a type, a frame slot, an
// `llvm::Function`.
[[nodiscard]] constexpr DefId canonicalOf(const Def& def, DefId self) {
  return def.canonical.valid() ? def.canonical : self;
}

} // namespace minc::resolve
