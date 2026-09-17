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
#include "builtins/builtin.h"
#include "parse/syntax_kind.h"
#include "resolve/def.h"
#include "resolve/map.h"
#include "resolve/scope.h"
#include "support/intern/interner.h"
#include "support/intern/sym_id.h"
#include "support/span/file_id.h"
#include "support/span/span.h"
#include "support/typenames/type_name.h"

namespace minc::resolve {
namespace {

using ast::AstId;
using ast::Node;
using ast::NodeKind;

// What an item *declares*, from the node it is. `nullopt` for a node kind that
// is not a file-scope item, which is how the collect pass stays total over a
// summary it did not have to trust: the mapping lives here and not in `ast`,
// because "a `ConstStmt` declares a constant" is a name-resolution fact and not
// a fact about the shape of a tree.
[[nodiscard]] std::optional<DefKind> defKindOf(NodeKind kind) {
  switch (kind) {
  case NodeKind::FnDecl:
    return DefKind::Function;
  case NodeKind::LetStmt:
    return DefKind::Variable;
  case NodeKind::ConstStmt:
    return DefKind::Constant;
  case NodeKind::TypeAliasDecl:
    return DefKind::TypeAlias;
  default:
    return std::nullopt;
  }
}

// The namespace a declaration puts its name in. Everything a *program* names is
// ordinary -- a function, a binding, a parameter -- while a name for a **type**
// goes in `Tag`, which is where the two can never be confused because the
// grammar already decides by position which one a name is (`type_alias.md`,
// decision 3). C puts a `typedef` in the ordinary namespace, which is why C needs
// `typedef struct T T;` to have one name in each; this language does not.
[[nodiscard]] Namespace namespaceOf(DefKind kind) {
  return kind == DefKind::TypeAlias ? Namespace::Tag : Namespace::Ordinary;
}

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
    // Everything below reads the file scope, which `collectItems` has finished
    // and `resolveBodies` has only consulted, so the two phases stay the two
    // phases `resolve.md` describes: collect, then resolve.
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

  // A program may not take a name the compiler keeps (`__builtin_...`,
  // `builtins/builtin.h`). One rule, called from the three places a *program*
  // declares something -- an item, a parameter, a local -- and deliberately not
  // from the two that bind the language's own rows, which is the whole reason it
  // is a function here instead of a condition at each call site: a choke point
  // that also covers the compiler's own bindings would refuse the table.
  //
  // The declaration is still *inserted* after this is reported. Refusing to
  // insert would turn one mistake into two -- an unknown name at every use of it
  // -- and the reader would meet the second one first. This is an error, so the
  // compilation stops before any stage can act on the name, and nothing needs to
  // pretend the declaration is usable.
  void reportReservedName(support::Span nameSpan, support::SymId name) {
    if (name == support::kInvalidSym) {
      return;
    }
    // A `std::string` and not a view of one: `nameOf` returns by value, and a view
    // of its result would dangle at the end of the full expression (`-Wdangling-gsl`,
    // which is how this was written the first time).
    const std::string spelling = nameOf(name);
    // **A type name is reserved**, and this is the half of `(T)x` that is not
    // about casts at all: the C spelling is decidable without a symbol table
    // *because* a reserved word can never be a declaration, so the parser may
    // read a run of type names inside parentheses as a type and be right
    // (`casts.md`, decision 4). The sentence says so, because "reserved" alone
    // would leave the reader looking for a reason.
    if (support::isTypeNameWord(spelling)) {
      errors_.push_back(
          ResolveError{nameSpan,
                       "'" + std::string(spelling) +
                           "' names a type, and a type name is reserved: it is what makes "
                           "`(T)x` a cast rather than a call -- choose another name",
                       ResolveErrorCode::ReservedIdentifier,
                       {},
                       {}});
      return;
    }
    if (!builtins::isReservedPrefix(spelling)) {
      return;
    }
    errors_.push_back(
        ResolveError{nameSpan,
                     "'" + std::string(spelling) + "' is a name the compiler keeps for itself",
                     ResolveErrorCode::ReservedIdentifier,
                     {},
                     {}});
  }

