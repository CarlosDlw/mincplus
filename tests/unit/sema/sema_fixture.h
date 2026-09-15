// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Run the whole front end over a string and keep every stage's artifact alive,
// then type-check it.
//
// The `resolve` fixture, one stage further: the checker consumes the lowered
// tree *and* the def map, so both have to outlive it, and the `TypeStore` has to
// outlive the answer because every `TypeId` in the typed file indexes it.
//
// The pipeline is the real one -- lex, preprocess, parse, lower, validate,
// resolve, check -- because the thing under test is the pipeline, and a fixture
// that stubbed a stage could pass while the stage was wrong.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ast/ast.h"
#include "ast/lower.h"
#include "ast/node.h"
#include "ast/validate.h"
#include "lex/token_stream.h"
#include "parse/parse_report.h"
#include "parse/syntax_kind.h"
#include "pp/preprocessor.h"
#include "resolve/resolve.h"
#include "sema/dump.h"
#include "sema/sema.h"
#include "sema/sema_error.h"
#include "sema/target.h"
#include "sema/type_store.h"
#include "sema/typed_ast.h"
#include "support/session/session.h"
#include "support/source/source_file.h"
#include "syntax/store.h"
#include "syntax/tree.h"

namespace minc::test {

class SemaFixture {
public:
  // The target is fixed at construction because the `TypeStore` stores it: a
  // type's width is a property of the target it was read against, so changing
  // the target mid-fixture would leave types in the store that mean something
  // else. It is a `TargetInfo` and not a name, so a test that wants another
  // target asks the one parser a user's `--target` goes through.
  explicit SemaFixture(std::string name = "test.mx",
                       sema::TargetInfo target = sema::defaultTarget())
      : name_(std::move(name)), store_(std::move(target)) {}

  SemaFixture(const SemaFixture&) = delete;
  SemaFixture& operator=(const SemaFixture&) = delete;

  SemaFixture& source(std::string text) {
    source_ = std::move(text);
    return *this;
  }
  SemaFixture& warnConversion(bool on = true) {
    options_.warnConversion = on;
    return *this;
  }
  SemaFixture& semaOptions(sema::SemaOptions options) {
    options_ = options;
    return *this;
  }
  SemaFixture& resolveOptions(resolve::ResolveOptions options) {
    resolveOptions_ = options;
    return *this;
  }

  // True when the pipeline got as far as a check. A false result means the
  // reason is in `ppErrors()` or `parseErrors()`.
  bool build() {
    const auto file = session_.addFile(name_, source_);
    if (!file.hasValue()) {
      return false;
    }
    pp::PPOptions ppOptions;
    pp::Preprocessor preprocessor(session_, std::move(ppOptions));
    pp_ = preprocessor.run(file.value());
    for (const pp::PPError& error : pp_.errors) {
      ppErrors_.emplace_back(pp::toString(error.code));
    }

    stream_ = pp::preprocessedStream(pp_);
    tree_ = trees_.parse(*stream_, 0);
    if (tree_ == nullptr) {
      return false;
    }
    for (const parse::ParseError& error : tree_->errors()) {
      parseErrors_.emplace_back(error.codeName());
    }

    const ast::OriginTable origins{&*stream_};
    lowered_ = ast::lowerFile(*tree_, session_.symbols(), origins);
    for (const ast::AstError& error : lowered_.errors) {
      astErrorCodes_.emplace_back(ast::toString(error.code));
    }
    structural_ = ast::validate(lowered_.file);
    for (const ast::AstError& error : structural_) {
      astErrorCodes_.emplace_back(ast::toString(error.code));
    }

    resolved_ = resolve::resolveUnit(lowered_.file, session_.symbols(), resolveOptions_);
    output_ = sema::checkUnit(lowered_.file, resolved_.map, session_.symbols(), store_, options_);
    return true;
  }

  // --- artifacts -------------------------------------------------------------

  [[nodiscard]] const ast::LoweredFile& lowered() const {
    return lowered_.file;
  }
  [[nodiscard]] const resolve::DefMap& map() const {
    return resolved_.map;
  }
  [[nodiscard]] const sema::SemaOutput& output() const {
    return output_;
  }
  [[nodiscard]] const sema::TypedFile& typed() const {
    return output_.typed;
  }
  [[nodiscard]] const sema::TypeStore& types() const {
    return store_;
  }
  [[nodiscard]] support::Interner& symbols() {
    return session_.symbols();
  }
  [[nodiscard]] const support::SourceManager& sources() const {
    return session_.sources();
  }

  // --- diagnostics -----------------------------------------------------------

