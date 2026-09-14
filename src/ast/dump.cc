// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "ast/dump.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ast/node.h"
#include "parse/syntax_kind.h"
#include "support/intern/interner.h"
#include "support/span/span.h"

namespace minc::ast {
namespace {

[[nodiscard]] std::string lexeme(std::string_view text, std::size_t maxBytes) {
  if (text.size() <= maxBytes) {
    return std::string(text);
  }
  return std::string(text.substr(0, maxBytes)) + "...";
}

[[nodiscard]] std::string rangeText(const support::Span& span) {
  return std::to_string(span.begin) + ".." + std::to_string(span.end);
}

// The item tree as a table: the spelled signature is the whole point of the
// structure, so it is what is printed -- name, arity, whether a body exists, and
// where it was written.
[[nodiscard]] std::string dumpItems(const LoweredFile& file) {
  std::string out;
  const ItemTree& items = file.items();
  out += "# items " + std::to_string(items.items.size()) + "  hash " + std::to_string(items.hash) +
         "\n";
  for (std::size_t i = 0; i < items.items.size(); ++i) {
    const Item& item = items.items[i];
    out += std::to_string(i);
    out += "  ";
    out += parse::toString(item.kind);
    out += "  ";
    // Read the name back out of the declaration's own `Name` child, so the text
    // comes from the unit text (where the bytes are) rather than from the origin
    // span, which may point into a header or a macro body.
    out += file.spellingOf(file.childOfKind(AstId{item.node}, NodeKind::Name));
    out += "  params=" + std::to_string(item.paramCount);
    if (item.variadic) {
      out += ",...";
    }
    out += item.hasBody ? "  body" : "  no-body";
    out += "  ";
    out += rangeText(item.span);
    out += "\n";
  }
  return out;
}

} // namespace

std::string dumpAst(const LoweredFile& file, AstDumpOptions options) {
  if (options.items) {
    return dumpItems(file);
  }

  std::string out;
  out += "# lowered " + std::to_string(file.nodeCount()) + " nodes, " +
         std::to_string(file.children().size()) + " child slots\n";

  // An explicit stack, so a deep tree is a long dump rather than a deep call.
  struct Frame {
    AstId id;
    std::uint32_t depth;
  };
  std::vector<Frame> stack;
  stack.push_back(Frame{file.root(), 0});
  while (!stack.empty()) {
    const Frame frame = stack.back();
    stack.pop_back();

    const Node& node = file.at(frame.id);
    out += std::to_string(frame.id.index);
    out += ' ';
    out.append(static_cast<std::size_t>(frame.depth) * 2, ' ');
    out += parse::toString(node.kind);

    if (node.name != support::kInvalidSym) {
      out += " <";
      out += file.spellingOf(node);
      out += ">";
    } else if (node.isToken()) {
      out += " \"";
      out += lexeme(file.spellingOf(node), options.maxTextBytes);
      out += '"';
    }

    out += "  ";
    out += rangeText(node.unit);
    // The origin is only interesting when it differs: printing it on every line
    // of a file with no includes would be four columns of the same path.
    if (node.origin.file != node.unit.file || node.origin.begin != node.unit.begin ||
        node.origin.end != node.unit.end) {
      out += "  @";
      out += std::to_string(node.origin.file);
      out += ':';
      out += rangeText(node.origin);
    }
    if (node.inError) {
      out += "  [error region]";
    }
    out += '\n';

    const std::span<const AstId> kids = file.childrenOf(frame.id);
    for (std::size_t i = kids.size(); i > 0; --i) {
      stack.push_back(Frame{kids[i - 1], frame.depth + 1});
    }
  }
  return out;
}

} // namespace minc::ast
