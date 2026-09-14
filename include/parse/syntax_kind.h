// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// One tag for every element that can appear in the syntax tree.
//
// Tokens and interior nodes live in the same tag space so generic tree code
// (a dump, a validator, a highlighter) never needs a special case for leaves.
// Token kinds occupy [0, kFirstNodeKind) and carry exactly their
// `lex::TokenKind` value, so `toSyntaxKind` is a cast; node kinds start at
// `kFirstNodeKind`.
//
// The node kinds are the *decided* syntax and nothing else. Kinds for features
// whose syntax is not fixed yet (macros, token trees, attributes) are reserved
// and listed here, so "we left room for that" is a fact in the code rather than
// a claim in a document.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "lex/token_kind.h"

namespace minc::parse {

// Token kinds are all below this; node kinds are all at or above it. It is 256
// rather than "just past the last token" so that adding token kinds never
// renumbers a node kind -- numbers that appear in tests and golden files.
inline constexpr std::uint16_t kFirstNodeKind = 256;

static_assert(static_cast<std::uint16_t>(lex::TokenKind::Last) < kFirstNodeKind,
              "every token kind must stay below kFirstNodeKind; raise kFirstNodeKind "
              "if the token set grew past 256");

enum class SyntaxKind : std::uint16_t {
  // A whole file. Always the root.
  File = kFirstNodeKind,
  // Anything the parser could not make sense of, and the node a recovery run
  // wraps skipped tokens in. Keeping the offending bytes under a node is what
  // lets a malformed file still produce a lossless tree.
  Error,

  // Declarations.
  FnDecl,
  ParamList,
  Param,
  // The `...` of a parameter list, as a node rather than as a loose token, so
  // "this list is variadic" is a *child* and not a flag a later stage has to
  // remember -- the same reason the `else` arm is an `ElseClause`. It is not a
  // `Param`: it has no name and no type, and the arity a signature has must not
  // count it.
  VariadicParam,
  Block,

  // Statements.
  LetStmt,
  ConstStmt,
  ReturnStmt,
  ExprStmt,
  EmptyStmt,
  // `IfStmt` is the whole `if`, condition and both arms. The `else` arm is an
  // `ElseClause` so that "there is an else" is a child and not a flag, and so
  // that `else if` is one nested `IfStmt` inside it rather than a special kind.
  IfStmt,
  ElseClause,
  WhileStmt,
  // `ForStmt` holds its three clauses as nodes, in source order, then the body
  // block. The condition and the step get their own kinds so that a reader asks
  // for them *by name*: three bare expressions in a row could only be told apart
  // by counting, and counting is what breaks when a clause is added. The init is
  // a statement -- possibly an empty one -- because that is what it is.
  ForStmt,
  ForCondition,
  ForStep,
  BreakStmt,
  ContinueStmt,

  // Names and types. A type is a *position*, not a token kind: `i32`, `long`,
  // and `unsigned long long int` are all `Type` nodes made of identifiers.
  Name,
  Type,

  // Expressions.
  LiteralExpr,
  PathExpr,
  ParenExpr,
  // `&x` and `*p` are `PrefixExpr`, like `-x`: an operator token and one
  // operand. A pointer is not a second kind of expression, only a second thing
  // the same shape can mean, which is what keeps this list from growing per
  // feature.
  PrefixExpr,
  PostfixExpr,
  BinaryExpr,
  ConditionalExpr,
  AssignExpr,
  CallExpr,
  // `a[i]`. Not a `PostfixExpr`: that node is an operator token and the
  // expression it applies to, while an index has three parts (the base, the
  // index, and the brackets that delimit it) and the index is a full
  // expression of its own. Children are `base`, `[`, `index`, `]`.
  IndexExpr,
  ArgList,

  // Reserved: names are fixed now, the syntax that produces them is not.
  MacroCall,
  TokenTree,
  Attribute,
};

// Leaves and interior nodes share the space; this is how generic code tells
// them apart.
[[nodiscard]] constexpr bool isTokenKind(SyntaxKind kind) {
  return static_cast<std::uint16_t>(kind) < kFirstNodeKind;
}

// The token range deliberately reuses the lexer's numeric values without
// repeating 60 of them as enumerators, so an integer conversion is the
// definition rather than a mistake. The range checker cannot express "this
// enum has values with no enumerator", hence the targeted suppression.
[[nodiscard]] constexpr SyntaxKind toSyntaxKind(lex::TokenKind kind) {
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  return static_cast<SyntaxKind>(static_cast<std::uint16_t>(kind));
}

// The inverse, valid only when isTokenKind(kind) is true.
[[nodiscard]] constexpr lex::TokenKind toTokenKind(SyntaxKind kind) {
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  return static_cast<lex::TokenKind>(static_cast<std::uint16_t>(kind));
}

// Stable name (`FnDecl`, `BinaryExpr`, and, for a leaf, the token kind's name).
// Never localized, never abbreviated.
[[nodiscard]] std::string_view toString(SyntaxKind kind);

// Every interior node kind, derived from the one table that also holds the
// names, so a kind added to the enum without a table row is caught by a test.
[[nodiscard]] std::span<const SyntaxKind> allNodeKinds();

} // namespace minc::parse
