// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "ast/lower.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ast/node.h"
#include "lex/token.h"
#include "parse/syntax_kind.h"
#include "support/intern/sym_id.h"
#include "support/limits.h"
#include "support/span/file_id.h"
#include "support/span/span.h"
#include "syntax/node.h"

namespace minc::ast {

support::Span OriginTable::originOf(support::FileId unit, std::uint32_t begin,
                                    std::uint32_t end) const {
  // No stream: the unit text is its own source. This is the honest answer for a
  // file that includes nothing, and it is also the fallback when a caller
  // (a test, a synthetic tree) has no preprocessed stream to offer.
  if (stream == nullptr) {
    return support::Span(unit, begin, end);
  }
  const std::vector<lex::Token>& tokens = stream->tokens();
  if (tokens.empty()) {
    return support::Span(unit, begin, end);
  }

  // The tokens tile the text, so the token containing an offset is found by
  // binary search on the starts. `first` is the token covering `begin`, `last`
  // the one covering `end - 1` (a zero-width node keeps `first`).
  const auto lower = [](std::uint32_t offset, const lex::Token& token) {
    return offset < token.offset;
  };
  const auto firstIt = std::upper_bound(tokens.begin(), tokens.end(), begin, lower);
  const std::size_t first =
      firstIt == tokens.begin() ? 0 : static_cast<std::size_t>(firstIt - tokens.begin()) - 1;

  std::size_t last = first;
  if (end > begin) {
    const auto lastIt = std::upper_bound(tokens.begin(), tokens.end(), end - 1, lower);
    last = lastIt == tokens.begin() ? 0 : static_cast<std::size_t>(lastIt - tokens.begin()) - 1;
  }

  const support::Span firstSpan = stream->spanOfAt(first);
  const support::Span lastSpan = stream->spanOfAt(last);
  // A synthesized token -- the separator space the preprocessor inserts so two
  // tokens do not run together -- was written nowhere, so its origin is invalid.
  // Falling back to the unit range keeps the span usable instead of carrying
  // "file 0xFFFFFFFF" into a diagnostic.
  if (firstSpan.file == support::kInvalidFile) {
    return support::Span(unit, begin, end);
  }
  // A range that crosses a file boundary (a macro argument written in a header,
  // its invocation in the main file) has no single written span; the first one is
  // returned, because a caret has to point somewhere and the start is where the
  // reader looks. The unit span is still exact for anything that needs the bytes.
  if (firstSpan.file != lastSpan.file) {
    return firstSpan;
  }
  return support::Span(firstSpan.file, firstSpan.begin, lastSpan.end);
}

namespace {

constexpr parse::SyntaxKind kIdentifierKind = parse::toSyntaxKind(lex::TokenKind::Identifier);
constexpr parse::SyntaxKind kStaticKind = parse::toSyntaxKind(lex::TokenKind::KwStatic);

// FNV-1a, one word at a time. Only ever a filter: `ItemTree::operator==` is what
// decides, so a collision costs a comparison and never a wrong answer.
[[nodiscard]] std::uint64_t mix(std::uint64_t hash, std::uint32_t value) {
  hash ^= value;
  hash *= 1099511628211ULL;
  return hash;
}

[[nodiscard]] std::uint64_t mixSpan(std::uint64_t hash, const support::Span& span) {
  hash = mix(hash, span.file);
  hash = mix(hash, span.begin);
  hash = mix(hash, span.end);
  return hash;
}

// The fields `Item::operator==` compares, and only those: the hash has to answer
// the same question equality does, or the filter and the verdict disagree.
[[nodiscard]] std::uint64_t hashItem(std::uint64_t hash, const Item& item) {
  hash = mix(hash, static_cast<std::uint32_t>(item.kind));
  hash = mix(hash, item.name);
  hash = mixSpan(hash, item.span);
  hash = mixSpan(hash, item.nameSpan);
  hash = mix(hash, item.paramCount);
  hash = mix(hash, item.variadic ? 1u : 0u);
  hash = mix(hash, item.hasBody ? 1u : 0u);
  hash = mix(hash, item.isStatic ? 1u : 0u);
  return hash;
}

// One lowering pass. A class rather than a set of free functions because the
// state (the node array, the flat child array, the open accumulators, the items)
// is the whole of it, and it must not be reachable from outside.
class Lowerer {
public:
  Lowerer(const syntax::SyntaxTree& tree, support::Interner& symbols, OriginTable origins,
          LowerLimits limits)
      : tree_(tree), symbols_(symbols), origins_(origins), limits_(limits) {
    // The parser's depth bound is the reason recursion is safe here. Asserting it
    // at the entry point means a future producer of green trees cannot quietly
    // remove the guarantee and turn lowering into a stack overflow.
    if (tree_.stats().maxDepth > support::kMaxNestingDepth) {
      tooDeep_ = true;
    }
  }

