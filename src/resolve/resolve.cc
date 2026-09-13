// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The resolver: collect the file scope, then walk the bodies.
//
// The body walk is iterative. Recursion would make the depth of the input decide
// the depth of the call stack, and a stack overflow is a crash, not a
// diagnostic; the explicit stack below has the same shape as the recursion it
// replaces and no bound of its own to get wrong.
#include "resolve/resolve.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ast/ast.h"
#include "ast/node.h"
#include "parse/syntax_kind.h"
#include "resolve/def.h"
#include "resolve/map.h"
#include "resolve/scope.h"
#include "support/intern/interner.h"
#include "support/intern/sym_id.h"
#include "support/span/file_id.h"
#include "support/span/span.h"

namespace minc::resolve {
namespace {

using ast::AstId;
using ast::Node;
using ast::NodeKind;

// One unit of work for the body walk. `Visit` looks at a node; `Declare`
// introduces a binding, and exists because a `let`'s initializer has to be
// resolved *before* its name enters scope -- the initializer sees the outer
// binding, not the one being declared.
struct Step {
  enum class Op : std::uint8_t { Visit, Declare };
  Op op = Op::Visit;
  AstId node;
  ScopeId scope;
};

class Resolver {
public:
  Resolver(const ast::LoweredFile& file, support::Interner& symbols, ResolveOptions options)
      : file_(file), symbols_(symbols), options_(options) {}

  [[nodiscard]] ResolveOutput run() {
    map_.maxScopeDepth = options_.maxScopeDepth;
    collectItems();
    resolveBodies();
    if (options_.warnUnused) {
      reportUnused();
    }
    ResolveOutput out;
    out.map = std::move(map_);
    out.errors = std::move(errors_);
    out.warnings = std::move(warnings_);
    return out;
  }

private:
  // --- scopes and definitions ------------------------------------------------

  [[nodiscard]] ScopeId createScope(ScopeKind kind, ScopeId parent, support::Span span,
                                    std::uint32_t node) {
    if (map_.scopes.size() >= options_.maxScopes) {
      if (!scopesLimitReported_) {
        scopesLimitReported_ = true;
        errors_.push_back(ResolveError{
            span, "too many scopes to resolve", ResolveErrorCode::LimitScopes, {}, {}});
      }
      return kInvalidScopeId;
    }
    Scope scope;
    scope.parent = parent;
    scope.kind = kind;
    scope.span = span;
    scope.node = node;
    map_.scopes.push_back(std::move(scope));
    return ScopeId{static_cast<std::uint32_t>(map_.scopes.size() - 1)};
  }

  [[nodiscard]] std::string nameOf(support::SymId name) const {
    return std::string(symbols_.lookup(name));
  }

