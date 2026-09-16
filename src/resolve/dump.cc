// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "resolve/dump.h"

#include "builtins/builtin.h"

#include <cstddef>
#include <cstdint>
#include <string>

#include "ast/node.h"
#include "resolve/def.h"
#include "resolve/map.h"
#include "support/intern/interner.h"
#include "support/line/line_table.h"
#include "support/source/source_file.h"
#include "support/source/source_manager.h"
#include "support/span/span.h"

namespace minc::resolve {
namespace {

[[nodiscard]] std::string scopeLabel(const DefMap& map, ScopeId id) {
  const Scope& scope = map.scope(id);
  return std::string(toString(scope.kind)) + "#" + std::to_string(id.index);
}

} // namespace

std::string locationOf(const support::SourceManager& sources, const support::Span& span) {
  const support::SourceFile* file = sources.find(span.file);
  if (file == nullptr) {
    return "<unknown>";
  }
  const support::LineCol at = file->lookup(span.begin);
  return file->path + ":" + std::to_string(at.line) + ":" + std::to_string(at.col);
}

std::string dumpDefMap(const DefMap& map, const ast::LoweredFile& file,
                       const support::Interner& symbols, const support::SourceManager& sources,
                       DefMapDumpOptions options) {
  std::string out;
  // The counts belong to the caller's summary line; this prints the tables.
  if (options.showScopes) {
    out += "\n  scopes\n";
    for (std::uint32_t i = 0; i < map.scopes.size(); ++i) {
      const Scope& scope = map.scopes[i];
      out += "    #" + std::to_string(i);
      out += "  ";
      out += toString(scope.kind);
      out += "  ";
      out += std::to_string(scope.span.begin) + ".." + std::to_string(scope.span.end);
      if (scope.parent.valid()) {
        out += "  parent #" + std::to_string(scope.parent.index);
      } else {
        out += "  (root)";
      }
      out += '\n';
    }
  }

  if (options.showDefs) {
    out += "\n  defs\n";
    for (std::uint32_t i = 0; i < map.defs.size(); ++i) {
      const Def& def = map.defs[i];
      out += "    #" + std::to_string(i);
      out += "  " + scopeLabel(map, def.scope);
      out += "  " + std::string(toString(def.ns));
      out += "  " + std::string(toString(def.kind));
      out += "  " + std::string(symbols.lookup(def.name));
      out += "  refs " + std::to_string(def.refCount);
      if (isPredefined(def.predefined)) {
        // *Which* one, not merely that it is one: the dump is read when a name
        // behaves unexpectedly, and "predefined" alone would leave the reader to
        // guess whether it is a value or the null pointer.
        out += "  [predefined " + std::string(toString(def.predefined)) + "]";
      } else if (def.builtin != builtins::BuiltinId::None) {
        // A prelude name or a reserved one, and *which*: the same rule as above,
        // and the one field that tells the reader this declaration has no
        // source at all.
        const builtins::BuiltinInfo* row = builtins::lookup(def.builtin);
        out += "  [builtin " + (row != nullptr ? std::string(row->spelling) : std::string("?")) +
               ", " + std::string(row != nullptr ? toString(row->spellingClass) : "?") + "]";
      } else {
        out += "  " + locationOf(sources, def.nameSpan);
      }
      out += '\n';
    }
  }

  if (options.showRefs || options.showUnresolved) {
    out += "\n  refs\n";
    for (const NameRef& ref : map.refs) {
      const bool resolved = ref.resolved();
      if (options.showUnresolved && resolved) {
        continue;
      }
      out += "    " + locationOf(sources, ref.span);
      out += "  " + std::string(symbols.lookup(ref.name));
      if (resolved) {
        out += "  -> defs#" + std::to_string(ref.target.index);
      } else {
        out += "  -> unresolved (" + std::string(toString(ref.reason)) + ")";
        if (ref.suggestion != support::kInvalidSym) {
          out += "  did-you-mean '" + std::string(symbols.lookup(ref.suggestion)) + "'";
        }
      }
      out += '\n';
    }
  }

  // `file` is unused today -- the spans already name their file -- but the
  // parameter keeps the mapping honest: a dump always has the unit it describes.
  (void)file;
  return out;
}

} // namespace minc::resolve
