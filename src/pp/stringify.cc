// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "pp/stringify.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace minc::pp {
namespace {

// Appends `spelling` to `out`, escaping the two characters the standard says
// must be escaped inside a string literal. Applied only to string and character
// literals, because those are the tokens whose spelling already contains
// quotes backslashes that would otherwise end the string early.
void appendEscaped(std::string& out, std::string_view spelling) {
  for (const char c : spelling) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
}

} // namespace

std::string stringifyTokens(const TokenText& text, std::span<const PPToken> tokens) {
  std::string message;
  message.push_back('"');
  bool lastWasSignificant = false;
  bool pendingSpace = false;
  for (const PPToken& token : tokens) {
    if (token.isPlacemarker()) {
      continue;
    }
    if (isPPTrivia(token.kind)) {
      // Trivia is not emitted; it only decides whether the *next* significant
      // token is separated from the previous one by a space.
      if (lastWasSignificant) {
        pendingSpace = true;
      }
      continue;
    }
    if (pendingSpace) {
      message.push_back(' ');
      pendingSpace = false;
    }
    const std::string_view spelling = text.spelling(token);
    // Only literals can contain a quote or a backslash that needs escaping;
    // every other token's spelling is copied verbatim.
    if (token.is(lex::TokenKind::StringLiteral) || token.is(lex::TokenKind::CharLiteral)) {
      appendEscaped(message, spelling);
    } else {
      message.append(spelling);
    }
    lastWasSignificant = true;
  }
  message.push_back('"');
  return message;
}

bool pasteTokens(TokenText& text, const PPToken& left, const PPToken& right, PPToken& out) {
  // A placemarker contributes nothing, so the other operand is the result. This
  // is what makes a paste with an empty argument produce the non-empty side
  // instead of the empty side's absence.
  if (left.isPlacemarker()) {
    out = right;
    out.flags = static_cast<PPTokenFlags>(out.flags | flagOf(PPTokenFlag::Pasted));
    return true;
  }
  if (right.isPlacemarker()) {
    out = left;
    out.flags = static_cast<PPTokenFlags>(out.flags | flagOf(PPTokenFlag::Pasted));
    return true;
  }

  std::string concatenated(text.spelling(left));
  concatenated += text.spelling(right);

  PPToken pasted;
  if (!text.relex(concatenated, pasted)) {
    return false;
  }

  // Both operands' spans are recorded: a caret on a pasted token can show the
  // two pieces that met, which pointing at one of them (as GCC and Clang do)
  // cannot.
  pasted.loc.spelling = left.loc.spelling.valid() ? left.loc.spelling : right.loc.spelling;
  if (left.loc.spelling.valid() && right.loc.spelling.valid()) {
    pasted.loc.secondOperand = right.loc.spelling;
  }
  // A pasted token inherits the invocation context of its operands: it was
  // produced where they were, so its expansion frame is theirs.
  pasted.loc.expansion = left.loc.expansion;
  pasted.flags = static_cast<PPTokenFlags>(pasted.flags | flagOf(PPTokenFlag::Pasted));
  out = pasted;
  return true;
}

} // namespace minc::pp
