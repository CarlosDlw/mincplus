// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "lex/lex_report.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace minc::lex {
namespace {

[[nodiscard]] char hexDigit(unsigned value) {
  return static_cast<char>(value < 10U ? ('0' + value) : ('A' + (value - 10U)));
}

[[nodiscard]] std::string byteToHex(unsigned char byte) {
  std::string out;
  out.push_back(hexDigit(byte >> 4U));
  out.push_back(hexDigit(byte & 0x0FU));
  return out;
}

void reportInvalid(const TokenStream& stream, const Token& token, support::DiagBag& diags) {
  const std::string_view spelling = stream.text().substr(token.offset, token.length);
  std::string message;

  if (token.length == 1 && !spelling.empty()) {
    const auto byte = static_cast<unsigned char>(spelling.front());
    if (byte >= 0x20U && byte <= 0x7EU) {
      message = "unexpected character '";
      if (byte == '\\' || byte == '\'') {
        message.push_back('\\'); // keep the quotes readable
      }
      message.push_back(static_cast<char>(byte));
      message.push_back('\'');
    } else {
      message = "unexpected byte 0x" + byteToHex(byte);
    }
  } else {
    message = "unexpected non-ASCII character outside a comment or string literal";
  }

  diags.error(stream.spanOf(token), std::move(message), "lex-invalid-character");
}

} // namespace

std::size_t reportLexErrors(const TokenStream& stream, support::DiagBag& diags) {
  return reportLexErrors(stream, diags, nullptr);
}

std::size_t reportLexErrors(const TokenStream& stream, support::DiagBag& diags, TokenSkip skip) {
  const std::size_t before = diags.size();

  for (const Token& token : stream.tokens()) {
    if (skip != nullptr && skip(token, stream.text())) {
      continue;
    }
    if (token.is(TokenKind::Invalid)) {
      reportInvalid(stream, token, diags);
    }
    if (!token.hasAnyFlag()) {
      continue;
    }
    // Walk the table, not the token's bits: a flag added to the enum is
    // reported as soon as it has a row, and `allTokenFlags()` is what the tests
    // check against.
    for (const FlagInfo& info : flagInfos()) {
      if (token.has(info.flag)) {
        diags.error(stream.spanOf(token), info.message, info.code);
      }
    }
  }

  return diags.size() - before;
}

} // namespace minc::lex