  // Every error, for a test that has a *sentence* to look for and not a code:
  // the sentences are what a reader repairs from, and a test that only checked
  // the code would pass with a message that says the wrong thing.
  [[nodiscard]] const std::vector<sema::SemaError>& errors() const {
    return output_.errors;
  }
  [[nodiscard]] std::vector<std::string> errorCodes() const {
    std::vector<std::string> out;
    for (const sema::SemaError& error : output_.errors) {
      out.emplace_back(sema::toString(error.code));
    }
    return out;
  }
  [[nodiscard]] std::vector<std::string> warningCodes() const {
    std::vector<std::string> out;
    for (const sema::SemaError& warning : output_.warnings) {
      out.emplace_back(sema::toString(warning.code));
    }
    return out;
  }
  [[nodiscard]] bool hasError(std::string_view code) const {
    return contains(errorCodes(), code);
  }
  [[nodiscard]] bool hasWarning(std::string_view code) const {
    return contains(warningCodes(), code);
  }
  [[nodiscard]] const sema::SemaError& firstError() const {
    return output_.errors.front();
  }
  [[nodiscard]] std::size_t errorCount() const {
    return output_.errors.size();
  }
  [[nodiscard]] std::size_t warningCount() const {
    return output_.warnings.size();
  }
  [[nodiscard]] bool hasParseError() const {
    return !parseErrors_.empty();
  }
  [[nodiscard]] bool hasPpError() const {
    return !ppErrors_.empty();
  }
  [[nodiscard]] bool hasAstError() const {
    return !astErrorCodes_.empty();
  }

  // --- queries ---------------------------------------------------------------

  // The type of the `Name` node of the first binding spelled `name`, as text.
  // An empty string when there is no such binding.
  [[nodiscard]] std::string bindingType(std::string_view name) const {
    const ast::AstId node = findBindingName(name);
    if (!node.valid()) {
      return {};
    }
    return store_.spelling(typed().typeOf(node));
  }
  // A type as the language spells it, for the tests that compare an element's
  // recorded type against the one the annotation wrote -- the two are the same
  // `TypeId` and asserting the *spelling* is how a failure prints something a
  // reader can act on instead of an index.
  [[nodiscard]] std::string spellingOfType(sema::TypeId id) const {
    return store_.spelling(id);
  }
  // The published value of a **file-scope** binding: what the lowering writes as
  // the object's bytes. Null when the unit has no binding with that name, which
  // is how a test tells `Zero` apart from "not a global at all".
  [[nodiscard]] const sema::GlobalInfo* globalOf(std::string_view name) const {
    const ast::AstId decl = findBindingDecl(name);
    return decl.valid() ? typed().globalOf(decl) : nullptr;
  }
  // The value kind of a file-scope binding, as text, or "none" when it has no
  // record. A string rather than the enumerator because a failure message that
  // prints `3` says nothing about which kind it was.
  [[nodiscard]] std::string globalKind(std::string_view name) const {
    const sema::GlobalInfo* info = globalOf(name);
    return info == nullptr ? std::string("none") : std::string(sema::toString(info->value));
  }
  // The file-scope bindings of the unit, in the order the table holds them.
  [[nodiscard]] std::vector<std::string> globalNames() const {
    std::vector<std::string> out;
    for (const ast::AstId id : allNodes()) {
      const ast::NodeKind kind = lowered().at(id).kind;
      if (kind != ast::NodeKind::LetStmt && kind != ast::NodeKind::ConstStmt) {
        continue;
      }
      const ast::AstId nameNode = lowered().childOfKind(id, ast::NodeKind::Name);
      if (!nameNode.valid()) {
        continue;
      }
      // Only the file scope: a binding inside a body has no published value, and
      // a body's nodes are *after* the file's items in the pre-order, so the
      // filter is the node's own parentage -- which the tree does not record. The
      // items are what `resolve` gave a file-scope definition, so that is the
      // question asked instead of the parent.
      if (typed().globalOf(id) == nullptr) {
        continue;
      }
      out.emplace_back(lowered().spellingOf(nameNode));
    }
    return out;
  }

