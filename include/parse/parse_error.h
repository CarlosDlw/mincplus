// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// A syntax error, as a value.
//
// Errors are deliberately *not* part of the syntax tree. An `Error` node marks
// where the offending bytes are; this carries what was wrong with them. Keeping
// them apart means the tree stays a pure value, the same tree can be compared
// byte for byte across runs, and a consumer that only wants structure never
// walks diagnostics.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "support/span/span.h"

namespace minc::parse {

// The closed set of things that can go wrong in the grammar.
//
// A code is an enumerator rather than a string literal at each call site for
// the same reason `TokenFlag` is: the parser cannot invent a code that no test
// and no documentation knows about, two sites cannot spell the same condition
// two ways, and `allParseErrorCodes()` can be checked against the inputs that
// produce them, so a code with no trigger is caught instead of shipped dead.
enum class ParseErrorCode : std::uint8_t {
  // A required token was absent; `expect()` inserts a zero-width one.
  ExpectedToken,
  // Something where a declaration (today: a function) was required.
  ExpectedItem,
  // A name slot with nothing name-shaped in it.
  ExpectedName,
  // A type position with nothing type-shaped in it.
  ExpectedType,
  // An expression position with nothing expression-shaped in it.
  ExpectedExpression,
  // A token that cannot start a statement.
  ExpectedStatement,
  // A function declaration with neither a body nor `extern`. The two spellings
  // are exactly what tells a definition from a declaration, so one of them has
  // to be there; the message names the one to type.
  MissingExtern,
  // `extern fn` with a body. `extern` says the definition lives in another unit,
  // so a body here contradicts the word rather than being an extra detail.
  ExternWithBody,
  // `extern` written before a binding (`extern let x: i32;`). The *form* is not
  // built yet -- a global has storage, an initializer, and an address the loader
  // initializes rather than this unit -- so the word is refused by name instead
  // of being misread as a function declaration that lost its `fn`.
  ExternBinding,
  // `static` written together with `extern`. The two words answer one question
  // -- who may see this name -- with opposite answers, so both cannot be right.
  ConflictingLinkage,
  // `static` somewhere other than in front of a declaration. It is a declaration
  // word, not a statement one: a function-local `static` is a different feature.
  StaticPosition,
  // `static` or `extern` in front of a `type` declaration. Both words are about
  // *symbols* -- one makes a definition internal, the other says the definition
  // is elsewhere -- and a name for a type produces neither: it never reaches a
  // linker, so there is nothing for either word to say (`type_alias.md`).
  TypeAliasLinkage,
  // There is deliberately no code for `type` inside a block: the declaration is
  // the same production in both positions, and *where* its name is visible is a
  // scope rule the resolver and checker own (`type_alias.md`, decision 5).
  // `...` in a function that has a body. *Reading* a variadic argument needs
  // `va_start`, which the language does not have, so only a declaration may be
  // variadic -- the marker in a definition would be a function nobody can write.
  VariadicDefinition,
  // `...` somewhere other than the end of a parameter list, or with no parameter
  // before it. The marker ends the list, so a list with anything after it has no
  // meaning to give the arguments that follow.
  VariadicPosition,
  // `[` in a type position with no count inside it, or with something that is not
  // a literal number. The count is how many elements there are, and the shape
  // that decides it is the one the whole type's identity rests on.
  //
  // `[]` -- the two brackets with *nothing* between them -- is deliberately not
  // this code: it is the spelling of a slice, a *complete* type, so it parses and
  // is interned like any other (`slices.md`).
  ExpectedArrayCount,
  // `[N` with the closing bracket missing. Reported at the point the bracket
  // should have been, and the group is closed anyway so one missing `]` does not
  // turn the rest of the declaration into a second diagnostic.
  ExpectedArrayCountClose,
  // A `{` with no type in front of it: `let a: [3]i32 = {1, 2, 3};`, and the same
  // mistake nested one level down, `[2][3]i16{{1, 2, 3}, ...}`, and at a fill's
  // value. The two groupings are not interchangeable and the sentence says which
  // is which: `{...}` comes *after* a type (`[3]i32{1, 2, 3}`), while a group of
  // elements is a value and is written with the same brackets its type is
  // (`[1, 2, 3]`). Without this code the reader who wrote C's braces gets
  // "expected an expression" at a token that starts an expression everywhere
  // else, which teaches nothing about the characters to change.
  BraceWithoutType,
  // `(T, U` with the closing `)` missing -- the product's peer of
  // `ExpectedArrayCountClose`, and the sentence a reader who left a group open
  // needs: it names the character to type and the group it closes.
  ExpectedTypeGroupClose,
  // A number written without its leading digit: `.5`, and the same in an exponent
  // position. **A decimal literal begins with a digit** (`tuples.md`), because a
  // `.` after a value reads a member of it (`t.0`) and the two cannot both be
  // true of the same character. The sentence is the whole fix: `0.5`.
  LeadingPointNumber,
  // A `(T, U)` group followed straight by an expression: `(i32, bool)x`, which is
  // a cast to a *product* -- two parenthesised forms that look alike and mean
  // different things. Refused by name rather than left to the next token's
  // "expected `;`", which teaches nothing about what to write instead.
  CastToProduct,
  // A literal written *against* an identifier: `10z`, `1.5u8x`, `'a'u8`.
  //
  // The scanner claims a trailing run only when the run is a suffix the language
  // knows (`suffix.h`), so an unknown run leaves the literal and the name as two
  // tokens -- and two tokens written with nothing between them are one mistake.
  // This is a *parse* finding and not a lexical one because telling `10z` from
  // `10 z` needs the spans, and because the sentence names the set of suffixes
  // the reader could have meant, which is the grammar's table.
  InvalidLiteralSuffix,
  // `<i32, bool` with the closing `>` missing -- the type argument list's peer
  // of `ExpectedTypeGroupClose`: the sentence a reader who left a list open
  // needs, naming the character to type and the list it closes.
  ExpectedTypeArgClose,
  // A `>` that closes nothing: `A<B>>` written where one `>` is enough, or the
  // `=` of a `>=`/`>>=` where no declaration has an `=` to give.
  //
  // Named rather than left to the next token's "expected `;`", because the
  // *character* is the mistake and the fix is which one to delete -- or, for
  // `>=`, that `>` and `=` are two characters (`generics.md`, decision 4).
  StrayTypeArgClose,
  // A constraint on a binder -- `<T: Ordered>`. The slot is the `:` of every
  // other binding and it is reserved, but nothing reads it yet: an accepted
  // constraint that no stage enforces is a declaration that promises more than
  // the compiler checks, which is worse than a missing one. Refused by name, and
  // the tokens are consumed with it so one mistake stays one sentence.
  ConstraintNotRead,
  // The parser stopped: too many errors, or input nested past the guard.
  Aborted,
};

// Code and its stable short name (`parse-expected-token`). Keeping them in one
// row means the string a user greps for cannot drift from the enumerator, and
// searching a code in the source finds the enumerator that produced it.
struct ParseErrorCodeInfo {
  ParseErrorCode code;
  const char* name;
};

[[nodiscard]] std::span<const ParseErrorCodeInfo> parseErrorCodeInfos();

// Every code, derived from the table above rather than listed a second time, so
// a code added to the enum without a row is caught by the tests.
[[nodiscard]] std::span<const ParseErrorCode> allParseErrorCodes();

// The stable short name (`parse-expected-token`), never localized.
[[nodiscard]] std::string_view toString(ParseErrorCode code);

struct ParseError {
  // Byte range inside one file. A recovered "expected ';'" points at the spot
  // the token is missing from, so the caret lands where the user must type.
  support::Span span;
  std::string message;
  ParseErrorCode code = ParseErrorCode::Aborted;

  // The name to report, without a second copy of the code in this struct.
  [[nodiscard]] std::string_view codeName() const {
    return toString(code);
  }
};

} // namespace minc::parse
