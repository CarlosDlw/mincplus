// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The `#if` family's skip stack.
//
// Deliberately not part of the expander: whether tokens are being emitted is a
// function of the conditional nesting *alone*, so it is decidable without
// touching a macro and is therefore testable without one. The preprocessor asks
// one question -- `emitting()` -- on every token, and the answer is the top
// frame's `live` bit, because each frame already folds in whether its parents
// emit.
//
// Every failure is a value with a name, never an assertion: a stray `#endif` in
// a header from the internet is a diagnostic, not a crash.
#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "pp/pp_error.h"

namespace minc::pp {

class ConditionalStack {
public:
  struct Frame {
    // Whether the *enclosing* region emits. Folded into `live` at push time so
    // `emitting()` is one load.
    bool parentLive = true;
    // Whether the region this frame currently governs emits.
    bool live = true;
    // Whether some branch of this conditional has already been taken.
    bool taken = false;
    // Whether `#else` has been seen, which makes a further `#elif`/`#else` an
    // error rather than a second silent branch.
    bool seenElse = false;
  };

  // True when everything at the current nesting level is being emitted. The
  // common case is an empty stack, which emits.
  [[nodiscard]] bool emitting() const {
    return frames_.empty() || frames_.back().live;
  }
  [[nodiscard]] bool empty() const {
    return frames_.empty();
  }
  [[nodiscard]] std::size_t size() const {
    return frames_.size();
  }

  // `#if`/`#ifdef`/`#ifndef`. Fails only on the nesting limit.
  [[nodiscard]] std::optional<PPErrorCode> open(bool condition, std::size_t limit);

  // Whether the *next* `#elif` branch's condition should be evaluated at all.
  // False when a branch was already taken, when the enclosing region does not
  // emit, or when there is nothing open -- in all three cases the condition's
  // text must not be evaluated, because it may not even compile.
  [[nodiscard]] bool shouldEvaluateElif() const;

  // `#elif`/`#elifdef`/`#elifndef`. `#elif` after `#else` is `ElseAfterElse`;
  // with nothing open it is `UnexpectedConditional`.
  [[nodiscard]] std::optional<PPErrorCode> takeElif(bool condition);

  // `#else`.
  [[nodiscard]] std::optional<PPErrorCode> takeElse();

  // `#endif`.
  [[nodiscard]] std::optional<PPErrorCode> close();

  void clear() {
    frames_.clear();
  }

private:
  std::vector<Frame> frames_;
};

} // namespace minc::pp