  [[nodiscard]] LowerOutput run() {
    if (tooDeep_) {
      errors_.push_back(AstError{support::Span(tree_.file(), 0, 0),
                                 "syntax tree is deeper than the parser's bound",
                                 AstErrorCode::NodeLimit});
      return finish();
    }

    const AstId root = lowerNode(tree_.root(), /*inError=*/false);
    if (root.valid()) {
      collectItems(root);
    }
    return finish();
  }

private:
  [[nodiscard]] LowerOutput finish() {
    ItemTree items;
    items.items = std::move(items_);
    for (const Item& item : items.items) {
      items.hash = hashItem(items.hash, item);
    }
    LoweredFile file = LoweredFile::make(tree_.file(), tree_.revision(), tree_.text(),
                                         std::move(nodes_), std::move(children_), std::move(items));
    return LowerOutput{std::move(file), std::move(errors_)};
  }

  [[nodiscard]] support::SymId intern(std::string_view text) {
    return symbols_.intern(text);
  }

  // Allocates one node. Returns an invalid id at the node limit, after reporting
  // it once; every caller checks, so the failure is a diagnostic and not a
  // half-built tree.
  [[nodiscard]] AstId pushNode(parse::SyntaxKind kind, std::uint32_t offset, std::uint32_t width,
                               bool inError, support::SymId name) {
    if (nodes_.size() >= limits_.maxNodes) {
      if (!overLimit_) {
        overLimit_ = true;
        errors_.push_back(AstError{origins_.originOf(tree_.file(), offset, offset + width),
                                   "too many nodes to lower", AstErrorCode::NodeLimit});
      }
      return AstId{};
    }
    Node node;
    node.kind = kind;
    node.origin = origins_.originOf(tree_.file(), offset, offset + width);
    node.unit = support::Span(tree_.file(), offset, offset + width);
    node.name = name;
    node.inError = inError;
    nodes_.push_back(node);
    return AstId{static_cast<std::uint32_t>(nodes_.size() - 1)};
  }

  // The interned name a node carries, or `kInvalidSym`. `Name` and `PathExpr`
  // are the two nodes that *mean* one identifier; a `Type` is a run and is
  // deliberately left nameless.
  [[nodiscard]] support::SymId nameOf(const syntax::SyntaxNode& node) const {
    if (kindOf(node) != parse::SyntaxKind::Name && kindOf(node) != parse::SyntaxKind::PathExpr) {
      return support::kInvalidSym;
    }
    const std::size_t count = node.childCount();
    for (std::size_t i = 0; i < count; ++i) {
      const syntax::NodeOrToken child = node.child(i);
      if (child.isToken() && child.asToken().kind() == kIdentifierKind) {
        return symbols_.intern(child.asToken().text());
      }
    }
    return support::kInvalidSym;
  }

  [[nodiscard]] static parse::SyntaxKind kindOf(const syntax::SyntaxNode& node) {
    return node.kind();
  }

  [[nodiscard]] AstId lowerToken(const syntax::SyntaxToken& token, bool inError) {
    if (overLimit_) {
      return AstId{};
    }
    support::SymId name = support::kInvalidSym;
    if (token.kind() == kIdentifierKind) {
      name = intern(token.text());
    }
    return pushNode(token.kind(), token.offset, token.width(), inError, name);
  }

