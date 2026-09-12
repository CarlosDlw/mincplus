// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "syntax/store.h"

#include <cstdint>
#include <optional>
#include <utility>

#include "syntax/builder.h"

namespace minc::syntax {

const SyntaxTree* TreeStore::parse(const lex::TokenStream& stream, std::uint32_t revision) {
  const support::FileId file = stream.file();

  // Invalidate before building, not after: if the build fails, a tree from the
  // previous revision must not be left behind still claiming to describe the
  // file the caller just handed in.
  trees_.erase(file);

  std::optional<SyntaxTree> tree = buildSyntaxTree(cache_, stream, revision);
  if (!tree.has_value()) {
    return nullptr;
  }

  // The map's element addresses are stable, so the returned pointer stays valid
  // across later insertions for other files.
  const auto [position, inserted] = trees_.emplace(file, std::move(*tree));
  static_cast<void>(inserted);
  return &position->second;
}

const SyntaxTree* TreeStore::find(support::FileId file) const {
  const auto position = trees_.find(file);
  return position == trees_.end() ? nullptr : &position->second;
}

bool TreeStore::drop(support::FileId file) {
  return trees_.erase(file) != 0;
}

void TreeStore::clear() {
  trees_.clear();
  cache_.clear();
}

} // namespace minc::syntax
