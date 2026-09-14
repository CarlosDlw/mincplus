// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The typed view over the untyped tree.
//
// These are thin wrappers -- a `SyntaxNode` plus checked accessors -- and every
// field is optional on purpose. A half-written function has a name and no body,
// and the AST must be able to say so; a typed tree with non-optional fields
// could not represent the code an editor sees most of the time.
//
// The set below covers the syntax that is decided. The rest are the same shape:
// `cast` checks the kind, each accessor looks up a child by kind. When the node
// count justifies it, this layer is generated from one grammar description (see
// `docs/architectures/parser.md`, decision 16) rather than hand-extended.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "syntax/node.h"

namespace minc::syntax {

// Base for the single-kind wrappers: the checked cast, the kind, and access to
// the raw tree. This is CRTP rather than a macro so the compiler checks every
// member's spelling, the debugger can see through it, and a reflow cannot cut a
// line continuation in half.
template <typename Derived, parse::SyntaxKind Kind> class AstNodeBase {
public:
  // Returns nullopt without touching the tree when the kind does not match --
  // that check is the whole point of having a typed layer at all.
  [[nodiscard]] static std::optional<Derived> cast(SyntaxNode node) {
    if (node.kind() != Kind) {
      return std::nullopt;
    }
    return Derived(node);
  }

  [[nodiscard]] parse::SyntaxKind kind() const {
    return Kind;
  }
  [[nodiscard]] const SyntaxNode& syntax() const {
    return node_;
  }
  [[nodiscard]] bool valid() const {
    return node_.valid();
  }

private:
  // Only the wrapper this base exists for can build one, so the base cannot be
  // instantiated as an ordinary template class. The derived classes still get
  // their own public constructors, which is where the empty-invalid state is
  // defined.
  friend Derived;

  AstNodeBase() = default;
  explicit AstNodeBase(SyntaxNode node) : node_(node) {}

  SyntaxNode node_;
};

// `fn Type Name() Block`, and its declaration form `extern fn Type Name();`.
//
// One wrapper for both, because they are one declaration with one difference:
// `extern` says the definition is elsewhere, and a declaration therefore has no
// body. A caller asks `bodyOf` and reads an empty answer as `isExtern`, which is
// the pair the grammar guarantees -- a body implies no `extern`, and the reverse.
class FnDecl : public AstNodeBase<FnDecl, parse::SyntaxKind::FnDecl> {
public:
  FnDecl() = default;
  explicit FnDecl(SyntaxNode node) : AstNodeBase(node) {}

  // True when the declaration begins with `extern`, and so has no body.
  [[nodiscard]] bool isExtern() const;
};

class Block : public AstNodeBase<Block, parse::SyntaxKind::Block> {
public:
  Block() = default;
  explicit Block(SyntaxNode node) : AstNodeBase(node) {}
};

// Expressions.
class BinaryExpr : public AstNodeBase<BinaryExpr, parse::SyntaxKind::BinaryExpr> {
public:
  BinaryExpr() = default;
  explicit BinaryExpr(SyntaxNode node) : AstNodeBase(node) {}
};

class CallExpr : public AstNodeBase<CallExpr, parse::SyntaxKind::CallExpr> {
public:
  CallExpr() = default;
  explicit CallExpr(SyntaxNode node) : AstNodeBase(node) {}
};

// `let` and `const`, which share a shape and differ in one bit: a `const` may
// not be reassigned. One wrapper for both, because a caller looking at a
// declaration should not have to try two casts to find it.
class VariableStmt {
public:
  VariableStmt() = default;
  explicit VariableStmt(SyntaxNode node) : node_(node) {}

  [[nodiscard]] static std::optional<VariableStmt> cast(SyntaxNode node);
  [[nodiscard]] bool isConst() const;
  [[nodiscard]] const SyntaxNode& syntax() const {
    return node_;
  }
  [[nodiscard]] bool valid() const {
    return node_.valid();
  }

private:
  SyntaxNode node_;
};

// Return type, name, parameter list, and body of a function. All optional:
// while a function is being typed, most of them are absent.
[[nodiscard]] std::optional<SyntaxNode> returnTypeOf(const FnDecl& decl);
[[nodiscard]] std::optional<SyntaxNode> nameOf(const FnDecl& decl);
[[nodiscard]] std::optional<SyntaxNode> parameterListOf(const FnDecl& decl);
[[nodiscard]] std::optional<SyntaxNode> bodyOf(const FnDecl& decl);

// Name, annotation, and initializer of a `let`/`const`.
[[nodiscard]] std::optional<SyntaxNode> variableName(const VariableStmt& stmt);
[[nodiscard]] std::optional<SyntaxNode> variableType(const VariableStmt& stmt);
[[nodiscard]] std::optional<SyntaxNode> variableInitializer(const VariableStmt& stmt);

// Operands and operator of a binary expression. The operator is a leaf, so it
// comes back as a token.
[[nodiscard]] std::optional<SyntaxNode> leftOperand(const BinaryExpr& expr);
[[nodiscard]] std::optional<SyntaxNode> rightOperand(const BinaryExpr& expr);
[[nodiscard]] std::optional<SyntaxToken> binaryOperator(const BinaryExpr& expr);

// Callee and argument list of a call. The argument list is absent for `f()`.
[[nodiscard]] std::optional<SyntaxNode> callee(const CallExpr& expr);
[[nodiscard]] std::optional<SyntaxNode> argumentList(const CallExpr& expr);

// True for the kinds that can be an expression. The parser's recovery wraps
// junk in an `Error` node, so that counts too.
[[nodiscard]] bool isExpressionKind(parse::SyntaxKind kind);

// Text of the first identifier leaf under `node` -- the spelling of a `Name`, a
// `Type`, or a `PathExpr`.
[[nodiscard]] std::string_view identifierText(const SyntaxNode& node);

// Every leaf's text under `node`, appended to `out` in source order.
void appendText(const SyntaxNode& node, std::string& out);

} // namespace minc::syntax