  [[nodiscard]] DefId insertDef(ScopeId scopeId, Namespace ns, support::SymId name,
                                support::Span span, support::Span nameSpan, DefKind kind,
                                Linkage linkage, bool inError) {
    if (map_.defs.size() >= options_.maxDefs) {
      if (!defsLimitReported_) {
        defsLimitReported_ = true;
        errors_.push_back(ResolveError{
            nameSpan, "too many definitions to resolve", ResolveErrorCode::LimitDefs, {}, {}});
      }
      return kInvalidDef;
    }

    Scope& scope = map_.scopes[scopeId.index];
    const auto nsIndex = static_cast<std::size_t>(ns);
    Def def;
    def.span = span;
    def.nameSpan = nameSpan;
    def.name = name;
    def.kind = kind;
    def.scope = scopeId;
    def.ns = ns;
    def.linkage = linkage;
    def.inError = inError;
    const support::FileId owner =
        nameSpan.file != support::kInvalidFile ? nameSpan.file : span.file;
    const DefId id{owner, static_cast<std::uint32_t>(map_.defs.size())};

    const auto existing = scope.byName[nsIndex].find(name);
    if (existing != scope.byName[nsIndex].end()) {
      const DefId canonical = existing->second;
      if (kind == DefKind::Function && map_.defs[canonical.index].kind == DefKind::Function) {
        // C lets a function be declared many times and a header be included
        // twice, so a repeated function declaration extends the chain instead of
        // being an error. The canonical definition stays the lookup answer.
        DefId tail = canonical;
        while (map_.defs[tail.index].nextRedundant.valid()) {
          tail = map_.defs[tail.index].nextRedundant;
        }
        map_.defs.push_back(def);
        map_.defs[tail.index].nextRedundant = id;
        return canonical;
      }
      // Anything else with the same name in one scope and namespace is a
      // redeclaration. The new declaration is recorded -- the IDE wants the
      // site -- and linked into the chain, but it does not become the lookup
      // answer and it is not reported again by the unused pass.
      errors_.push_back(ResolveError{nameSpan,
                                     "redeclaration of '" + nameOf(name) + "'",
                                     ResolveErrorCode::Redeclaration,
                                     {},
                                     {}});
      map_.defs.push_back(def);
      map_.defs[id.index].nextRedundant = canonical;
      map_.defs[id.index].hasProblem = true;
      return kInvalidDef;
    }

    if (options_.warnShadow && ns == Namespace::Ordinary && scope.parent.valid()) {
      if (const std::optional<DefId> outer = lookup(map_, scope.parent, ns, name)) {
        warnings_.push_back(
            ResolveError{nameSpan,
                         "declaration of '" + nameOf(name) + "' shadows an earlier declaration",
                         ResolveErrorCode::ShadowedName,
                         {},
                         map_.defs[outer->index].nameSpan});
      }
    }

    map_.defs.push_back(def);
    scope.byName[nsIndex].emplace(name, id);
    scope.table(ns).push_back(id);
    return id;
  }

  // --- phase 1: collect ------------------------------------------------------

  void collectItems() {
    // The file scope covers the whole unit. Its span is the root's *unit* range,
    // not the root's origin: a unit-spanning node's origin is the written range
    // of its first and last token, which is a different coordinate system
    // whenever the preprocessor changed the text.
    const Node& root = file_.at(file_.root());
    map_.fileScope = createScope(ScopeKind::File, kInvalidScopeId, root.unit, ast::kInvalidAst);
    installPredefined();

    const std::vector<ast::Item>& items = file_.items().items;
    map_.itemDefs.assign(items.size(), kInvalidDef);
    map_.itemScopes.assign(items.size(), kInvalidScopeId);
    for (std::size_t i = 0; i < items.size(); ++i) {
      const ast::Item& item = items[i];
      if (item.kind != NodeKind::FnDecl) {
        continue;
      }
      // A nameless declaration was already reported by the parser (it inserts a
      // zero-width name). Recording nothing is how this stage keeps the
      // "one mistake, one diagnostic" rule.
      if (item.name == support::kInvalidSym) {
        continue;
      }
      const Node& node = file_.at(AstId{item.node});
      map_.itemDefs[i] =
          insertDef(map_.fileScope, Namespace::Ordinary, item.name, item.span, item.nameSpan,
                    DefKind::Function, Linkage::External, node.inError);
    }
  }

  // The language's own names, bound in the file scope before anything is read
  // -- Go's universe block, and the reason `true` is not an unknown name. The
  // lexer deliberately leaves `true`/`false` as identifiers (they are values of
  // `bool`, not grammar), so resolving them is exactly this stage's job.
  void installPredefined() {
    const support::Span nowhere(file_.file(), 0, 0);
    for (const std::string_view spelling : {"true", "false"}) {
      const support::SymId name = symbols_.intern(spelling);
      if (name == support::kInvalidSym) {
        continue; // the interner is full; nothing can be added anyway
      }
      const DefId id = insertDef(map_.fileScope, Namespace::Ordinary, name, nowhere, nowhere,
                                 DefKind::Constant, Linkage::None, /*inError=*/false);
      if (id.valid() && id.index < map_.defs.size()) {
        map_.defs[id.index].predefined = true;
      }
    }
  }

  // --- phase 2: resolve ------------------------------------------------------