  [[nodiscard]] AstId lowerNode(const syntax::SyntaxNode& node, bool inError) {
    if (overLimit_) {
      return AstId{};
    }
    constexpr parse::SyntaxKind kError = parse::SyntaxKind::Error;
    const bool error = inError || node.kind() == kError;
    const AstId id = pushNode(node.kind(), node.offset(), node.width(), error, nameOf(node));
    if (!id.valid()) {
      return id;
    }

    // One accumulator per open node. Trivia is dropped here and nowhere else, so
    // the walk below is the single place that decides what "the tree" means.
    open_.emplace_back();
    const std::size_t count = node.childCount();
    for (std::size_t i = 0; i < count && !overLimit_; ++i) {
      const syntax::NodeOrToken child = node.child(i);
      const AstId childId =
          child.isToken()
              ? (child.asToken().isTrivia() ? AstId{} : lowerToken(child.asToken(), error))
              : lowerNode(child.asNode(), error);
      if (childId.valid()) {
        open_.back().push_back(childId);
      }
    }

    std::vector<AstId> kids = std::move(open_.back());
    open_.pop_back();
    Node& self = nodes_[id.index];
    self.firstChild = static_cast<std::uint32_t>(children_.size());
    self.childCount = static_cast<std::uint32_t>(kids.size());
    children_.insert(children_.end(), kids.begin(), kids.end());
    // A green node's width includes the trivia the builder hung inside it, so it
    // is the *wrong* range for a node from which trash has just been dropped:
    // a `LiteralExpr` with a leading space would be three bytes wide around a
    // one-byte token, and that range maps to the space's origin, which is no
    // origin at all. The span is therefore recomputed from the children that
    // survived.
    tightenSpan(id, kids);
    return id;
  }

  // The exact extent of a node after trivia was dropped, and the written span of
  // that extent. A node with no surviving child keeps a zero-width span where it
  // would have been, which is what a missing token is.
  void tightenSpan(AstId id, const std::vector<AstId>& kids) {
    Node& self = nodes_[id.index];
    if (kids.empty()) {
      self.unit = support::Span(self.unit.file, self.unit.begin, self.unit.begin);
      self.origin = origins_.originOf(tree_.file(), self.unit.begin, self.unit.end);
      return;
    }
    std::uint32_t begin = nodes_[kids[0].index].unit.begin;
    std::uint32_t end = nodes_[kids[0].index].unit.end;
    for (const AstId kid : kids) {
      const support::Span& span = nodes_[kid.index].unit;
      begin = std::min(begin, span.begin);
      end = std::max(end, span.end);
    }
    self.unit = support::Span(self.unit.file, begin, end);
    self.origin = origins_.originOf(tree_.file(), self.unit.begin, self.unit.end);
  }

  // The three node kinds a file-scope item can be. One predicate rather than the
  // same three comparisons in the definition below, so a form added to the
  // grammar is added in one place -- and `resolve` reads the *kind* off the
  // summary, so the list here and the `DefKind` it will produce are read from
  // the same node kind and cannot drift.
  [[nodiscard]] static bool isItemNode(parse::SyntaxKind kind) {
    return kind == parse::SyntaxKind::FnDecl || kind == parse::SyntaxKind::LetStmt ||
           kind == parse::SyntaxKind::ConstStmt || kind == parse::SyntaxKind::TypeAliasDecl;
  }

  void collectItems(AstId root) {
    for (const AstId child : fileChildren(root)) {
      const Node& node = nodes_[child.index];
      if (isItemNode(node.kind) && !node.inError) {
        collectItem(child);
      }
    }
  }

  [[nodiscard]] std::span<const AstId> fileChildren(AstId root) const {
    const Node& node = nodes_[root.index];
    return std::span<const AstId>(children_.data() + node.firstChild, node.childCount);
  }

