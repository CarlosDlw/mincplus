// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "syntax/ast.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace minc::syntax {
namespace {

// The n-th interior child, or nullopt. Operands of a binary expression and the
// callee of a call are positional, so index access is the honest way to ask.
[[nodiscard]] std::optional<SyntaxNode> nodeChildAt(const SyntaxNode& node, std::size_t index) {
  const std::vector<SyntaxNode> children = node.nodeChildren();
  if (index >= children.size()) {
    return std::nullopt;
  }
  return children[index];
}

// The first non-trivia leaf child, whatever its kind. A binary expression's
// only non-trivia token child is its operator, so this is "the operator"
// without a kind list -- and skipping trivia here is required, because trivia
// *is* in the tree: whitespace between the operands is a token child too.
[[nodiscard]] std::optional<SyntaxToken> firstTokenChild(const SyntaxNode& node) {
  for (std::size_t i = 0; i < node.childCount(); ++i) {
    const NodeOrToken child = node.child(i);
    if (child.isToken() && !child.asToken().isTrivia()) {
      return child.asToken();
    }
  }
  return std::nullopt;
}

} // namespace

bool isExpressionKind(parse::SyntaxKind kind) {
  switch (kind) {
  case parse::SyntaxKind::LiteralExpr:
  case parse::SyntaxKind::PathExpr:
  case parse::SyntaxKind::ParenExpr:
  case parse::SyntaxKind::PrefixExpr:
  case parse::SyntaxKind::PostfixExpr:
  case parse::SyntaxKind::BinaryExpr:
  case parse::SyntaxKind::ConditionalExpr:
  case parse::SyntaxKind::AssignExpr:
  case parse::SyntaxKind::CallExpr:
  // A recovered expression is still in expression position, so a caller looking
  // for "the initializer" finds the error node rather than nothing.
  case parse::SyntaxKind::Error:
    return true;
  default:
    return false;
  }
}
std::optional<VariableStmt> VariableStmt::cast(SyntaxNode node) {
  if (node.kind() != parse::SyntaxKind::LetStmt && node.kind() != parse::SyntaxKind::ConstStmt) {
    return std::nullopt;
  }
  return VariableStmt(node);
}

bool VariableStmt::isConst() const {
  return node_.kind() == parse::SyntaxKind::ConstStmt;
}

std::optional<SyntaxNode> returnTypeOf(const FnDecl& decl) {
  return decl.syntax().childOfKind(parse::SyntaxKind::Type);
}

std::optional<SyntaxNode> nameOf(const FnDecl& decl) {
  return decl.syntax().childOfKind(parse::SyntaxKind::Name);
}

std::optional<SyntaxNode> parameterListOf(const FnDecl& decl) {
  return decl.syntax().childOfKind(parse::SyntaxKind::ParamList);
}

std::optional<SyntaxNode> bodyOf(const FnDecl& decl) {
  return decl.syntax().childOfKind(parse::SyntaxKind::Block);
}

std::optional<SyntaxNode> variableName(const VariableStmt& stmt) {
  return stmt.syntax().childOfKind(parse::SyntaxKind::Name);
}

std::optional<SyntaxNode> variableType(const VariableStmt& stmt) {
  return stmt.syntax().childOfKind(parse::SyntaxKind::Type);
}

std::optional<SyntaxNode> variableInitializer(const VariableStmt& stmt) {
  // The initializer is whatever expression follows `=`; by grammar a `let` has
  // at most one direct expression child, so the first one is it.
  for (std::size_t i = 0; i < stmt.syntax().childCount(); ++i) {
    const NodeOrToken child = stmt.syntax().child(i);
    if (!child.isToken() && isExpressionKind(child.asNode().kind())) {
      return child.asNode();
    }
  }
  return std::nullopt;
}

std::optional<SyntaxNode> leftOperand(const BinaryExpr& expr) {
  return nodeChildAt(expr.syntax(), 0);
}

std::optional<SyntaxNode> rightOperand(const BinaryExpr& expr) {
  return nodeChildAt(expr.syntax(), 1);
}

std::optional<SyntaxToken> binaryOperator(const BinaryExpr& expr) {
  return firstTokenChild(expr.syntax());
}

std::optional<SyntaxNode> callee(const CallExpr& expr) {
  return nodeChildAt(expr.syntax(), 0);
}

std::optional<SyntaxNode> argumentList(const CallExpr& expr) {
  return expr.syntax().childOfKind(parse::SyntaxKind::ArgList);
}

std::string_view identifierText(const SyntaxNode& node) {
  if (const std::optional<SyntaxToken> token =
          node.tokenOfKind(parse::toSyntaxKind(lex::TokenKind::Identifier))) {
    return token->text();
  }
  return {};
}

void appendText(const SyntaxNode& node, std::string& out) {
  // Iterative for the same reason the rest of the tree walks are: a subtree can
  // be as deep as the parser's guard allows.
  struct Frame {
    const GreenNode* node;
    std::size_t next;
  };
  std::vector<Frame> stack;
  if (node.green() != nullptr) {
    stack.push_back({node.green(), 0});
  }
  while (!stack.empty()) {
    Frame& frame = stack.back();
    if (frame.next >= frame.node->children.size()) {
      stack.pop_back();
      continue;
    }
    const GreenChild& child = frame.node->children[frame.next];
    ++frame.next;
    if (child.isNode) {
      stack.push_back({child.node, 0});
    } else {
      out.append(child.token->text);
    }
  }
}

} // namespace minc::syntax
