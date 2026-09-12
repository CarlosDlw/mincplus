// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Deduplicated identifiers.
//
// The index is keyed by views into the pool. The pool is a std::deque, whose
// elements never move when it grows, so both those keys and every view
// returned by lookup() stay valid for the interner's lifetime. clear() is the
// only operation that invalidates them.
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>

#include "support/intern/sym_id.h"

namespace minc::support {

class Interner {
public:
  Interner() = default;

  // Returns kInvalidSym once the SymId space is exhausted.
  [[nodiscard]] SymId intern(std::string_view text);

  // Allocation-free: unordered_map hashes the view directly.
  [[nodiscard]] bool contains(std::string_view text) const;

  [[nodiscard]] std::string_view lookup(SymId id) const;

  [[nodiscard]] std::uint32_t size() const {
    return static_cast<std::uint32_t>(pool_.size());
  }
  [[nodiscard]] bool empty() const {
    return pool_.empty();
  }

  // Invalidates every view previously handed out.
  void clear();

private:
  std::deque<std::string> pool_;
  std::unordered_map<std::string_view, SymId> index_;
};

} // namespace minc::support
