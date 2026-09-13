// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The operator tokens the checker names, once.
//
// The lowered tree shares one tag space for tokens and interior nodes
// (`parse/syntax_kind.h`): a token's tag *is* its `lex::TokenKind` value, below
// `kFirstNodeKind`. That is the right model -- one list of names, no second table
// to keep in step -- but it has one consequence worth naming: a token's value is
// not an *enumerator* of `SyntaxKind`, so a `switch` whose cases are tokens has
// to switch on the number, not on the enum.
//
// So the tokens are spelled here as **integer tags**, once each, and the switches
// that read them switch on `tagOf(...)`. The alternative -- casting at every case
// -- would scatter the same cast thirty times, and the alternative to *that*
// would be a second enumerator per token, which is exactly the duplication the
// shared tag space exists to avoid. Interior node kinds stay `ast::NodeKind`:
// they really are enumerators, and a switch over them stays type-checked.
//
// Only the tokens the checker names appear. A token nobody reads has no business
// being here: an entry that stops being used is an entry that silently stops
// matching.
#pragma once

#include <cstdint>

#include "ast/node.h"
#include "lex/token_kind.h"
#include "parse/syntax_kind.h"

namespace minc::sema {

// A syntax tag, as the integer the switches compare.
using Tag = std::uint16_t;

[[nodiscard]] constexpr Tag tagOf(ast::NodeKind kind) {
  return static_cast<Tag>(kind);
}
// A `lex::TokenKind` and the `SyntaxKind` it maps to have the same value by
// construction, so this is the same number without going through the tree.
[[nodiscard]] constexpr Tag tagOf(lex::TokenKind kind) {
  return static_cast<Tag>(kind);
}

// The one token the checker asks for as a *node kind*: it is compared with
// `Node::is`, which takes the enum.
inline constexpr ast::NodeKind kIdentifierNode = parse::toSyntaxKind(lex::TokenKind::Identifier);

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
// The two compound operators that are also a *step* when their left side is a
// pointer: `p += n` and `p -= n`.
inline constexpr Tag kTokPlusEqual = tagOf(lex::TokenKind::PlusEqual);
inline constexpr Tag kTokMinusEqual = tagOf(lex::TokenKind::MinusEqual);
inline constexpr Tag kTokPercentEqual = tagOf(lex::TokenKind::PercentEqual);
inline constexpr Tag kTokAmpEqual = tagOf(lex::TokenKind::AmpEqual);
inline constexpr Tag kTokPipeEqual = tagOf(lex::TokenKind::PipeEqual);
inline constexpr Tag kTokCaretEqual = tagOf(lex::TokenKind::CaretEqual);
inline constexpr Tag kTokLessLessEqual = tagOf(lex::TokenKind::LessLessEqual);
inline constexpr Tag kTokGreaterGreaterEqual = tagOf(lex::TokenKind::GreaterGreaterEqual);

} // namespace minc::sema