  [[nodiscard]] DefId insertDef(ScopeId scopeId, Namespace ns, support::SymId name,
                                support::Span span, support::Span nameSpan,
                                support::Span nameUnitSpan, DefKind kind, Linkage linkage,
                                bool inError) {
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
    def.unitSpan = nameUnitSpan;
    def.name = name;
    def.kind = kind;
    def.scope = scopeId;
    def.ns = ns;
    def.linkage = linkage;
    def.inError = inError;
    const DefId id = defIdOf(def, static_cast<std::uint32_t>(map_.defs.size()));
    // A declaration that claims a name is its own identity; the two paths below
    // are the ones that hand that identity to a declaration written earlier.
    def.canonical = id;

    const auto existing = scope.byName[nsIndex].find(name);
    if (existing != scope.byName[nsIndex].end()) {
      const DefId canonical = existing->second;
      // A name the *table* binds is not redeclarable, and the function case is
      // why this has to be said: a repeated function declaration is legal, so a
      // user `fn clz(...)` would otherwise be absorbed into the row's chain --
      // the call would keep answering with the builtin while a body was defined
      // under the same symbol, which is a program meaning two things at once.
      // Refused as a redeclaration, so the reader gets one sentence about the
      // name they wrote rather than a wrong answer about the name they meant.
      //
      // An ordinary name is untouched by this: the predefined values are not
      // functions, so they already take the redeclaration path below.
      if (map_.defs[canonical.index].builtin != builtins::BuiltinId::None) {
        // A *reserved* spelling has already been answered by
        // `reportReservedName`, and its sentence is the better one: the name is
        // the compiler's, which is why it cannot be declared -- where this one
        // only says the name is taken. One mistake, one diagnostic, so the
        // reserved class reports here and nothing else does.
        const builtins::BuiltinInfo* const row =
            builtins::lookup(map_.defs[canonical.index].builtin);
        if (row == nullptr || !row->isReserved()) {
          errors_.push_back(
              ResolveError{nameSpan,
                           "'" + nameOf(name) + "' is a builtin, so it cannot be declared again",
                           ResolveErrorCode::Redeclaration,
                           {},
                           {}});
        }
        map_.defs.push_back(def);
        map_.defs[id.index].canonical = canonical;
        map_.defs[id.index].nextRedundant = canonical;
        map_.defs[id.index].hasProblem = true;
        return kInvalidDef;
      }
      if (kind == DefKind::Function && map_.defs[canonical.index].kind == DefKind::Function) {
        // C lets a function be declared many times and a header be included
        // twice, so a repeated function declaration extends the chain instead of
        // being an error. The canonical definition stays the lookup answer --
        // and, from here on, the *identity*: a second declaration of one
        // function is one function, so it answers to one `DefId`.
        DefId tail = canonical;
        while (map_.defs[tail.index].nextRedundant.valid()) {
          tail = map_.defs[tail.index].nextRedundant;
        }
        map_.defs.push_back(def);
        map_.defs[id.index].canonical = canonical;
        map_.defs[tail.index].nextRedundant = id;
        return canonical;
      }
      // Anything else with the same name in one scope and namespace is a
      // redeclaration. The new declaration is recorded -- the IDE wants the
      // site -- and linked into the chain, but it does not become the lookup
      // answer and it is not reported again by the unused pass. Its identity is
      // the earlier declaration's: the name denotes one thing, and the second
      // declaration is a mistake about that thing rather than a second one.
      errors_.push_back(ResolveError{nameSpan,
                                     "redeclaration of '" + nameOf(name) + "'",
                                     ResolveErrorCode::Redeclaration,
                                     {},
                                     {}});
      map_.defs.push_back(def);
      map_.defs[id.index].canonical = canonical;
      map_.defs[id.index].nextRedundant = canonical;
      map_.defs[id.index].hasProblem = true;
      return kInvalidDef;
    }

    // The ordinary namespace *and* the type one: a name that hides a type is as
    // confusing as one that hides a value, and the rule is one rule
    // (`type_alias.md`, decision 3).
    if (options_.warnShadow && (ns == Namespace::Ordinary || ns == Namespace::Tag) &&
        scope.parent.valid()) {
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
    installBuiltins();

    const std::vector<ast::Item>& items = file_.items().items;
    map_.itemDefs.assign(items.size(), kInvalidDef);
    map_.itemScopes.assign(items.size(), kInvalidScopeId);
    for (std::size_t i = 0; i < items.size(); ++i) {
      const ast::Item& item = items[i];
      const std::optional<DefKind> kind = defKindOf(item.kind);
      if (!kind.has_value()) {
        continue;
      }
      // A nameless declaration was already reported by the parser (it inserts a
      // zero-width name). Recording nothing is how this stage keeps the
      // "one mistake, one diagnostic" rule.
      if (item.name == support::kInvalidSym) {
        continue;
      }
      const Node& node = file_.at(AstId{item.node});
      // The item tree carries the *written* spans -- they are part of the
      // signature the editor keys on, and a unit offset is not: it shifts when
      // anything earlier in the unit text changes. So the name's unit range is
      // read from the tree here rather than added to `Item`.
      const AstId nameNode = file_.childOfKind(AstId{item.node}, NodeKind::Name);
      const support::Span nameUnit = nameNode.valid() ? file_.at(nameNode).unit : item.nameSpan;
      // `static` is "this unit only"; a file-scope declaration without it is
      // external, which is C's default and the answer that keeps the name
      // visible to a linker. Visibility across *modules* is a different axis and
      // belongs to the module system (`globals.md`, decision 5) -- it filters
      // lookup and does not rewrite this field.
      const Linkage linkage = item.isStatic ? Linkage::Internal : Linkage::External;
      reportReservedName(item.nameSpan, item.name);
      map_.itemDefs[i] = insertDef(map_.fileScope, namespaceOf(*kind), item.name, item.span,
                                   item.nameSpan, nameUnit, *kind, linkage, node.inError);
    }
  }

  // The language's own names, bound in the file scope before anything is read
  // -- Go's universe block, and the reason `true` is not an unknown name. The
  // lexer deliberately leaves them as identifiers (they are values, not
  // grammar), so resolving them is exactly this stage's job; the *types* they
  // have are `sema`'s to decide, and the constants `ir`'s.
  //
  // The list is `kPredefinedNames` and nothing else -- not a second copy of it
  // here. This loop binds a name and records *which* name it bound; the stages
  // below read that field instead of matching on spelling, which is what keeps
  // the three of them from disagreeing about the same name.
  //
  // They are ordinary defs in the file scope, so a `let null = 1;` shadows one
  // exactly as any other binding would, and `-Wshadow` says so. That is the
  // same rule every name follows and needs no exception here.
  void installPredefined() {
    const support::Span nowhere(file_.file(), 0, 0);
    for (const PredefinedName& row : kPredefinedNames) {
      const support::SymId name = symbols_.intern(row.spelling);
      if (name == support::kInvalidSym) {
        continue; // the interner is full; nothing can be added anyway
      }
      const DefId id = insertDef(map_.fileScope, Namespace::Ordinary, name, nowhere, nowhere,
                                 nowhere, DefKind::Constant, Linkage::None, /*inError=*/false);
      if (id.valid() && id.index < map_.defs.size()) {
        map_.defs[id.index].predefined = row.name;
      }
    }
  }

  // The builtins, bound exactly like the names above and for the same reason: a
  // call has to resolve to *something*, and resolving a builtin through the same
  // scope as every other name is what makes `clz(1)` a call whose errors read
  // like a function's, and what makes a program that writes its own `clz` call
  // its own. No name matching happens anywhere (`-fno-builtin` exists because GCC
  // does it the other way), so there is nothing here to switch off.
  //
  // Both spelling classes are bound, and the difference between them is not
  // visible *here* -- it is a rule about who may take the name: the preprocessor
  // refuses a `#define` of the prefix and `sema` refuses a declaration of it, so
  // the reserved rows cannot be shadowed while the prelude ones can.
  void installBuiltins() {
    const support::Span nowhere(file_.file(), 0, 0);
    for (const builtins::BuiltinInfo& row : builtins::all()) {
      const support::SymId name = symbols_.intern(row.spelling);
      if (name == support::kInvalidSym) {
        continue; // the interner is full; nothing can be added anyway
      }
      // `Linkage::None` and not `External`: the latter is what a *declaration* in
      // the unit asks the linker for, and a builtin asks it for nothing. The two
      // prelude classes then behave identically under redeclaration, which is
      // what keeps `let clz = 1;` shadowing it exactly as `let true = 1;` shadows
      // `true`.
      const DefId id = insertDef(map_.fileScope, Namespace::Ordinary, name, nowhere, nowhere,
                                 nowhere, DefKind::Function, Linkage::None, /*inError=*/false);
      if (id.valid() && id.index < map_.defs.size()) {
        map_.defs[id.index].builtin = row.id;
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
      if (item.kind == NodeKind::FnDecl) {
        resolveFunctionBody(item, i);
        continue;
      }
      // A type name has no body to resolve: its right-hand side is a `Type` node,
      // and the words of a type are read by the checker, which owns the type
      // vocabulary -- a name in a type position is not an expression and must not
      // be looked up as one (`type_alias.md`, decision 10).
      if (item.kind == NodeKind::TypeAliasDecl) {
        continue;
      }
      // A file-scope binding's initializer is resolved in the **file scope**, and
      // not in a scope of its own. Two reasons, and both are already decided:
      // a file-scope name is visible independently of order (`resolve.md`,
      // decision A), so the initializer sees every item in the unit; and that
      // includes the binding being declared, so `const a = a + 1;` resolves to
      // *itself* here -- which is what lets `sema` answer it with one sentence
      // about a cycle instead of one about an unknown name.
      resolveExpression(AstId{item.body}, map_.fileScope);
    }
  }

  void resolveFunctionBody(const ast::Item& item, std::size_t index) {
    const AstId body{item.body};
    const Node& bodyNode = file_.at(body);
    if (bodyNode.inError) {
      return;
    }
    // The function's body block *is* the function scope, as in C: there is no
    // second block scope around it, and the parameters enter this scope, so a
    // parameter and a `let` at the top of the body cannot both take one name.
    const ScopeId functionScope =
        createScope(ScopeKind::Function, map_.fileScope, bodyNode.origin, body.index);
    if (!functionScope.valid()) {
      return;
    }
    map_.itemScopes[index] = functionScope;
    declareParameters(item, functionScope);
    walkBody(body, functionScope);
  }

  // One expression, resolved in a scope. Used for a file-scope binding's
  // initializer, whose subtree is not a block and so cannot go through
  // `pushChildren` -- a `PathExpr` root is *itself* the name use, and pushing its
  // children would skip the one node there is to resolve.
  void resolveExpression(AstId expr, ScopeId scope) {
    std::vector<Step> stack;
    stack.push_back(Step{Step::Op::Visit, expr, scope});
    walk(std::move(stack));
  }

  // Parameters are declared in the function scope before the body is walked, so
  // a use anywhere in the body -- including in a nested block that shadows them
  // -- answers to the parameter's definition. They live in the `ParamList`, which
  // is a *sibling* of the body block, so nothing in the body walk would reach
  // them; that is why this is a step of its own rather than a case in `visit`.
  void declareParameters(const ast::Item& item, ScopeId functionScope) {
    if (item.paramCount == 0) {
      return;
    }
    const AstId decl{item.node};
    if (!decl.valid()) {
      return;
    }
    const AstId paramList = file_.childOfKind(decl, NodeKind::ParamList);
    if (!paramList.valid()) {
      return;
    }
    for (const AstId child : file_.childrenOf(paramList)) {
      const Node& param = file_.at(child);
      if (param.isToken() || param.kind != NodeKind::Param) {
        continue;
      }
      const AstId nameNode = file_.childOfKind(child, NodeKind::Name);
      if (!nameNode.valid()) {
        continue;
      }
      const Node& name = file_.at(nameNode);
      if (name.name == support::kInvalidSym) {
        continue; // the parser already reported the missing name
      }
      reportReservedName(name.origin, name.name);
      (void)insertDef(functionScope, Namespace::Ordinary, name.name, param.origin, name.origin,
                      name.unit, DefKind::Parameter, Linkage::None, param.inError || name.inError);
    }
  }

  void walkBody(AstId body, ScopeId functionScope) {
    std::vector<Step> stack;
    pushChildren(body, functionScope, stack);
    walk(std::move(stack));
  }

  // The one walk. Iterative, on an explicit stack, for the reason at the top of
  // this file: recursion would make the depth of the input decide the depth of
  // the call stack, and a stack overflow is a crash and not a diagnostic.
  void walk(std::vector<Step> stack) {
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
    case NodeKind::ForStmt: {
      // A `for` opens a scope of its own, so a binding declared in the
      // initializer is visible in the condition, the step and the body, and
      // nowhere else -- and so it cannot outlive the loop. The body block then
      // opens a `Block` inside it, which is what makes a shadowing declaration
      // in the body legal.
      const ScopeId loop = createScope(ScopeKind::Loop, scope, self.origin, node.index);
      if (loop.valid()) {
        pushChildren(node, loop, stack);
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
    case NodeKind::TypeAliasDecl:
      // `type T = ...;` among the statements. Its name is visible from the
      // declaration to the end of the block and not before it, which is the same
      // shape as the two bindings above and for the same reason: a block is read
      // top to bottom. The name goes in the *type* namespace (`namespaceOf`), so
      // the same statement may take an ordinary name already in scope -- and the
      // target may see a name the declaration is about to hide, which is why the
      // declare step is pushed under the target's children.
      stack.push_back(Step{Step::Op::Declare, node, scope});
      pushChildren(node, scope, stack);
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
    // What this statement declares, from the one table that says -- so a form
    // added to it is bound here without a second list to keep in sync. A
    // statement that declares nothing has nothing to insert.
    const std::optional<DefKind> kind = defKindOf(self.kind);
    if (!kind.has_value()) {
      return;
    }
    reportReservedName(name.origin, name.name);
    (void)insertDef(scope, namespaceOf(*kind), name.name, self.origin, name.origin, name.unit,
                    *kind, Linkage::None, self.inError);
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
    //
    // A *type name* is the one case with a better sentence than "unknown", and it
    // comes in two spellings that get one answer: a word of the language (`x =
    // (i32);`) and a name this unit declared with `type` (`let x = Bytes;`).
    // Neither is a name that could have been declared and was not -- the second
    // *is* declared, in the other namespace -- so "unknown name" would send the
    // reader looking for a typo that is not there. The mistake is using a type
    // where a value belongs, the fix is the cast or the value they meant, and when
    // the declaration exists the note points at it.
    //
    // This is the whole of why the two namespaces are visible to each other here:
    // the *lookup* is the ordinary one and stays that way, and only the sentence
    // that reports its failure asks the type namespace a question.
    std::string message;
    std::string note;
    support::Span noteSpan = self.origin;
    support::SymId suggestion = support::kInvalidSym;
    if (support::isTypeNameWord(nameOf(self.name))) {
      const std::string spelling(nameOf(self.name));
      message = "'" + spelling + "' names a type, and a type is not a value; write a cast -- `(" +
                spelling + ")value` or `value as " + spelling + "`";
    } else if (const std::optional<DefId> declared =
                   lookup(map_, scope, Namespace::Tag, self.name)) {
      const std::string spelling(nameOf(self.name));
      message = "'" + spelling + "' names a type, so it has no value; write a value of type `" +
                spelling + "` here, or a cast to it";
      if (declared->index < map_.defs.size()) {
        const Def& def = map_.defs[declared->index];
        noteSpan = def.nameSpan;
        note = def.inError ? std::string{} : "declared as a name for a type here";
        // **Not counted as a use of it.** The name resolved to nothing, and
        // giving the type def a reference would tell `-Wunused` and the checker
        // that a value read it -- the one thing this use did not do.
      }
      // The use is still recorded, with the *reason* reserved for exactly this: a
      // name that exists in another namespace. A consumer that asks why a name did
      // not resolve gets "wrong namespace" rather than "not found".
      record(self, kInvalidDef, UnresolvedReason::WrongNamespace, support::kInvalidSym);
      errors_.push_back(ResolveError{self.origin, message, ResolveErrorCode::UnknownName,
                                     std::move(note), noteSpan});
      return;
    } else {
      message = "unknown name '" + std::string(nameOf(self.name)) + "'";
      suggestion = suggestName(map_, scope, Namespace::Ordinary, self.name, symbols_,
                               options_.maxSuggestionCandidates, options_.maxSuggestionDistance);
      if (suggestion != support::kInvalidSym) {
        note = "did you mean '" + nameOf(suggestion) + "'?";
        if (const std::optional<DefId> candidate =
                lookup(map_, scope, Namespace::Ordinary, suggestion)) {
          if (candidate->index < map_.defs.size()) {
            noteSpan = map_.defs[candidate->index].nameSpan;
          }
        }
      }
    }
    errors_.push_back(ResolveError{self.origin, message, ResolveErrorCode::UnknownName,
                                   std::move(note), noteSpan});
    record(self, kInvalidDef, UnresolvedReason::NotFound, suggestion);
  }

  void reportUnused() {
    for (const Def& def : map_.defs) {
      if (def.inError || def.hasProblem || isLanguageDef(def) || def.refCount != 0 ||
          def.name == support::kInvalidSym) {
        continue;
      }
      // Only a declaration that could have been read and was not, and only one
      // whose reach stops at this unit. An externally linked name -- a function
      // another unit may call, a constant it may import -- is visible to the
      // whole program, so "unused" would be a claim about every unit at once,
      // which one unit cannot make. A file-scope `static` binding and a
      // block-scope binding are this unit's own business and are reported.
      if (def.kind != DefKind::Variable && def.kind != DefKind::Constant &&
          def.kind != DefKind::Parameter) {
        continue;
      }
      if (def.linkage == Linkage::External) {
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
