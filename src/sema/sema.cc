// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The stage entry point and the compilation's centralized checker.
//
// Two ways in, and the difference is deliberate:
//
//   * `checkUnit` is pure. Same inputs, same output, no state kept. A test uses
//     it, and a caller that wants a check and nothing else uses it.
//   * `Context` is what a compilation uses. It owns the `TypeStore` -- one per
//     compilation, because a type has to mean the same thing in every unit that
//     compares against it -- and it caches a unit's answer per revision, so the
//     IR builder, the linter and the language server can all ask again and get
//     *the same* `TypedFile` instead of a second check with a possibly different
//     verdict.
//
// The cache key is the revision and nothing else. A body edit changes a type, so
// there is no signature-level shortcut to take here, and pretending otherwise
// would be the one bug this store could have.
#include "sema/sema.h"

#include <cstddef>
#include <cstdint>
#include <utility>

#include "checker.h"
#include "sema/type_store.h"
#include "support/intern/interner.h"
#include "support/span/file_id.h"

namespace minc::sema {

SemaOutput checkUnit(const ast::LoweredFile& file, const resolve::DefMap& defs,
                     const support::Interner& symbols, TypeStore& types, SemaOptions options) {
  Checker checker(file, defs, symbols, types, options);
  return checker.run();
}

Context::Context(TargetInfo target) : types_(std::move(target)) {}

const SemaOutput* Context::check(support::FileId file, std::uint32_t revision,
                                 const ast::LoweredFile& lowered, const resolve::DefMap& defs,
                                 const support::Interner& symbols, SemaOptions options) {
  const auto found = entries_.find(file);
  if (found != entries_.end() && found->second.revision == revision) {
    ++stats_.hits;
    return &found->second.output;
  }
  ++stats_.misses;

  SemaOutput output = checkUnit(lowered, defs, symbols, types_, options);
  auto [it, inserted] = entries_.insert_or_assign(file, Entry{revision, std::move(output)});
  (void)inserted;
  // The node is stable until this file is dropped or the context is cleared:
  // `unordered_map` never moves an existing element when another is added, which
  // is exactly the property every caller holds a pointer across.
  return &it->second.output;
}

const SemaOutput* Context::find(support::FileId file) const {
  const auto found = entries_.find(file);
  if (found == entries_.end()) {
    return nullptr;
  }
  return &found->second.output;
}

bool Context::drop(support::FileId file) {
  return entries_.erase(file) != 0;
}

void Context::clear() {
  entries_.clear();
}

} // namespace minc::sema
