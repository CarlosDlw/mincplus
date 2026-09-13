// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Source position -> definition. The IDE primitive, built here rather than
// bolted on later.
//
// rust-analyzer calls this the "uber-IDE pattern" -- resolve the parent syntax
// node to its parent item, ask the item for its syntax children, and pick ours
// out -- and notes it is present in Roslyn and Kotlin too. Go-to-definition,
// find-references and rename are all this mapping, which is why it is an output
// of resolution rather than something the editor has to re-derive.
#pragma once

#include <cstdint>
#include <optional>

#include "ast/ast.h"
#include "resolve/def.h"
#include "resolve/map.h"
#include "support/span/file_id.h"

namespace minc::resolve {

// The definition written at `(file, offset)`, choosing the innermost -- the
// smallest name span -- when declaration names are nested at one byte. Nullopt
// when no declaration starts there.
//
// Linear in the number of definitions, which is the right shape for the
// command and for a test. The language server wants an offset-keyed index on
// top, and the index is a `DefMap` question, not a different answer.
[[nodiscard]] std::optional<DefId> defAt(const DefMap& map, support::FileId file,
                                         std::uint32_t offset);

// The definition a name *node* declares. Used by the item map, where the node is
// already in hand.
//
// The node's *unit* range decides when it has one, because that range is one
// token per name and stays unique; the written span does not, and a macro that
// expands one argument into two names gives two declarations the same one. A
// synthetic name with no unit range falls back to the written span.
[[nodiscard]] std::optional<DefId> defOfNameNode(const DefMap& map, const ast::LoweredFile& file,
                                                 ast::AstId nameNode);

} // namespace minc::resolve
