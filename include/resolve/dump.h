// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Human-readable rendering of a resolved unit, for `mincc resolve`.
//
// Pure formatting: it returns a string and never writes, so a test can pin the
// exact output and the driver decides where it goes. It prints kinds, scopes and
// locations and never an address, so the same input produces byte-identical
// output on every run and every platform -- which is what lets the modes be
// compared and what makes "determinism" a test rather than an aspiration.
#pragma once

#include <string>

#include "ast/ast.h"
#include "resolve/map.h"
#include "support/intern/interner.h"
#include "support/source/source_manager.h"

namespace minc::resolve {

struct DefMapDumpOptions {
  bool showScopes = true;
  bool showDefs = true;
  // Every name use and its target.
  bool showRefs = false;
  // Only the uses with no target, with their reasons.
  bool showUnresolved = false;
};

// `<path>:<line>:<col>` for a span, or `<unknown>` when the file is not loaded.
[[nodiscard]] std::string locationOf(const support::SourceManager& sources,
                                     const support::Span& span);

[[nodiscard]] std::string dumpDefMap(const DefMap& map, const ast::LoweredFile& file,
                                     const support::Interner& symbols,
                                     const support::SourceManager& sources,
                                     DefMapDumpOptions options = {});

} // namespace minc::resolve
