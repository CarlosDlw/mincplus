// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "parse/syntax_kind.h"

#include <array>
#include <cstddef>
#include <utility>

namespace minc::parse {
namespace {

// The node kinds, in one table. Adding a node kind is a row here plus an
// enumerator in syntax_kind.h; the dump, the tests, and the derived
// `allNodeKinds()` all read this instead of repeating the list.
//
// The size is **deduced**, and that is not cosmetic: it used to be written out
// (`std::array<NodeKindInfo, 46>`), so adding a kind was a row here, an enumerator
// there, *and* an edit to a number nobody remembers -- and forgetting the third
// failed the build in a file the reader was not editing. `std::to_array` makes the
// table the only thing that decides how big it is.
struct NodeKindInfo {
  SyntaxKind kind;
  const char* name;
};

constexpr auto kNodeKindInfos = std::to_array<NodeKindInfo>({
    {SyntaxKind::File, "File"},
    {SyntaxKind::Error, "Error"},
    {SyntaxKind::FnDecl, "FnDecl"},
    // The row `type Name = T;` was missing from this table, which made
    // `toString(TypeAliasDecl)` answer "Unknown" in every dump that named one.
    // Found while adding the rows below; the test that checks every node kind has
    // a row did not exist either, and now does.
    {SyntaxKind::TypeAliasDecl, "TypeAliasDecl"},
    {SyntaxKind::ParamList, "ParamList"},
    {SyntaxKind::Param, "Param"},
    {SyntaxKind::VariadicParam, "VariadicParam"},
    {SyntaxKind::Block, "Block"},
    {SyntaxKind::LetStmt, "LetStmt"},
    {SyntaxKind::ConstStmt, "ConstStmt"},
    {SyntaxKind::ReturnStmt, "ReturnStmt"},
    {SyntaxKind::ExprStmt, "ExprStmt"},
    {SyntaxKind::EmptyStmt, "EmptyStmt"},
    {SyntaxKind::IfStmt, "IfStmt"},
    {SyntaxKind::ElseClause, "ElseClause"},
    {SyntaxKind::WhileStmt, "WhileStmt"},
    {SyntaxKind::ForStmt, "ForStmt"},
    {SyntaxKind::ForCondition, "ForCondition"},
    {SyntaxKind::ForStep, "ForStep"},
    {SyntaxKind::BreakStmt, "BreakStmt"},
    {SyntaxKind::ContinueStmt, "ContinueStmt"},
    {SyntaxKind::Name, "Name"},
    {SyntaxKind::Type, "Type"},
    {SyntaxKind::LiteralExpr, "LiteralExpr"},
    {SyntaxKind::PathExpr, "PathExpr"},
    {SyntaxKind::ParenExpr, "ParenExpr"},
    {SyntaxKind::CastExpr, "CastExpr"},
    {SyntaxKind::PrefixExpr, "PrefixExpr"},
    {SyntaxKind::PostfixExpr, "PostfixExpr"},
    {SyntaxKind::BinaryExpr, "BinaryExpr"},
    {SyntaxKind::ConditionalExpr, "ConditionalExpr"},
    {SyntaxKind::AssignExpr, "AssignExpr"},
    {SyntaxKind::CallExpr, "CallExpr"},
    {SyntaxKind::IndexExpr, "IndexExpr"},
    {SyntaxKind::SliceExpr, "SliceExpr"},
    {SyntaxKind::TupleExpr, "TupleExpr"},
    {SyntaxKind::FieldExpr, "FieldExpr"},
    {SyntaxKind::ArgList, "ArgList"},
    {SyntaxKind::ArrayLiteral, "ArrayLiteral"},
    {SyntaxKind::TypedInitializer, "TypedInitializer"},
    {SyntaxKind::TuplePattern, "TuplePattern"},
    {SyntaxKind::GenericParams, "GenericParams"},
    {SyntaxKind::Constraint, "Constraint"},
    {SyntaxKind::TypeArgList, "TypeArgList"},
    {SyntaxKind::MacroCall, "MacroCall"},
    {SyntaxKind::TokenTree, "TokenTree"},
    {SyntaxKind::Attribute, "Attribute"},
});

// Derived, not listed again: a kind added to the table above is picked up by
// every consumer of allNodeKinds() without a second edit that could be
// forgotten. The pack expansion initializes every slot from the table, so no
// element is ever value-initialized to a zero that is not a valid `SyntaxKind`.
template <std::size_t... Indexes>
[[nodiscard]] constexpr auto kindsFromTable(std::index_sequence<Indexes...>) {
  return std::array<SyntaxKind, sizeof...(Indexes)>{kNodeKindInfos[Indexes].kind...};
}

constexpr auto kAllNodeKinds = kindsFromTable(std::make_index_sequence<kNodeKindInfos.size()>{});

} // namespace

std::span<const SyntaxKind> allNodeKinds() {
  return kAllNodeKinds;
}

std::string_view toString(SyntaxKind kind) {
  // Leaves are named by the lexer, so there is one name per token kind and it
  // is the same string the token dump prints.
  if (isTokenKind(kind)) {
    return lex::toString(toTokenKind(kind));
  }
  for (const NodeKindInfo& info : kNodeKindInfos) {
    if (info.kind == kind) {
      return info.name;
    }
  }
  return "Unknown";
}

} // namespace minc::parse