  void resolveBodies() {
    const std::vector<ast::Item>& items = file_.items().items;
    for (std::size_t i = 0; i < items.size(); ++i) {
      const ast::Item& item = items[i];
      if (!item.hasBody || item.body == ast::kInvalidAst) {
        continue;
      }
      const AstId body{item.body};
      const Node& bodyNode = file_.at(body);
      if (bodyNode.inError) {
        continue;
      }
      // The function's body block *is* the function scope, as in C: there is no
      // second block scope around it, and a parameter would enter this scope.
      const ScopeId functionScope =
          createScope(ScopeKind::Function, map_.fileScope, bodyNode.origin, body.index);
      if (!functionScope.valid()) {
        continue;
      }
      map_.itemScopes[i] = functionScope;
      walkBody(body, functionScope);
    }
  }

  void walkBody(AstId body, ScopeId functionScope) {
    std::vector<Step> stack;
    pushChildren(body, functionScope, stack);
    while (!stack.empty()) {
      const Step step = stack.back();
      stack.pop_back();
      if (step.op == Step::Op::Declare) {
        declareBinding(step.node, step.scope);
      } else {
        visit(step.node, step.scope, stack);
      }
    }
  }

  void visit(AstId node, ScopeId scope, std::vector<Step>& stack) {
    const Node& self = file_.at(node);
    if (self.inError) {
      // The parser already reported this region, and a second diagnostic for one
      // mistake is worse than none. Nothing inside it is resolved, so no name use
      // there is answered -- which is why "every use has an answer" is a claim
      // about the code the parser understood, not about every byte.
      return;
    }
    switch (self.kind) {
    case NodeKind::PathExpr:
      resolveName(node, scope);
      return; // its only child is the identifier leaf; there is nothing below
    case NodeKind::Block: {
      const ScopeId inner = createScope(ScopeKind::Block, scope, self.origin, node.index);
      if (inner.valid()) {
        pushChildren(node, inner, stack);
      }
      return;
    }
    case NodeKind::LetStmt:
    case NodeKind::ConstStmt:
      // The declaration comes *after* the initializer, so the stack is given the
      // declare step first and the initializer steps on top of it.
      stack.push_back(Step{Step::Op::Declare, node, scope});
      pushInitializers(node, scope, stack);
      return;
    default:
      pushChildren(node, scope, stack);
      return;
    }
  }

  void pushChildren(AstId node, ScopeId scope, std::vector<Step>& stack) {
    const std::span<const AstId> kids = file_.childrenOf(node);
    for (std::size_t i = kids.size(); i > 0; --i) {
      const AstId child = kids[i - 1];
      if (file_.at(child).isToken()) {
        continue; // a token has no name to resolve
      }
      stack.push_back(Step{Step::Op::Visit, child, scope});
    }
  }

  void pushInitializers(AstId stmt, ScopeId scope, std::vector<Step>& stack) {
    const std::span<const AstId> kids = file_.childrenOf(stmt);
    for (std::size_t i = kids.size(); i > 0; --i) {
      const AstId child = kids[i - 1];
      const Node& node = file_.at(child);
      if (node.isToken() || node.kind == NodeKind::Name || node.kind == NodeKind::Type) {
        continue;
      }
      stack.push_back(Step{Step::Op::Visit, child, scope});
    }
  }

  void declareBinding(AstId stmt, ScopeId scope) {
    const Node& self = file_.at(stmt);
    if (self.inError) {
      return;
    }
    const AstId nameNode = file_.childOfKind(stmt, NodeKind::Name);
    if (!nameNode.valid()) {
      return;
    }
    const Node& name = file_.at(nameNode);
    if (name.name == support::kInvalidSym) {
      return; // the parser reported the missing name
    }
    const DefKind kind = self.kind == NodeKind::ConstStmt ? DefKind::Constant : DefKind::Variable;
    (void)insertDef(scope, Namespace::Ordinary, name.name, self.origin, name.origin, kind,
                    Linkage::None, self.inError);
  }

