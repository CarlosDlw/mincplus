// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "lex/header_name.h"

#include <cstddef>

namespace minc::lex {
namespace {

// `h-char` and `q-char` both exclude the newline; they differ only in the byte
// that terminates them. A CR counts as one for the same reason `LineTable`
// treats it as one: the two must not disagree about where a line ends.
[[nodiscard]] bool endsLine(char c) {
  return c == '\n' || c == '\r';
}

} // namespace

std::optional<HeaderName> scanHeaderName(std::string_view text, std::uint32_t offset) {
  if (offset >= text.size()) {
    return std::nullopt;
  }
  const char open = text[offset];
  if (open != '<' && open != '"') {
    return std::nullopt;
  }
  const char close = open == '<' ? '>' : '"';

  // Scanned as bytes, with no escape handling and no comment handling: that is
  // the whole point of the grammar being written this way.
  for (std::size_t i = offset + 1; i < text.size(); ++i) {
    if (endsLine(text[i])) {
      break;
    }
    if (text[i] != close) {
      continue;
    }
    HeaderName name;
    name.offset = offset;
    name.length = static_cast<std::uint32_t>(i + 1 - offset);
    name.angle = open == '<';
    name.text = text.substr(offset + 1, i - (offset + 1));
    return name;
  }
  return std::nullopt;
}

} // namespace minc::lex
