// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Value-or-error without exceptions.
//
// Storage is a std::variant, so exactly one of the two states exists at a
// time; there is no "both empty because a move failed" window as there would
// be with two optionals. Constructing from a raw value means success and from
// Unexpected<E> means failure.
#pragma once

#include <optional>
#include <utility>
#include <variant>

#include "support/expected/unexpected.h"

namespace minc::support {

template <typename T, typename E> class Expected {
public:
  using ValueType = T;
  using ErrorType = E;

  // Intentionally implicit: `return value;` from a Fallible<T> function reads
  // naturally. Errors must go through makeUnexpected to stay explicit.
  Expected(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
  Expected(Unexpected<E> unexpected)
      : storage_(std::in_place_index<1>, std::move(unexpected).error()) {}

  [[nodiscard]] bool hasValue() const {
    return storage_.index() == 0;
  }
  [[nodiscard]] bool hasError() const {
    return storage_.index() == 1;
  }
  [[nodiscard]] explicit operator bool() const {
    return hasValue();
  }

  // Precondition: hasValue()/hasError() respectively. Misuse is a programming
  // error and throws std::bad_variant_access rather than returning garbage.
  [[nodiscard]] const T& value() const& {
    return std::get<0>(storage_);
  }
  [[nodiscard]] T& value() & {
    return std::get<0>(storage_);
  }
  [[nodiscard]] T&& value() && {
    return std::move(std::get<0>(storage_));
  }

  [[nodiscard]] const E& error() const& {
    return std::get<1>(storage_);
  }
  [[nodiscard]] E& error() & {
    return std::get<1>(storage_);
  }
  [[nodiscard]] E&& error() && {
    return std::move(std::get<1>(storage_));
  }

  [[nodiscard]] const T& operator*() const& {
    return value();
  }
  [[nodiscard]] T& operator*() & {
    return value();
  }

  template <typename U> [[nodiscard]] T valueOr(U&& fallback) const& {
    return hasValue() ? value() : static_cast<T>(std::forward<U>(fallback));
  }

private:
  std::variant<T, E> storage_;
};

template <typename E> class Expected<void, E> {
public:
  Expected() = default;
  Expected(Unexpected<E> unexpected) : error_(std::move(unexpected).error()) {}

  [[nodiscard]] bool hasValue() const {
    return !error_.has_value();
  }
  [[nodiscard]] bool hasError() const {
    return error_.has_value();
  }
  [[nodiscard]] explicit operator bool() const {
    return hasValue();
  }

  [[nodiscard]] const E& error() const& {
    return *error_;
  }
  [[nodiscard]] E& error() & {
    return *error_;
  }
  [[nodiscard]] E&& error() && {
    return std::move(*error_);
  }

private:
  std::optional<E> error_;
};

} // namespace minc::support
