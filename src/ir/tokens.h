// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The operator and keyword tokens the lowering names, once.
//
// The lowered tree shares one tag space for tokens and interior nodes
// (`parse/syntax_kind.h`): a token's tag *is* its `lex::TokenKind` value, which
// is a number and not an enumerator of `SyntaxKind`. So a switch whose cases are
// tokens has to switch on that number, and the tokens are spelled here as
// integer tags -- the same decision `sema/tokens.h` makes, and for the same
// reason: a cast at every case would scatter the same cast thirty times, and the
// alternative (a second enumerator per token) is the duplication the shared tag
// space exists to avoid.
//
// `ast::NodeKind` is an alias of `parse::SyntaxKind`, so the two `tagOf`
// overloads below are the two spellings of one number.
#pragma once

#include <cstdint>

#include "ast/node.h"
#include "lex/token_kind.h"
#include "parse/syntax_kind.h"

namespace minc::ir {

// A syntax tag, as the integer the switches compare.
using Tag = std::uint16_t;

[[nodiscard]] constexpr Tag tagOf(ast::NodeKind kind) {
  return static_cast<Tag>(kind);
}
[[nodiscard]] constexpr Tag tagOf(lex::TokenKind kind) {
  return static_cast<Tag>(kind);
}

// Literals.
inline constexpr Tag kTokIntegerLiteral = tagOf(lex::TokenKind::IntegerLiteral);
inline constexpr Tag kTokFloatLiteral = tagOf(lex::TokenKind::FloatLiteral);
inline constexpr Tag kTokCharLiteral = tagOf(lex::TokenKind::CharLiteral);
inline constexpr Tag kTokStringLiteral = tagOf(lex::TokenKind::StringLiteral);

// Unary.
inline constexpr Tag kTokPlusPlus = tagOf(lex::TokenKind::PlusPlus);
inline constexpr Tag kTokMinusMinus = tagOf(lex::TokenKind::MinusMinus);
inline constexpr Tag kTokPlus = tagOf(lex::TokenKind::Plus);
inline constexpr Tag kTokMinus = tagOf(lex::TokenKind::Minus);
inline constexpr Tag kTokStar = tagOf(lex::TokenKind::Star);
inline constexpr Tag kTokSlash = tagOf(lex::TokenKind::Slash);
inline constexpr Tag kTokPercent = tagOf(lex::TokenKind::Percent);
inline constexpr Tag kTokTilde = tagOf(lex::TokenKind::Tilde);
inline constexpr Tag kTokBang = tagOf(lex::TokenKind::Bang);

// Bitwise and shifts.
inline constexpr Tag kTokAmp = tagOf(lex::TokenKind::Amp);
inline constexpr Tag kTokPipe = tagOf(lex::TokenKind::Pipe);
inline constexpr Tag kTokCaret = tagOf(lex::TokenKind::Caret);
inline constexpr Tag kTokLessLess = tagOf(lex::TokenKind::LessLess);
inline constexpr Tag kTokGreaterGreater = tagOf(lex::TokenKind::GreaterGreater);

// Logical.
inline constexpr Tag kTokAmpAmp = tagOf(lex::TokenKind::AmpAmp);
inline constexpr Tag kTokPipePipe = tagOf(lex::TokenKind::PipePipe);

// Comparison.
inline constexpr Tag kTokEqualEqual = tagOf(lex::TokenKind::EqualEqual);
inline constexpr Tag kTokBangEqual = tagOf(lex::TokenKind::BangEqual);
inline constexpr Tag kTokLess = tagOf(lex::TokenKind::Less);
inline constexpr Tag kTokLessEqual = tagOf(lex::TokenKind::LessEqual);
inline constexpr Tag kTokGreater = tagOf(lex::TokenKind::Greater);
inline constexpr Tag kTokGreaterEqual = tagOf(lex::TokenKind::GreaterEqual);

// Assignment.
inline constexpr Tag kTokEqual = tagOf(lex::TokenKind::Equal);
inline constexpr Tag kTokPlusEqual = tagOf(lex::TokenKind::PlusEqual);
inline constexpr Tag kTokMinusEqual = tagOf(lex::TokenKind::MinusEqual);
inline constexpr Tag kTokStarEqual = tagOf(lex::TokenKind::StarEqual);
inline constexpr Tag kTokSlashEqual = tagOf(lex::TokenKind::SlashEqual);
inline constexpr Tag kTokPercentEqual = tagOf(lex::TokenKind::PercentEqual);
inline constexpr Tag kTokAmpEqual = tagOf(lex::TokenKind::AmpEqual);
inline constexpr Tag kTokPipeEqual = tagOf(lex::TokenKind::PipeEqual);
inline constexpr Tag kTokCaretEqual = tagOf(lex::TokenKind::CaretEqual);
inline constexpr Tag kTokLessLessEqual = tagOf(lex::TokenKind::LessLessEqual);
inline constexpr Tag kTokGreaterGreaterEqual = tagOf(lex::TokenKind::GreaterGreaterEqual);

} // namespace minc::ir
