// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// A preprocessor token, with the provenance that a macro expansion destroys.
//
// The lexer's `Token` is 12 bytes and owns no text: it is `(offset, length)`
// into *one* file. That is enough while every token comes from the file being
// read, and it stops being enough the moment a macro is involved, because a
// token's spelling can come from a header the macro was defined in while its
// position in the output comes from the invocation in the file being read. Two
// questions, two answers, so two fields:
//
//   * `loc.spelling` -- where the bytes are. Always a real file and offset; this
//     is what a caret points at and what `#` reads.
//   * `loc.expansion` -- which macro invocations produced it, as a chain. This
//     is what "in expansion of macro 'MAX', invoked here" is made of.
//
// Clang and GCC each pack an answer to both into one 32-bit location, because
// locations were already a packed integer when macro expansion was added. That
// compression is why their "spelling location" and "expansion location" are
// folklore rather than two named fields. This stage starts after the lexer, so
// it can pay the 12 bytes and be explicit.
//
// Design record: `docs/architectures/preprocessor.md`.
#pragma once

#include <cstdint>

#include "lex/token_kind.h"
#include "support/intern/sym_id.h"
#include "support/span/file_id.h"
#include "support/span/span.h"

namespace minc::pp {

// Where a token was written: a real byte range in a real file. Never synthetic
// -- a token that was not written down (a paste, a stringification) records the
// spans it was built from instead of inventing one.
struct SourceLoc {
  support::FileId file = support::kInvalidFile;
  std::uint32_t offset = 0;
  std::uint32_t length = 0;

  constexpr SourceLoc() = default;
  constexpr SourceLoc(support::FileId f, std::uint32_t o, std::uint32_t n)
      : file(f), offset(o), length(n) {}

  [[nodiscard]] constexpr bool valid() const {
    return file != support::kInvalidFile;
  }
  [[nodiscard]] constexpr std::uint32_t end() const {
    return offset + length;
  }
  [[nodiscard]] constexpr support::Span span() const {
    return support::Span(file, offset, end());
  }
};

// An index into the expansion table. `kNoExpansion` is the root frame: the
// token was written directly in the file being read, not produced by a macro.
using ExpansionId = std::uint32_t;
inline constexpr ExpansionId kNoExpansion = 0;

struct TokenLoc {
  SourceLoc spelling;
  ExpansionId expansion = kNoExpansion;

  // The second operand of a `##`, when this token was produced by one. A pasted
  // token is spelled by concatenating two spans, and pointing at only one of
  // them (which is what GCC and Clang do) makes the diagnostic a puzzle. This
  // is the token's own span when it was not pasted.
  SourceLoc secondOperand;

  [[nodiscard]] constexpr bool pasted() const {
    return secondOperand.valid();
  }
};

// What the expander needs to remember about a token, beyond what the lexer
// found. Kept separate from the lexer's flags because they answer different
// questions: the lexer's say "this lexeme is malformed", these say "this token
// is not eligible for replacement".
enum class PPTokenFlag : std::uint8_t {
  None = 0,
  // The standard's rule: a name that was not replaced because its macro was
  // disabled is "no longer available for further replacement even if it is
  // later re-examined". This only ever applies to tokens that came out of
  // argument pre-expansion.
  Ineligible = 1U << 0U,
  // The empty operand of a `##`, standing in so the paste still has two
  // operands. Zero width, and not a token in its own right.
  Placemarker = 1U << 1U,
  // Produced by `##` (spelling concatenated) or by `#` (a string literal).
  Pasted = 1U << 2U,
  Stringified = 1U << 3U,
  // A macro name the expander is currently expanding. Present so a traversal
  // can tell "this name was written here" from "this name is being replaced
  // here", which is what a highlighter and a formatter both want.
  Expanding = 1U << 4U,
  // The parameter of a function-like macro, substituted into the replacement
  // list because it is being replaced. Only the flag is stored; the index lives
  // in the definition (`MacroBodyToken::param`), because it is a property of the
  // definition rather than of the invocation.
  Param = 1U << 5U,
  // A `##` in a replacement list, standing between the two substituted operands
  // until the paste pass consumes the three of them together. A distinct flag
  // rather than "the spelling is ##", because an argument can itself contain a
  // `##` and the pass must not paste it.
  PasteOperator = 1U << 6U,
};

using PPTokenFlags = std::uint8_t;

[[nodiscard]] constexpr PPTokenFlags flagOf(PPTokenFlag flag) {
  return static_cast<PPTokenFlags>(flag);
}
[[nodiscard]] constexpr bool hasFlag(PPTokenFlags flags, PPTokenFlag flag) {
  return (flags & flagOf(flag)) != 0;
}

// Marks "this token's spelling is not in the source" in `PPToken::scratch`.
inline constexpr std::uint32_t kNoScratch = 0xFFFFFFFFu;

struct PPToken {
  lex::TokenKind kind = lex::TokenKind::EndOfFile;
  PPTokenFlags flags = 0;
  std::uint32_t length = 0; // spelling length in bytes; 0 for a placemarker
  TokenLoc loc;
  // Index into the expander's scratch buffer when the spelling has no source
  // buffer to point at -- a pasted or stringified token. `kNoScratch` for every
  // token that was written down, which is nearly all of them, so the common
  // case costs nothing and the field never has two meanings.
  std::uint32_t scratch = kNoScratch;

  [[nodiscard]] constexpr bool is(lex::TokenKind other) const {
    return kind == other;
  }
  // Is this token a name *to this stage*? Phase 4 has no keywords, so `if` is
  // an identifier here whether or not the lexer called it one -- which is what
  // keeps `#if`, `#define` and `#undef` working when the grammar claims those
  // spellings. The kind is left untouched, so the parser still sees a keyword in
  // the preprocessed stream: the two readings are a property of the question,
  // not of the token.
  [[nodiscard]] constexpr bool isName() const {
    return lex::isIdentifierLike(kind);
  }
  [[nodiscard]] constexpr bool has(PPTokenFlag flag) const {
    return hasFlag(flags, flag);
  }
  [[nodiscard]] constexpr bool isPlacemarker() const {
    return has(PPTokenFlag::Placemarker);
  }
  [[nodiscard]] constexpr bool isEndOfFile() const {
    return kind == lex::TokenKind::EndOfFile;
  }
  // The token's spelling is only available through the file it points at, so a
  // consumer that wants text needs the sources -- which is why nothing here
  // stores a string.
  [[nodiscard]] constexpr support::Span span() const {
    return loc.spelling.span();
  }
};

// Trivia in this stage's stream: whitespace and comments. It is *kept* -- the
// preprocessed stream is the whole front end's input, and the tree builder
// needs the whitespace to hold every byte -- but it is not what the grammar
// reads, so the parser skips it exactly as it does over a lexed file. A comment
// never survives a directive-free copy unchanged in *kind*: whitespace is copied
// verbatim so a file with no directives comes out byte-identical, while the
// space between two expanded tokens is synthesized by the emitter.
[[nodiscard]] constexpr bool isPPTrivia(lex::TokenKind kind) {
  return lex::isTrivia(kind);
}

} // namespace minc::pp