  void collectItem(AstId decl) {
    const Node& node = nodes_[decl.index];
    Item item;
    item.kind = node.kind;
    item.node = decl.index;
    item.isStatic = childOfKind(decl, kStaticKind).valid();

    const AstId name = childOfKind(decl, parse::SyntaxKind::Name);
    if (name.valid()) {
      item.name = nodes_[name.index].name;
      item.nameSpan = nodes_[name.index].origin;
    }

    // The body, which is what the signature stops short of. One rule for both
    // kinds of item: a function's body is its block, a binding's body is its
    // initializer, and an item with neither is a declaration whose definition is
    // elsewhere. The signature therefore ends where the body begins -- which is
    // what makes it stable under an edit inside that body.
    AstId body;
    if (node.kind == parse::SyntaxKind::FnDecl) {
      const AstId params = childOfKind(decl, parse::SyntaxKind::ParamList);
      if (params.valid()) {
        item.paramCount = countParams(params);
        item.variadic = childOfKind(params, parse::SyntaxKind::VariadicParam).valid();
      }
      body = childOfKind(decl, parse::SyntaxKind::Block);
    } else if (node.kind == parse::SyntaxKind::TypeAliasDecl) {
      // What a type name's signature stops short of is the type it names: the
      // declaration's first half is `type Name =`, and editing the type it points
      // at is what an editor's "did this declaration's interface change?"
      // question is about -- the same question a binding's initializer and a
      // function's body answer for the other two forms.
      body = childOfKind(decl, parse::SyntaxKind::Type);
    } else {
      body = bindingInitializer(decl);
    }
    item.hasBody = body.valid();
    item.body = body.valid() ? body.index : kInvalidAst;

    const std::uint32_t end = body.valid() ? nodes_[body.index].origin.begin : node.origin.end;
    item.span = support::Span(node.origin.file, node.origin.begin, end);
    items_.push_back(item);
  }

  // The initializer of a binding: the interior child that is neither the name nor
  // the annotation. The same rule `validate` and the checker use to find it, so
  // "which child is the value" is answered one way by all three readers.
  [[nodiscard]] AstId bindingInitializer(AstId stmt) const {
    const Node& node = nodes_[stmt.index];
    for (std::uint32_t i = 0; i < node.childCount; ++i) {
      const AstId child = children_[node.firstChild + i];
      const Node& kid = nodes_[child.index];
      if (kid.isToken() || kid.kind == parse::SyntaxKind::Name ||
          kid.kind == parse::SyntaxKind::Type) {
        continue;
      }
      return child;
    }
    return AstId{};
  }

  [[nodiscard]] std::uint32_t countParams(AstId list) const {
    const Node& node = nodes_[list.index];
    std::uint32_t count = 0;
    for (std::uint32_t i = 0; i < node.childCount; ++i) {
      if (nodes_[children_[node.firstChild + i].index].kind == parse::SyntaxKind::Param) {
        ++count;
      }
    }
    return count;
  }

  [[nodiscard]] AstId childOfKind(AstId parent, parse::SyntaxKind kind) const {
    const Node& node = nodes_[parent.index];
    for (std::uint32_t i = 0; i < node.childCount; ++i) {
      const AstId child = children_[node.firstChild + i];
      if (nodes_[child.index].kind == kind) {
        return child;
      }
    }
    return AstId{};
  }

  const syntax::SyntaxTree& tree_;
  support::Interner& symbols_;
  OriginTable origins_;
  LowerLimits limits_;
  std::vector<Node> nodes_;
  std::vector<AstId> children_;
  std::vector<std::vector<AstId>> open_;
  std::vector<Item> items_;
  std::vector<AstError> errors_;
  bool overLimit_ = false;
  bool tooDeep_ = false;
};

} // namespace

LowerOutput lowerFile(const syntax::SyntaxTree& tree, support::Interner& symbols,
                      const OriginTable& origins, LowerLimits limits) {
  Lowerer lowerer(tree, symbols, origins, limits);
  return lowerer.run();
}

} // namespace minc::ast
