// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include <utility>

namespace minc::support {

// Tag type marking the error side of Expected, so that a type usable as both
// value and error is still unambiguous at the call site.
template <typename E> class Unexpected {
public:
  explicit Unexpected(E err) : err_(std::move(err)) {}

  [[nodiscard]] const E& error() const& {
    return err_;
  }
  [[nodiscard]] E& error() & {
    return err_;
  }
  [[nodiscard]] E&& error() && {
    return std::move(err_);
  }

private:
  E err_;
};

template <typename E> [[nodiscard]] Unexpected<E> makeUnexpected(E err) {
  return Unexpected<E>(std::move(err));
}

} // namespace minc::support
