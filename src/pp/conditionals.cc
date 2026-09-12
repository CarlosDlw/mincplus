// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "pp/conditionals.h"

#include <cstddef>
#include <optional>

namespace minc::pp {

std::optional<PPErrorCode> ConditionalStack::open(bool condition, std::size_t limit) {
  if (frames_.size() >= limit) {
    return PPErrorCode::ConditionalNestingExceeded;
  }
  Frame frame;
  frame.parentLive = emitting();
  // A branch inside a skipped region is skipped no matter what it evaluates to,
  // so the condition is only consulted when the parent emits. That is also what
  // keeps the *skipped* branch from being evaluated at all: the caller does not
  // evaluate a condition it is not going to use.
  frame.live = frame.parentLive && condition;
  frame.taken = frame.live;
  frames_.push_back(frame);
  return std::nullopt;
}

bool ConditionalStack::shouldEvaluateElif() const {
  if (frames_.empty()) {
    return false;
  }
  const Frame& frame = frames_.back();
  return frame.parentLive && !frame.taken && !frame.seenElse;
}

std::optional<PPErrorCode> ConditionalStack::takeElif(bool condition) {
  if (frames_.empty()) {
    return PPErrorCode::UnexpectedConditional;
  }
  Frame& frame = frames_.back();
  if (frame.seenElse) {
    return PPErrorCode::ElseAfterElse;
  }
  frame.live = frame.parentLive && !frame.taken && condition;
  frame.taken = frame.taken || frame.live;
  return std::nullopt;
}

std::optional<PPErrorCode> ConditionalStack::takeElse() {
  if (frames_.empty()) {
    return PPErrorCode::UnexpectedConditional;
  }
  Frame& frame = frames_.back();
  if (frame.seenElse) {
    return PPErrorCode::ElseAfterElse;
  }
  frame.seenElse = true;
  frame.live = frame.parentLive && !frame.taken;
  frame.taken = true;
  return std::nullopt;
}

std::optional<PPErrorCode> ConditionalStack::close() {
  if (frames_.empty()) {
    return PPErrorCode::UnexpectedConditional;
  }
  frames_.pop_back();
  return std::nullopt;
}

} // namespace minc::pp
