// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "syntax/dump.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace minc::syntax {
namespace {

constexpr std::string_view kReset = "\x1b[0m";
constexpr std::string_view kNodeColor = "\x1b[1;34m"; // interior nodes: bold blue
constexpr std::string_view kLeafColor = "\x1b[32m";   // leaves: green
constexpr std::string_view kDimColor = "\x1b[2m";     // offsets: dim
constexpr std::string_view kTextColor = "\x1b[33m";   // lexemes: yellow

constexpr std::size_t kIndentWidth = 2;

[[nodiscard]] bool colored(support::ColorMode mode) {
  return mode == support::ColorMode::Ansi;
}

void appendNumber(std::string& out, std::uint32_t value) {
  if (value == 0) {
    out.push_back('0');
    return;
  }
  char digits[10];
  std::size_t count = 0;
  while (value != 0) {
    digits[count] = static_cast<char>('0' + (value % 10U));
    ++count;
    value /= 10U;
  }
  while (count != 0) {
    --count;
    out.push_back(digits[count]);
  }
}

// Escapes control characters so a tab or a newline cannot break the alignment,
// and truncates on a byte boundary only after a whole escape has been written.
[[nodiscard]] std::string escapeText(std::string_view text, std::size_t maxBytes) {
  std::string out;
  std::size_t consumed = 0;
  for (const char rawChar : text) {
    if (consumed >= maxBytes) {
      out += "...";
      break;
    }
    const auto byte = static_cast<unsigned char>(rawChar);
    switch (byte) {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (byte >= 0x20U && byte <= 0x7EU) {
        out.push_back(static_cast<char>(byte));
      } else {
        constexpr char kHex[] = "0123456789ABCDEF";
        out += "\\x";
        out.push_back(kHex[(byte >> 4U) & 0x0FU]);
        out.push_back(kHex[byte & 0x0FU]);
      }
      break;
    }
    ++consumed;
  }
  return out;
}

void appendIndent(std::string& out, std::size_t depth) {
  out.append(depth * kIndentWidth, ' ');
}

void appendKind(std::string& out, parse::SyntaxKind kind, bool isLeaf, support::ColorMode mode) {
  const std::string_view name = parse::toString(kind);
  if (colored(mode)) {
    out += isLeaf ? kLeafColor : kNodeColor;
  }
  out += name;
  if (colored(mode)) {
    out += kReset;
  }
}

void appendRange(std::string& out, std::uint32_t begin, std::uint32_t end,
                 support::ColorMode mode) {
  if (colored(mode)) {
    out += kDimColor;
  }
  out.push_back('@');
  appendNumber(out, begin);
  out += "..";
  appendNumber(out, end);
  if (colored(mode)) {
    out += kReset;
  }
}

} // namespace

std::string dumpTree(const SyntaxTree& tree, std::string_view path, DumpOptions options) {
  std::string out;
  out += "== ";
  out += path;
  out += "  (";
  appendNumber(out, static_cast<std::uint32_t>(tree.text().size()));
  out += " bytes, ";
  appendNumber(out, tree.stats().tokenCount);
  out += " tokens, ";
  appendNumber(out, tree.stats().nodeCount);
  out += " nodes, ";
  appendNumber(out, static_cast<std::uint32_t>(tree.errors().size()));
  out += " errors, depth ";
  appendNumber(out, tree.stats().maxDepth);
  out += ")\n\n";

  const GreenNode* root = tree.greenRoot();
  if (root == nullptr) {
    out += "(no tree)\n";
    return out;
  }

  // Iterative, because a deep tree must not be the thing that overflows the
  // stack -- the parser's guard bounds the tree, not this function.
  struct Frame {
    const GreenNode* node;
    std::uint32_t offset;
    std::size_t next;
    std::uint32_t childOffset;
  };

  appendKind(out, root->kind, /*isLeaf=*/false, options.color);
  appendRange(out, 0, root->width, options.color);
  out.push_back('\n');

  std::vector<Frame> stack;
  stack.push_back({root, 0, 0, 0});

  while (!stack.empty()) {
    Frame& frame = stack.back();
    if (frame.next >= frame.node->children.size()) {
      stack.pop_back();
      continue;
    }

    const GreenChild& child = frame.node->children[frame.next];
    const std::uint32_t childOffset = frame.offset + frame.childOffset;
    frame.childOffset += child.width();
    ++frame.next;

    const std::size_t depth = stack.size();

    if (child.isNode) {
      appendIndent(out, depth);
      appendKind(out, child.node->kind, /*isLeaf=*/false, options.color);
      appendRange(out, childOffset, childOffset + child.node->width, options.color);
      out.push_back('\n');

      if (options.maxDepth != 0 && depth >= options.maxDepth) {
        appendIndent(out, depth + 1);
        out += "...\n";
        continue;
      }
      stack.push_back({child.node, childOffset, 0, 0});
      continue;
    }

    if (!options.showTrivia) {
      const SyntaxToken token{child.token, childOffset};
      if (token.isTrivia()) {
        continue;
      }
    }

    appendIndent(out, depth);
    appendKind(out, child.token->kind, /*isLeaf=*/true, options.color);
    appendRange(out, childOffset, childOffset + child.token->width(), options.color);
    if (!child.token->text.empty()) {
      out.push_back(' ');
      if (colored(options.color)) {
        out += kTextColor;
      }
      out.push_back('"');
      out += escapeText(child.token->text, options.maxTextBytes);
      out.push_back('"');
      if (colored(options.color)) {
        out += kReset;
      }
    }
    out.push_back('\n');
  }

  return out;
}

} // namespace minc::syntax
