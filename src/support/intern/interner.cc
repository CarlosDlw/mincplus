// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "support/intern/interner.h"

#include "support/limits.h"

namespace minc::support {

SymId Interner::intern(std::string_view text) {
  const auto found = index_.find(text);
  if (found != index_.end()) {
    return found->second;
  }
  if (pool_.size() >= kMaxSymbols) {
    return kInvalidSym;
  }

  const auto id = static_cast<SymId>(pool_.size());
  pool_.emplace_back(text);
  // The key points into the pool element just added; deque growth moves the
  // element object at most once and never moves its buffer after that.
  index_.emplace(pool_.back(), id);
  return id;
}

bool Interner::contains(std::string_view text) const {
  return index_.find(text) != index_.end();
}

std::string_view Interner::lookup(SymId id) const {
  if (id >= pool_.size()) {
    return {};
  }
  return pool_[id];
}

void Interner::clear() {
  pool_.clear();
  index_.clear();
}

} // namespace minc::support
