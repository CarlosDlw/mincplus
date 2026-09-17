// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The name a type position was *written* with, when it was written as a name for
// a type (`type_alias.md`, decision 8).
//
// It lives in a header of its own because it is the one fact both sides of the
// debug boundary need and neither can borrow from the other: the *lowering* knows
// the position (`sema` recorded the alias there, on the `Type` node) and the
// *debug information* knows what a DIE is made of. Putting it in `debug.h` would
// make `lowering.h` include `DIBuilder.h` -- and every `ir` translation unit
// includes `lowering.h` -- for a three-field struct; putting it in `lowering.h`
// would do the same in the other direction. So it is here, and it is the whole of
// what crosses: the name, where it was written, and which declaration it came
// from.
#pragma once

#include <cstdint>
#include <string_view>

#include "sema/typed_ast.h"
#include "support/span/span.h"

namespace minc::ir {

// Empty -- `kNoAlias` and no spelling -- is the ordinary case, and it is what a
// caller that passes nothing gets: a position written as `i32` is a type and not
// a name for one, and `[8]u8` is an expansion with no name to answer with.
struct AliasName {
  std::string_view spelling;
  support::Span span;
  // The declaration's index in `TypedFile::aliases()`, and the cache key for its
  // `DW_TAG_typedef`: one DIE per declaration, so a name used at ten positions is
  // one node, and a *block's* name of a spelling the file also uses is a second
  // node rather than a collision.
  std::uint32_t index = sema::TypedFile::kNoAlias;
};

} // namespace minc::ir
