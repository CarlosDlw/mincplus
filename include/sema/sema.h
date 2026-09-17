// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Type checking: a resolved unit in, a typed unit out.
//
// The stage contract, unchanged from the lexer on: this takes an artifact and
// returns one. It never prints, never exits, never sees a `DiagBag`; every error
// is a value with a stable code and a span, and `sema_report.h` is the only
// thing that turns those into text.
//
// `checkUnit` is the pure entry point: same inputs, same output, no hidden
// state, callable as often as a caller likes. `Context` is the centralised
// version a compilation actually uses -- it owns the type store and caches the
// result per `(FileId, revision)`, so a later stage (or the language server) can
// ask about a unit again and again and get *the same* answer instead of a second
// check. That is the property the rest of the pipeline is built on: the IR never
// has to re-derive a type, and it may re-read every type as many times as it
// needs without paying for it or risking a different verdict.
//
// Design record: `docs/architectures/sema.md`.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "ast/ast.h"
#include "resolve/map.h"
#include "sema/sema_error.h"
#include "sema/target.h"
#include "sema/type_store.h"
#include "sema/typed_ast.h"
#include "support/intern/interner.h"
#include "support/limits.h"
#include "support/span/file_id.h"

namespace minc::sema {

// Everything the checker enforces. The bounds follow the preprocessor's rule:
// they can be lowered (a test that wants to prove the bound is a bound) and
// never disabled.
struct SemaOptions {
  // Warn on the implicit narrowing of the assignment conversion. Off by
  // default: C's narrowing is what makes C code compile at all, and the
  // diagnosis a reader wants here is a lint, not an error.
  bool warnConversion = false;
  // Warn on an explicit cast that may lose something -- truncated bits, a sign
  // that changes meaning, a rounded value, or an out-of-range trap. Off by
  // default: an explicit cast is the source saying it knows, and a lint that
  // fires on every one of them is a lint nobody keeps on (`casts.md`,
  // decision 16).
  bool warnCast = false;
  // Name every cast between a pointer and an integer -- `memory.md`'s `expose`
  // and `with_exposed_provenance`. Not a lint about accuracy: it is the count the
  // model chose *instead* of an `unsafe` keyword, so it is off by default the way
  // the others are and its sites are the whole record of where an address crosses
  // into an integer and back.
  bool warnProvenance = false;
  std::size_t maxTypes = support::kMaxTypesPerUnit;
  // How many generic **instances** one unit may have. A declaration that grows its
  // own argument every time (`fn i32 g<T>(x: T) { return g::<*T>(x); }`) has no
  // last instance, so the count is where the compiler stops -- and it is *stated*
  // and settable rather than a constant nobody can see, which is the difference
  // between this and the limit rustc reports as "reached the recursion limit while
  // instantiating" (`generics.md`, § 5).
  std::size_t maxInstances = 1024;
};

struct SemaOutput {
  TypedFile typed;
  std::vector<SemaError> errors;
  std::vector<SemaError> warnings;
};

// Checks one translation unit. `defs` is the resolution of the same unit (every
// name already tied to a declaration), `symbols` the interner its `SymId`s came
// from, and `types` the compilation's type store, which the check adds to.
[[nodiscard]] SemaOutput checkUnit(const ast::LoweredFile& file, const resolve::DefMap& defs,
                                   const support::Interner& symbols, TypeStore& types,
                                   SemaOptions options = {});

// --- queries -----------------------------------------------------------------
//
// Free functions over the artifact, so a consumer does not need a `Context` to
// ask a question. They read; they never recompute, and calling one twice is
// free.

// The type of any node (the poison when it has none).
[[nodiscard]] inline TypeId typeOf(const TypedFile& typed, ast::AstId id) {
  return typed.typeOf(id);
}
[[nodiscard]] inline const ExprInfo& infoOf(const TypedFile& typed, ast::AstId id) {
  return typed.infoOf(id);
}
// The return type of the function declared at `fnDecl`, or the poison when that
// node is not a function in this unit.
[[nodiscard]] inline TypeId functionReturnType(const TypedFile& typed, ast::AstId fnDecl) {
  const FunctionInfo* info = typed.functionOf(fnDecl);
  return info == nullptr ? kTypeError : info->returnType;
}

// --- the compilation's checker -----------------------------------------------

class Context {
public:
  explicit Context(TargetInfo target = defaultTarget());
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;

  struct Stats {
    std::size_t hits = 0;   // answered from the cache
    std::size_t misses = 0; // checked, then stored
  };

  // The unit's typing, reusing the cached one when the revision is unchanged.
  //
  // Unlike resolution, a body edit **does** change this result -- a type comes
  // out of a body -- so the item tree is not the right key here and is not
  // consulted. The revision is the whole key, and pretending a signature-level
  // comparison made a body edit cheap would be a lie the first time a
  // body's type changed without its signature doing so.
  //
  // The returned pointer is stable until that file is dropped or the context is
  // cleared.
  [[nodiscard]] const SemaOutput* check(support::FileId file, std::uint32_t revision,
                                        const ast::LoweredFile& lowered,
                                        const resolve::DefMap& defs,
                                        const support::Interner& symbols, SemaOptions options = {});

  [[nodiscard]] const SemaOutput* find(support::FileId file) const;
  bool drop(support::FileId file);
  void clear();

  // The store every `TypeId` in every answer indexes. A later stage that needs
  // to spell or measure a type asks here rather than keeping a copy.
  //
  // **Mutable**, and the one consumer that needs it is the lowering: a generic
  // body is lowered once per instance, and reading one of its types through the
  // instance's substitution *builds* the type the store already had (`[4]T` with
  // `T := i32` is `[4]i32`, one intern). A `const` store would force the lowering
  // to carry a second type table per instance, which is exactly what
  // `generics.md` decision 20 removes.
  [[nodiscard]] TypeStore& types() {
    return types_;
  }
  [[nodiscard]] const TypeStore& types() const {
    return types_;
  }

  [[nodiscard]] const Stats& stats() const {
    return stats_;
  }
  [[nodiscard]] std::size_t size() const {
    return entries_.size();
  }

private:
  struct Entry {
    std::uint32_t revision = 0;
    SemaOutput output;
  };

  TypeStore types_;
  std::unordered_map<support::FileId, Entry> entries_;
  Stats stats_;
};

} // namespace minc::sema