  // The `ExprInfo` of the binding's initializer.
  [[nodiscard]] sema::ExprInfo initializerInfo(std::string_view name) const {
    for (const ast::AstId id : allNodes()) {
      const ast::NodeKind kind = lowered().at(id).kind;
      if (kind != ast::NodeKind::LetStmt && kind != ast::NodeKind::ConstStmt) {
        continue;
      }
      const ast::AstId nameNode = lowered().childOfKind(id, ast::NodeKind::Name);
      if (nameNode.valid() && lowered().spellingOf(nameNode) == name) {
        for (const ast::AstId child : lowered().childrenOf(id)) {
          if (lowered().at(child).isToken() || child == nameNode) {
            continue;
          }
          if (lowered().childOfKind(id, ast::NodeKind::Type) == child) {
            continue;
          }
          return typed().infoOf(child);
        }
      }
    }
    return {};
  }
  // The type of the expression with that spelling, when the unit has exactly one
  // node whose text is `text`.
  [[nodiscard]] std::string typeOfSpelling(std::string_view text) const {
    for (const ast::AstId id : allNodes()) {
      if (lowered().at(id).isToken() || lowered().at(id).kind == ast::NodeKind::Name) {
        continue;
      }
      if (lowered().spellingOf(id) == text) {
        return store_.spelling(typed().typeOf(id));
      }
    }
    return {};
  }

  // --- accesses --------------------------------------------------------------

  // The access record, as text pairs `place -> provenance`. The place is the
  // node's own spelling, which for `*p` is `*p` and for `p[i]` is `p[i]`; the
  // list is in the checker's walk order (source order).
  [[nodiscard]] std::vector<std::string> accessRecords() const {
    std::vector<std::string> out;
    for (const sema::AccessObligation& access : typed().accesses()) {
      out.emplace_back(std::string(lowered().spellingOf(access.place)) + " " +
                       std::string(sema::toString(access.provenance)) + " " +
                       store_.spelling(access.type));
    }
    return out;
  }
  [[nodiscard]] std::size_t accessCount() const {
    return typed().accesses().size();
  }
  // One access, as `provenance type extent`, or an empty string when the unit
  // records none for that place. The extent is the field a pointer access leaves
  // at 0 and an array subscript fills in, so this is the question the two askings
  // of "how far can this go" are told apart by (`arrays.md` decision 26).
  [[nodiscard]] std::string accessOf(std::string_view place) const {
    for (const sema::AccessObligation& access : typed().accesses()) {
      if (lowered().spellingOf(access.place) == place) {
        return std::string(sema::toString(access.provenance)) + " " + store_.spelling(access.type) +
               " " + std::to_string(access.extent);
      }
    }
    return {};
  }

  [[nodiscard]] std::string dump() const {
    return sema::dumpTypedFile(lowered(), typed(), store_);
  }
  [[nodiscard]] std::string dumpTypes() const {
    return sema::dumpTypeStore(store_);
  }

private:
  [[nodiscard]] std::vector<ast::AstId> allNodes() const {
    std::vector<ast::AstId> ids;
    ids.reserve(lowered().nodeCount());
    for (std::uint32_t i = 0; i < lowered().nodeCount(); ++i) {
      ids.push_back(ast::AstId{i});
    }
    return ids;
  }
  [[nodiscard]] ast::AstId findBindingName(std::string_view name) const {
    const ast::AstId decl = findBindingDecl(name);
    return decl.valid() ? lowered().childOfKind(decl, ast::NodeKind::Name) : ast::AstId{};
  }
  [[nodiscard]] ast::AstId findBindingDecl(std::string_view name) const {
    for (const ast::AstId id : allNodes()) {
      const ast::NodeKind kind = lowered().at(id).kind;
      if (kind != ast::NodeKind::LetStmt && kind != ast::NodeKind::ConstStmt) {
        continue;
      }
      const ast::AstId nameNode = lowered().childOfKind(id, ast::NodeKind::Name);
      if (nameNode.valid() && lowered().spellingOf(nameNode) == name) {
        return id;
      }
    }
    return ast::AstId{};
  }

  [[nodiscard]] static bool contains(const std::vector<std::string>& values,
                                     std::string_view wanted) {
    for (const std::string& value : values) {
      if (value == wanted) {
        return true;
      }
    }
    return false;
  }

  std::string name_;
  std::string source_;
  sema::SemaOptions options_;
  resolve::ResolveOptions resolveOptions_;

  support::Session session_;
  pp::PPResult pp_;
  std::optional<lex::TokenStream> stream_;
  syntax::TreeStore trees_{session_.arena()};
  const syntax::SyntaxTree* tree_ = nullptr;
  ast::LowerOutput lowered_;
  std::vector<ast::AstError> structural_;
  resolve::ResolveOutput resolved_;
  // The compilation's type store, named for what it is: the fixture owns one
  // because every `TypeId` in the answer indexes it.
  sema::TypeStore store_;
  sema::SemaOutput output_;

  std::vector<std::string> ppErrors_;
  std::vector<std::string> parseErrors_;
  std::vector<std::string> astErrorCodes_;
};

} // namespace minc::test
