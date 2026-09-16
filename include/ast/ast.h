// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// A lowered translation unit: the node array, plus the item tree inside it.
//
// It is a *value*: nothing is shared, nothing points outside, and the only
// externals are the views into the session's text that the spans describe. That
// is what lets `ItemTree` be compared and cached, and what makes the editor
// invariant possible -- editing a function body changes the node array and
// leaves the item tree identical.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "ast/node.h"
#include "support/intern/sym_id.h"
#include "support/span/file_id.h"
#include "support/span/span.h"

namespace minc::ast {

// One file-scope declaration, reduced to what a later stage needs to *know about
// it* without looking at it: how it is named, what kind of thing it is, where
// its signature is, and -- if it has one -- which node is its body.
//
// The signature is *spelled*: a name, an arity, whether a body exists, and which
// linkage word was written -- never a type. Types are `sema`'s, and an item that
// carried them would force this stage to decide something it has no business
// deciding.
//
// Two kinds of declaration share this one summary, and the split is the same for
// both: a function's signature is `[static] [extern] fn T Name(params)` and its
// body is the block; a binding's signature is `[static] let|const Name: T` and
// its body is the initializer. So `hasBody`/`body` mean "there is a definition
// here" in both cases, and an edit to a body -- a function's statements, a
// constant's value -- leaves the item tree identical. That is the invariant, not
// a convenience: the file scope is rebuilt exactly when an item tree changes.
//
// `operator==` is the load-bearing part, and it is narrower than the struct: the
// node indices (`node`, `body`) are deliberately **not** compared. They are
// artifacts of how many nodes happened to precede the item, so comparing them
// would make an edit inside one function's body look like a change to every
// signature after it -- exactly the invariant the item tree exists to provide.
struct Item {
  // `FnDecl`, `LetStmt` or `ConstStmt`: the node the declaration is, and so also
  // *which* kind of declaration this is.
  NodeKind kind = NodeKind::Error;
  support::SymId name = support::kInvalidSym;
  // The signature: from the start of the declaration through the closing `)` of
  // a parameter list, or through the type annotation of a binding. It stops where
  // the body begins, so a body edit cannot change it.
  support::Span span;
  // Just the declared name, for a caret and for `source_to_def`.
  support::Span nameSpan;
  std::uint32_t paramCount = 0;
  // True when the parameter list ends in `...`, which is part of the signature
  // and not an extra: `f(i32)` and `f(i32, ...)` are different functions, so a
  // change between them has to invalidate whatever the item tree keys on.
  bool variadic = false;
  bool hasBody = false;
  // True when the declaration was written `static`, which makes it internal to
  // this unit. Part of the signature and not a detail: it decides the symbol the
  // linker sees and whether another unit can name the declaration at all, so an
  // edit that adds or removes the word has to rebuild the file scope.
  bool isStatic = false;
  // Where in this file's node array the declaration and its body are. Not part
  // of the signature; see above.
  std::uint32_t node = kInvalidAst;
  std::uint32_t body = kInvalidAst;

  [[nodiscard]] bool operator==(const Item& other) const {
    return kind == other.kind && name == other.name && span == other.span &&
           nameSpan == other.nameSpan && paramCount == other.paramCount &&
           variadic == other.variadic && hasBody == other.hasBody && isStatic == other.isStatic;
  }
  [[nodiscard]] bool operator!=(const Item& other) const {
    return !(*this == other);
  }
};

// The declarations of one unit, and the thing the editor's invariant is keyed
// on. `hash` finds candidates and `==` confirms: comparing by hash alone would
// be a correctness bug waiting for a collision, and this structure is what
// decides whether the unit's scopes can be reused, so it is not the place for a
// probabilistic answer.
struct ItemTree {
  std::vector<Item> items;
  // FNV-1a over the compared fields. Only ever a filter; equality decides.
  std::uint64_t hash = 1469598103934665603ULL; // FNV-1a offset basis

  [[nodiscard]] bool operator==(const ItemTree& other) const {
    return items == other.items;
  }
  [[nodiscard]] bool operator!=(const ItemTree& other) const {
    return !(*this == other);
  }
  [[nodiscard]] bool empty() const {
    return items.empty();
  }
};

// The operands of a `SliceExpr`, split at the `..` that separates them.
//
// This is a function and not a field because the *source* is where the answer
// is: `a[1..]` and `a[..1]` are the same shape -- one operand and a separator --
// and only the separator's position says which bound was written. Counting
// children would make them the same tree; splitting at the separator keeps them
// two trees, which is what the reader typed and what a diagnostic has to name
// (`slices.md` decision 8).
//
// A bound that was not written is `kInvalidAst`, and that is not a failure: it
// is the form. `a[..]` has neither.
struct SliceParts {
  AstId base;
  AstId begin;
  AstId end;

  [[nodiscard]] bool hasBase() const {
    return base.valid();
  }
  [[nodiscard]] bool hasBegin() const {
    return begin.valid();
  }
  [[nodiscard]] bool hasEnd() const {
    return end.valid();
  }
};

class LoweredFile {
public:
  LoweredFile() = default;

  [[nodiscard]] support::FileId file() const {
    return file_;
  }
  [[nodiscard]] std::uint32_t revision() const {
    return revision_;
  }
  [[nodiscard]] const std::vector<Node>& nodes() const {
    return nodes_;
  }
  [[nodiscard]] std::size_t nodeCount() const {
    return nodes_.size();
  }
  [[nodiscard]] std::span<const AstId> children() const {
    return children_;
  }
  // The preprocessed text the unit-text spans index. A view: it belongs to the
  // `PPResult` that produced it and must outlive this file.
  [[nodiscard]] std::string_view unitText() const {
    return unitText_;
  }

  [[nodiscard]] AstId root() const {
    return AstId{0};
  }
  // Precondition: `id.valid()` and inside this file.
  [[nodiscard]] const Node& at(AstId id) const {
    return nodes_[id.index];
  }
  [[nodiscard]] std::span<const AstId> childrenOf(AstId id) const {
    const Node& node = at(id);
    return std::span<const AstId>(children_.data() + node.firstChild, node.childCount);
  }
  // `kInvalidAst` when there is no child of that kind.
  [[nodiscard]] AstId childOfKind(AstId id, NodeKind kind) const;
  [[nodiscard]] std::vector<AstId> childrenOfKind(AstId id, NodeKind kind) const;
  // The three operands of a `SliceExpr`, in the order the reader wrote them.
  [[nodiscard]] SliceParts slicePartsOf(AstId id) const;
  // The interned spelling of a name-bearing node, as text.
  [[nodiscard]] std::string_view spellingOf(AstId id) const;
  [[nodiscard]] std::string_view spellingOf(const Node& node) const;

  [[nodiscard]] const ItemTree& items() const {
    return items_;
  }
  // True when the node at `id` lies inside a region the parser could not
  // understand. Resolution and validation both skip those: the parser already
  // reported them, and a second diagnostic for one mistake is worse than none.
  [[nodiscard]] bool inErrorRegion(AstId id) const {
    return at(id).inError;
  }

  // Built by `lowerFile`; public so a test can construct one by hand.
  [[nodiscard]] static LoweredFile make(support::FileId file, std::uint32_t revision,
                                        std::string_view unitText, std::vector<Node> nodes,
                                        std::vector<AstId> children, ItemTree items);

private:
  support::FileId file_ = support::kInvalidFile;
  std::uint32_t revision_ = 0;
  std::string_view unitText_;
  std::vector<Node> nodes_;
  std::vector<AstId> children_;
  ItemTree items_;
};

} // namespace minc::ast
