// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Token classification.
//
// Tokens are classified by *what they are*, never by what a later stage will
// do with them: there is no "type name" kind or "declaration keyword" kind,
// because that would be semantic information. The kinds below are the whole
// lexical grammar, and keyword classification lives here too (see below).
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace minc::lex {

enum class TokenKind : std::uint8_t {
  EndOfFile,
  // A byte that starts no token we know. Emitted alone so the lossless
  // invariant still holds: every byte belongs to exactly one token.
  Invalid,

  // Trivia. Always produced, never skipped: the token stream is what the
  // formatter, the language server, and the lossless check consume.
  Whitespace,
  Newline,
  LineComment,
  BlockComment,

  // Literals. The raw spelling is the source text; interpreting it (value,
  // overflow, UTF-8 encoding) is a later stage's job.
  IntegerLiteral,
  FloatLiteral,
  CharLiteral,
  StringLiteral,

  Identifier,

  // A `<...>` or `"..."` file name in a directive. Part of the lexical grammar
  // (C 6.4.7) but recognized by context, so `lexOne` never returns it:
  // `lex::scanHeaderName` does. See `lex/header_name.h` for why the operand
  // cannot be reassembled from ordinary tokens.
  HeaderName,

  // Keywords. The statement keywords are here rather than left as identifiers
  // because the grammar needs them to start a statement: a `let` and an `if`
  // cannot be told apart by position alone, and the parser is trivia-blind, so
  // the classification has to be lexical.
  //
  // `KwExtern` is a *declaration* keyword rather than a statement one: it is only
  // legal immediately before `fn` at file scope, and it is lexical for the same
  // reason -- the parser decides which of the two function forms it is reading
  // from the first token of the declaration, and it may not ask a later stage.
  //
  // `KwStatic` is a declaration keyword for the same reason and beside `extern`:
  // the two are one question -- *who may see this name* -- and a reader deciding
  // what a declaration is has to know which of them was written from the first
  // token. It is a *linkage* word at file scope and not a storage duration (there
  // is one storage duration there), and it is not a statement keyword: a
  // function-local `static` is a different feature that does not exist yet.
  KwFn,
  KwExtern,
  KwStatic,
  KwLet,
  KwConst,
  KwReturn,
  KwIf,
  KwElse,
  KwWhile,
  KwFor,
  KwBreak,
  KwContinue,

  // Punctuation.
  LParen,
  RParen,
  LBrace,
  RBrace,
  LBracket,
  RBracket,
  Semicolon,
  Comma,
  Colon,
  Question,
  Dot,
  Arrow,
  // `...`. The variadic marker of a parameter list, and the same spelling a
  // variadic *macro* uses -- one token, one spelling, so the preprocessor no
  // longer has to reconstruct it from three `Dot`s and the parser never sees
  // three tokens where the language means one. The rule that the marker is only
  // legal in a declaration is a *grammar* rule and lives in the parser, which is
  // the same place every other "only here" rule lives.
  Ellipsis,

  // The preprocessor's operators. `#` and `##` are punctuators of the lexical
  // grammar -- Clang spells them `tok::hash` and `tok::hashhash`, GCC's cpplib
  // `CPP_HASH` and `CPP_HASHHASH` -- and only their *meaning* is positional.
  //
  // Classifying them here is what keeps every later stage small: the
  // preprocessor asks "is this a `Hash` at the start of a line?" instead of
  // asking "is this an unknown byte whose spelling happens to be `#`?", and it
  // never has to rebuild a `##` out of two adjacent `#` bytes. A file full of
  // directives is then a file with no lexical errors in it, which is the truth.
  Hash,
  HashHash,

  // Arithmetic.
  Plus,
  Minus,
  Star,
  Slash,
  Percent,
  PlusPlus,
  MinusMinus,

  // Bitwise.
  Amp,
  Pipe,
  Caret,
  Tilde,
  LessLess,
  GreaterGreater,

  // Logical.
  AmpAmp,
  PipePipe,
  Bang,

  // Comparison.
  EqualEqual,
  BangEqual,
  Less,
  LessEqual,
  Greater,
  GreaterEqual,

  // Assignment.
  Equal,
  PlusEqual,
  MinusEqual,
  StarEqual,
  SlashEqual,
  PercentEqual,
  AmpEqual,
  PipeEqual,
  CaretEqual,
  LessLessEqual,
  GreaterGreaterEqual,

  // Sentinel, never produced by the lexer. It gives the token range a bound at
  // compile time, so the syntax layer can pin its first node kind above every
  // token kind with a static_assert rather than a hand-maintained maximum that
  // would silently rot when a token kind is added.
  Last,
};

[[nodiscard]] constexpr bool isTrivia(TokenKind kind) {
  switch (kind) {
  case TokenKind::Whitespace:
  case TokenKind::Newline:
  case TokenKind::LineComment:
  case TokenKind::BlockComment:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] constexpr bool isKeyword(TokenKind kind) {
  switch (kind) {
  case TokenKind::KwFn:
  case TokenKind::KwExtern:
  case TokenKind::KwStatic:
  case TokenKind::KwLet:
  case TokenKind::KwConst:
  case TokenKind::KwReturn:
  case TokenKind::KwIf:
  case TokenKind::KwElse:
  case TokenKind::KwWhile:
  case TokenKind::KwFor:
  case TokenKind::KwBreak:
  case TokenKind::KwContinue:
    return true;
  default:
    return false;
  }
}

// The keywords that introduce a control-flow statement: exactly the five that
// head one and are not a declaration or a `return`. `else` is deliberately not
// here -- it continues an `if` rather than starting anything.
//
// The parser asks this instead of repeating the list, so "which words begin a
// statement" has one owner and a keyword added without a production cannot
// silently become a valid expression position.
[[nodiscard]] constexpr bool isControlFlowKeyword(TokenKind kind) {
  switch (kind) {
  case TokenKind::KwIf:
  case TokenKind::KwWhile:
  case TokenKind::KwFor:
  case TokenKind::KwBreak:
  case TokenKind::KwContinue:
    return true;
  default:
    return false;
  }
}

// Is this token a *name* to the preprocessor?
//
// The preprocessor runs in translation phase 4, and phase 4 has no keywords: a
// directive name, a macro name and the operand of `#if` are all `identifier`
// preprocessing-tokens (C 6.4), while keyword-ness is decided in phase 7, after
// every directive is gone. `#if` is therefore the hash followed by the
// *identifier* `if`, and it must go on meaning that once `if` becomes a keyword
// for the grammar -- which it is, and which is why the two spellings can never
// be told apart by anything but the hash in front of them.
//
// So this is what the preprocessor asks instead of `kind == Identifier`, at
// every place it reads a name.
[[nodiscard]] constexpr bool isIdentifierLike(TokenKind kind) {
  return kind == TokenKind::Identifier || isKeyword(kind);
}

[[nodiscard]] constexpr bool isLiteral(TokenKind kind) {
  switch (kind) {
  case TokenKind::IntegerLiteral:
  case TokenKind::FloatLiteral:
  case TokenKind::CharLiteral:
  case TokenKind::StringLiteral:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] constexpr bool isOperator(TokenKind kind) {
  switch (kind) {
  case TokenKind::Plus:
  case TokenKind::Minus:
  case TokenKind::Star:
  case TokenKind::Slash:
  case TokenKind::Percent:
  case TokenKind::PlusPlus:
  case TokenKind::MinusMinus:
  case TokenKind::Amp:
  case TokenKind::Pipe:
  case TokenKind::Caret:
  case TokenKind::Tilde:
  case TokenKind::LessLess:
  case TokenKind::GreaterGreater:
  case TokenKind::AmpAmp:
  case TokenKind::PipePipe:
  case TokenKind::Bang:
  case TokenKind::EqualEqual:
  case TokenKind::BangEqual:
  case TokenKind::Less:
  case TokenKind::LessEqual:
  case TokenKind::Greater:
  case TokenKind::GreaterEqual:
  case TokenKind::Equal:
  case TokenKind::PlusEqual:
  case TokenKind::MinusEqual:
  case TokenKind::StarEqual:
  case TokenKind::SlashEqual:
  case TokenKind::PercentEqual:
  case TokenKind::AmpEqual:
  case TokenKind::PipeEqual:
  case TokenKind::CaretEqual:
  case TokenKind::LessLessEqual:
  case TokenKind::GreaterGreaterEqual:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] constexpr bool isPunctuation(TokenKind kind) {
  switch (kind) {
  case TokenKind::LParen:
  case TokenKind::RParen:
  case TokenKind::LBrace:
  case TokenKind::RBrace:
  case TokenKind::LBracket:
  case TokenKind::RBracket:
  case TokenKind::Semicolon:
  case TokenKind::Comma:
  case TokenKind::Colon:
  case TokenKind::Question:
  case TokenKind::Dot:
  case TokenKind::Arrow:
  case TokenKind::Ellipsis:
    return true;
  default:
    return false;
  }
}

// The two operators that carry meaning only for the preprocessor. Position
// decides what they do, and position is the preprocessor's business, so a
// `Hash` that starts no directive and a `HashHash` outside a macro body are
// diagnosed there rather than rejected here. Neither ever reaches the parser.
[[nodiscard]] constexpr bool isPreprocessorOp(TokenKind kind) {
  switch (kind) {
  case TokenKind::Hash:
  case TokenKind::HashHash:
    return true;
  default:
    return false;
  }
}

// Stable name for tooling, tests, and the token dump. Never localized, never
// abbreviated, and independent of the source spelling.
[[nodiscard]] const char* toString(TokenKind kind);

struct Keyword {
  std::string_view text;
  TokenKind kind;
};

// The keyword table, in one place. The parser, the dump, and error messages
// all consult this instead of repeating the list.
//
// Primitive type names (`i32`, `u8`, ...) and the C spellings (`int`, `long`)
// are deliberately *not* here yet: they are reserved words in the language
// design but the decision to make them lexical keywords is still open, and
// until it is settled they lex as identifiers.
[[nodiscard]] std::span<const Keyword> keywords();

[[nodiscard]] std::optional<TokenKind> keywordFromText(std::string_view text);

} // namespace minc::lex
