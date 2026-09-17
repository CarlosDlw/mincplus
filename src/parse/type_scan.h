// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The **lexical shape of a type**, in one place: how far a type run reaches where
// it is written, and whether a `<` after a word is an argument list or the
// comparison operator it also is.
//
// Two readers ask these questions and must not answer them differently. The
// declaration reader has to split `fn Ret Name(...)` into a type and a name, and
// it finds the split by counting the run; the expression reader has to read the
// type after `as`, where the very same `<` may belong to the expression around it
// (`x as i32 < 3`). The walk lives here, not inside either of them, so a spelling
// that closes a list is one fact about the language (`casts.md`, decision 18).
//
// Nothing here builds a node, reports an error, or judges whether a type is
// *well formed*. This is a scan; every sentence is the reader's, one stage down.
#pragma once

#include <cstdint>

#include "parse/parser.h"
#include "support/typenames/type_name.h"
#include "token_class.h"

namespace minc::parse {

// What the scan of one `<...>` list found.
struct TypeArgScan {
  // One past the token that closed it -- or the end of the file, when nothing
  // did, because the scan has to stop somewhere and the reader is the one that
  // reports an unterminated list.
  std::uint32_t next = 0;
  // The token that closed it. The index a caller needs to judge the contents, and
  // meaningless unless `closed`.
  std::uint32_t close = 0;
  bool closed = false;
  // The closer carried a `>` the innermost list did not need: `A<B>>`, or the
  // `>>` of a shift. Which of the two it is is the *caller's* question -- this
  // reports how many points were written, not what they mean.
  bool stray = false;
  // The closer carried an `=` too (`>=`, `>>=`). In a declaration some `let` or
  // `const` owns that character; in a cast nobody does, and it is a mistake the
  // stray-closer sentence names -- with the *list* still read, which is why this
  // is asked before the two rules below rather than after them.
  bool sawEqual = false;
};

// One past the `>` that closes the argument list whose `<` is at `open`, walking
// the four spellings a closer has.
//
// The *rule* of closing -- which tokens close one list, and which close two -- is
// `Parser::closeList`'s, and this is the lookahead's copy of it, because a scan
// cannot parse. The two are held together by a test that runs every spelling
// through both: a drift here reads the name of a declaration from the wrong
// token, which is the one mistake this scan exists to prevent.
[[nodiscard]] inline TypeArgScan scanTypeArgList(const Parser& parser, std::uint32_t open) {
  TypeArgScan list;
  list.close = open;
  // `open` indexes the `<`, so the first iteration is what depth 1 means.
  std::uint32_t i = open;
  std::int32_t depth = 0;
  while (true) {
    const lex::TokenKind kind = parser.nth(i);
    if (kind == lex::TokenKind::EndOfFile) {
      list.next = i;
      return list;
    }
    std::int32_t closed = 0;
    switch (kind) {
    case lex::TokenKind::Less:
      ++depth;
      break;
    case lex::TokenKind::Greater:
    case lex::TokenKind::GreaterEqual:
      closed = 1;
      break;
    case lex::TokenKind::GreaterGreater:
    case lex::TokenKind::GreaterGreaterEqual:
      closed = 2;
      break;
    default:
      break;
    }
    if (closed != 0) {
      depth -= closed;
      if (depth <= 0) {
        // `depth < 0` is one `>` more than the lists opened. Kept as a fact about
        // the characters rather than resolved here, because the two readings --
        // a typo, and a shift -- differ in what *follows* the token.
        list.next = i + 1;
        list.close = i;
        list.closed = true;
        list.stray = depth < 0;
        list.sawEqual =
            kind == lex::TokenKind::GreaterEqual || kind == lex::TokenKind::GreaterGreaterEqual;
        return list;
      }
    }
    ++i;
  }
}

// Every token between the brackets has to be one a type can hold.
//
// Not "is this a well-formed type" -- that is the reader's word and it gets to
// say it -- but "is this of the right *kind* to be one": a word, a constructor, a
// bracket or paren group, a comma, or a list inside. A number is one of those
// only inside `[N]`, and a literal or an operator is none of them anywhere, which
// is what keeps `x as Foo < 3 > 2` the comparison it is instead of a type named
// `3`. The shape of the list -- that the elements are separated by commas and
// that its closer closes it -- is `scanTypeArgList`'s answer, not this one's.
[[nodiscard]] inline bool listContentsAreTypes(const Parser& parser, std::uint32_t open,
                                               std::uint32_t close) {
  std::uint32_t brackets = 0;
  std::uint32_t parens = 0;
  for (std::uint32_t i = open + 1; i < close; ++i) {
    switch (parser.nth(i)) {
    case lex::TokenKind::Identifier:
    case lex::TokenKind::Star:
    case lex::TokenKind::Bang:
    case lex::TokenKind::Less:
    case lex::TokenKind::Greater:
    case lex::TokenKind::GreaterEqual:
    case lex::TokenKind::GreaterGreater:
    case lex::TokenKind::GreaterGreaterEqual:
    case lex::TokenKind::Comma:
      break;
    case lex::TokenKind::IntegerLiteral:
      if (brackets == 0) {
        return false;
      }
      break;
    case lex::TokenKind::LBracket:
      ++brackets;
      break;
    case lex::TokenKind::RBracket:
      if (brackets == 0) {
        return false;
      }
      --brackets;
      break;
    case lex::TokenKind::LParen:
      ++parens;
      break;
    case lex::TokenKind::RParen:
      if (parens == 0) {
        return false;
      }
      --parens;
      break;
    default:
      return false;
    }
  }
  return true;
}

// Is the `<` at `open` the start of a real argument list?
//
// This is the one question a **cast** position has and no other type position
// does: after `as`, a `<` may belong to the expression around the cast. Three
// facts settle it, and they are asked in the order of how much they cost:
//
//   1. A list hangs off a *word*: `Vec<i32>`. A `<` after a closer is an operator
//      (`x as Vec<i32> < y` is a comparison on the cast's result), and there is no
//      list to consider.
//   2. A **reserved** type name takes no arguments, so there is nothing to decide:
//      `x as i32 < 3` is a comparison, and so is `x as i32 < y > 2`. This is the
//      case a reader that took every `<` for a list would break, and the reason
//      this position can have both (`casts.md`, decision 19).
//   3. What lies between the brackets has to be a list of types, and it has to be
//      *closed* by the closer the scan found. `x as Foo < 3 > 2` is a comparison,
//      and with this answered the sentence the reader gives is about the
//      comparison instead of about a type named `3`.
//
// What *follows* the closer decides the rest, because the characters themselves
// cannot: either reading of `W < ... > ...` is the same bytes, and the token after
// them is what says which one the reader wrote. Two shapes, and neither of them
// can follow a complete cast -- this grammar has no juxtaposition, so a type ends
// where an operator or a terminator begins:
//
//   - a **stray** `>` with an expression after it is a shift: `x as Foo < y >> 2`
//     is `(x as Foo) < (y >> 2)`, a legal program (`>>` binds tighter than `<`).
//     None of that and the extra `>` is a point written one character too long,
//     which is what the stray-closer sentence says (`x as A<B>>`);
//   - a `>` that closed the list exactly, with a **word or a literal** after it, is
//     an ordering comparison whose left side is the cast: `x as Foo < y > 2` has
//     `y` and `2` where a cast has nothing, and it is a *chain*, which has a
//     sentence of its own. An operator there is not evidence of anything --
//     `x as Vec<i32> * 2` is a multiplication on the cast's result, and `[`, `(`,
//     `.` and `;` are the expression or the terminator the type left room for.
//
// A closer that carried an `=` is exempt: `>=` in a cast is a character the reader
// wrote by accident, and the list is read so the stray sentence can name it
// (`x as Pair<i32, bool>= 1`).
[[nodiscard]] inline bool typeArgListIsReal(const Parser& parser, std::uint32_t open) {
  if (open == 0 || parser.nth(open - 1) != lex::TokenKind::Identifier) {
    return false;
  }
  if (support::isTypeNameWord(parser.text(open - 1))) {
    return false;
  }
  const TypeArgScan list = scanTypeArgList(parser, open);
  if (!list.closed) {
    // An **unterminated** `<...>`: nothing closed it, so nothing after the closer
    // can be evidence, and the two readings are the same tokens all the way to the
    // end of the line. `x as Foo < y` is a legal comparison and stays one. But when
    // the very first token after the `<` is a **reserved type word**, the comparison
    // reading does not exist at all -- a type word has no value, so it can never be
    // the operand of `<` -- and the reader that says the list is missing its `>` is
    // the only one describing something the reader could have meant (`x as
    // Vec<i32;`).
    return parser.nth(open + 1) == lex::TokenKind::Identifier &&
           support::isTypeNameWord(parser.text(open + 1));
  }
  if (!list.sawEqual) {
    const lex::TokenKind after = parser.nth(list.next);
    if (list.stray ? isExpressionStart(after)
                   : (after == lex::TokenKind::Identifier || lex::isLiteral(after))) {
      return false;
    }
  }
  return listContentsAreTypes(parser, open, list.close);
}

// What the scan of one type run found. It answers the two questions the
// declaration reader has -- how many tokens the run occupies, and where its last
// *word* ends -- in one walk, because a second walk is a second rule about what a
// word is (`tuples.md`).
struct TypeRunScan {
  // Tokens the run occupies, from here.
  std::uint32_t tokens = 0;
  // Words at the *top level* of the run: an identifier, or a whole `(T, U)`
  // group. The identifiers **inside** a group are not words of this run, which is
  // what keeps `fn (i32, bool) f()` from reading `bool` (or `f`) as the wrong
  // word -- the name of a function is the last word of the run, and a product is
  // one word.
  std::uint32_t words = 0;
  // The index of the last word's **first token** -- the name of the declaration.
  // Everything before it is the type, and it is deliberately not "one past the
  // word": `identity<T>` is *one* word whose name is `identity`, and the list
  // that follows the identifier is the declaration's, not the word's.
  std::uint32_t nameStart = 0;
  // That last word was a `(T, U)` group and not an identifier: a run that ends
  // this way has no name in it, and the reader has to say so.
  bool lastWordIsGroup = false;
};

// Where the run is being read. Two positions, and both of their differences are
// consequences of that and not switches to remember:
//
//   - a **declaration**'s run covers the type *and* the name (`fn i32 main(`), so
//     it keeps going after a word, and its `<` is always a list -- the run ends at
//     the `(` of the parameter list, so there is no expression for it to be a
//     comparison of;
//   - a **cast**'s run covers the type only, so it stops after the word and its
//     list: `x as i32 * 2` is a multiplication and `x as Vec<i32> < y` is a
//     comparison, and a run that kept reading would take the `*` for a pointer and
//     the `<` for a list.
enum class RunKind : std::uint8_t {
  Declaration,
  Cast,
};

// A run, from the current token: the constructors, the words, the groups, and the
// `<...>` lists. It stops at the first token that cannot continue a type, which is
// what makes it reusable both for a whole position and for one member of a
// product -- the member's run ends at a `,` or a `)` and needs no second loop.
[[nodiscard]] inline TypeRunScan scanTypeRun(const Parser& parser, RunKind position) {
  TypeRunScan run;
  std::uint32_t tokens = 0;
  while (true) {
    const lex::TokenKind kind = parser.nth(tokens);
    if (kind == lex::TokenKind::Star || kind == lex::TokenKind::Identifier ||
        // `!`, the bottom type. It is a type *token* rather than a word, which
        // is why it is listed beside the two the grammar already had: a run is
        // still what a type position holds, and `!` takes part in it exactly
        // where a word would -- `fn ! f()` is a return type and a name, and the
        // reader below splits the run the same way it splits `fn i32 f()`.
        kind == lex::TokenKind::Bang) {
      if (kind == lex::TokenKind::Identifier) {
        // The one place a *word* is counted, so "how many words" and "where the
        // name starts" cannot disagree.
        run.words += 1;
        run.nameStart = tokens;
        run.lastWordIsGroup = false;
        ++tokens;
        // `<...>`, the list attached to this word: **part of the word**, and the
        // reason `fn Vec<i32> f()` does not read `i32` as the name.
        if (parser.nth(tokens) == lex::TokenKind::Less) {
          // The one question the cast position has: is this `<` a list, or the
          // comparison an expression around the cast wrote? A declaration has
          // nothing to ask -- the run ends at the parameter list.
          if (position == RunKind::Cast && !typeArgListIsReal(parser, tokens)) {
            break;
          }
          tokens = scanTypeArgList(parser, tokens).next;
        }
        if (position == RunKind::Cast) {
          // The type is complete. Anything after it -- `*`, `<`, `(`, a word --
          // belongs to the expression the cast is part of, which is the whole of
          // why the reader is handed a bound instead of the rest of the line.
          break;
        }
        continue;
      }
      ++tokens;
      continue;
    }
    // A `(T, U)` product, walked as one balanced group: **one word**, whose
    // closing `)` is found by counting. Nothing inside is judged here -- the type
    // reader one stage down is the only place that decides what a member may be,
    // and a scan that tried to would be a second copy of that rule
    // (`tuples.md`, decision 15).
    //
    // Two conditions, and both are about `fn`, which is the only thing this scan
    // is for. A `(` continues the run **only at its start** (`run.words == 0`),
    // because after a word the `(` is the *parameter list*: `fn i32 main(` has its
    // type and its name behind it, and reading the list as a product would make
    // `main` the return type. And the group must be non-empty, so `fn *(` is still
    // the missing name it was before a product existed rather than an empty
    // group that reads as a type.
    if (kind == lex::TokenKind::LParen && run.words == 0 &&
        parser.nth(tokens + 1) != lex::TokenKind::RParen) {
      std::uint32_t depth = 0;
      while (true) {
        const lex::TokenKind in = parser.nth(tokens);
        // Past the end, `nth` answers the end-of-file token (the source clamps),
        // so an unterminated group ends the run here and the group's own reader
        // reports the missing `)`.
        if (in == lex::TokenKind::EndOfFile) {
          break;
        }
        ++tokens;
        if (in == lex::TokenKind::LParen) {
          ++depth;
          continue;
        }
        if (in == lex::TokenKind::RParen) {
          --depth;
          if (depth == 0) {
            break;
          }
        }
      }
      run.words += 1;
      // The group is one word and this is the token after it. When nothing
      // follows, `lastWordIsGroup` is true and the reader reports the missing
      // name instead of reading this index.
      run.nameStart = tokens;
      run.lastWordIsGroup = true;
      if (position == RunKind::Cast) {
        break;
      }
      continue;
    }
    // `[N]`, `[]`, or the bracket alone.
    if (parser.nth(tokens) == lex::TokenKind::LBracket) {
      const lex::TokenKind counted = parser.nth(tokens + 1);
      if ((counted == lex::TokenKind::IntegerLiteral ||
           (counted == lex::TokenKind::Identifier && parser.text(tokens + 1) == kInferredCount)) &&
          parser.nth(tokens + 2) == lex::TokenKind::RBracket) {
        tokens += 3;
        continue;
      }
      // `[]T`, the slice: **two** tokens, and a complete type like the counted
      // group above. The run is what a `Type` node holds, so a run that stopped
      // at the `[` would put the `]` outside it -- and in a *declaration* that is
      // not a cosmetic difference: the name is the last identifier of the run, so
      // `fn []i32 f()` would split `]` as the name and report a function called
      // `]`. One token pair here is the whole fix, and it is the same statement
      // the type reader makes one stage down: `[]` is a type.
      if (counted == lex::TokenKind::RBracket) {
        tokens += 2;
        continue;
      }
      ++tokens;
      continue;
    }
    break;
  }
  run.tokens = tokens;
  return run;
}

} // namespace minc::parse