  void resolveName(AstId path, ScopeId scope) {
    const Node& self = file_.at(path);

    // Every name use gets an answer, including the ones inside a region the
    // parser did not understand: the reference still exists, it is just not
    // worth a second diagnostic.
    auto record = [this](const Node& node, DefId target, UnresolvedReason reason,
                         support::SymId suggestion) {
      if (map_.refs.size() >= options_.maxRefs) {
        if (!refsLimitReported_) {
          refsLimitReported_ = true;
          errors_.push_back(ResolveError{
              node.origin, "too many name uses to resolve", ResolveErrorCode::LimitRefs, {}, {}});
        }
        return;
      }
      NameRef ref;
      ref.span = node.origin;
      ref.unitSpan = node.unit;
      ref.name = node.name;
      ref.target = target;
      ref.reason = reason;
      ref.suggestion = suggestion;
      map_.refs.push_back(ref);
    };

    if (self.name == support::kInvalidSym) {
      record(self, kInvalidDef, UnresolvedReason::NotFound, support::kInvalidSym);
      return;
    }
    if (const std::optional<DefId> hit = lookup(map_, scope, Namespace::Ordinary, self.name)) {
      if (hit->index < map_.defs.size()) {
        ++map_.defs[hit->index].refCount;
      }
      record(self, *hit, UnresolvedReason::None, support::kInvalidSym);
      return;
    }

    // Not found. One error and at most one note; the note points at the
    // declaration the reader probably meant, which is more useful than the same
    // words on the line that is already wrong.
    const std::string message = "unknown name '" + nameOf(self.name) + "'";
    const support::SymId suggestion =
        suggestName(map_, scope, Namespace::Ordinary, self.name, symbols_,
                    options_.maxSuggestionCandidates, options_.maxSuggestionDistance);
    std::string note;
    support::Span noteSpan = self.origin;
    if (suggestion != support::kInvalidSym) {
      note = "did you mean '" + nameOf(suggestion) + "'?";
      if (const std::optional<DefId> candidate =
              lookup(map_, scope, Namespace::Ordinary, suggestion)) {
        if (candidate->index < map_.defs.size()) {
          noteSpan = map_.defs[candidate->index].nameSpan;
        }
      }
    }
    errors_.push_back(ResolveError{self.origin, message, ResolveErrorCode::UnknownName,
                                   std::move(note), noteSpan});
    record(self, kInvalidDef, UnresolvedReason::NotFound, suggestion);
  }

  void reportUnused() {
    for (const Def& def : map_.defs) {
      if (def.inError || def.hasProblem || def.predefined || def.refCount != 0 ||
          def.name == support::kInvalidSym) {
        continue;
      }
      // Only a local binding that could have been read and was not. A function
      // is externally visible, so "unused" would be a claim about the whole
      // program, which one unit cannot make.
      if (def.kind != DefKind::Variable && def.kind != DefKind::Constant &&
          def.kind != DefKind::Parameter) {
        continue;
      }
      const std::string_view spelling = symbols_.lookup(def.name);
      if (!spelling.empty() && spelling.front() == '_') {
        continue; // the conventional "I know; leave it alone"
      }
      warnings_.push_back(ResolveError{def.nameSpan,
                                       "unused " + std::string(toString(def.kind)) + " '" +
                                           std::string(spelling) + "'",
                                       ResolveErrorCode::UnusedEntity,
                                       {},
                                       {}});
    }
  }

  const ast::LoweredFile& file_;
  support::Interner& symbols_;
  ResolveOptions options_;
  DefMap map_;
  std::vector<ResolveError> errors_;
  std::vector<ResolveError> warnings_;
  bool defsLimitReported_ = false;
  bool scopesLimitReported_ = false;
  bool refsLimitReported_ = false;
};

} // namespace

ResolveOutput resolveUnit(const ast::LoweredFile& file, support::Interner& symbols,
                          ResolveOptions options) {
  Resolver resolver(file, symbols, options);
  return resolver.run();
}

} // namespace minc::resolve
