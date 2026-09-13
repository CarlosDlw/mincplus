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
  // Just the name, for a caret and for `source_to_def`.
  support::Span nameSpan;
  support::SymId name = support::kInvalidSym;
  DefKind kind = DefKind::Variable;
  ScopeId scope;
  Namespace ns = Namespace::Ordinary;
  Linkage linkage = Linkage::None;
  // The next declaration of the same name in the same scope and namespace. C
  // lets a function be declared many times and lets a header be included twice,
  // so the scope table keeps the canonical definition and this chain keeps the
  // rest: the backend can see every declaration, and the IDE every site.
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
  // True for a name the language binds before any source is read -- today
  // `true` and `false`, which are ordinary names because the lexer deliberately
  // does not make them keywords. A predefined name has no declaration to point
  // at, so it is never reported as unused and never printed with a location.
  bool predefined = false;

  [[nodiscard]] constexpr bool isFunction() const {
    return kind == DefKind::Function;
  }
};

} // namespace minc::resolve
