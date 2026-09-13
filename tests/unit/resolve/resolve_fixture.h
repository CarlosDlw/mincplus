// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Run the whole front end over a string and keep every stage's artifact alive.
//
// A lowered AST is a value, but its `unitText` is a view into the preprocessor's
// text and its spans name files the session owns, so the fixture owns a
// `Session`, a `PPResult`, a `TreeStore` and a `ResolveStore` in the right order
// and hands the test the pieces.
//
// It runs the *real* pipeline -- lex, preprocess, parse, lower, validate,
// resolve -- because the thing under test is the pipeline, and a fixture that
// stubbed a stage could pass while the stage was wrong.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ast/ast.h"
#include "ast/dump.h"
#include "ast/lower.h"
#include "ast/validate.h"
#include "lex/token_stream.h"
#include "parse/parse_report.h"
#include "pp/pp_error.h"
#include "pp/preprocessor.h"
#include "resolve/dump.h"
#include "resolve/resolve.h"
#include "resolve/resolve_error.h"
#include "support/diag/diag_bag.h"
#include "support/session/session.h"
#include "support/source/source_file.h"
#include "syntax/store.h"
#include "syntax/tree.h"

namespace minc::test {

class ResolveFixture {
public:
  explicit ResolveFixture(std::string name = "test.mx") : name_(std::move(name)) {}

  ResolveFixture(const ResolveFixture&) = delete;
  ResolveFixture& operator=(const ResolveFixture&) = delete;

  ResolveFixture& source(std::string text) {
    source_ = std::move(text);
    return *this;
  }
  ResolveFixture& warnUnused(bool on = true) {
    options_.warnUnused = on;
    return *this;
  }
  ResolveFixture& warnShadow(bool on = true) {
    options_.warnShadow = on;
    return *this;
  }
  ResolveFixture& resolveOptions(resolve::ResolveOptions options) {
    options_ = options;
    return *this;
  }
  ResolveFixture& lowerLimits(ast::LowerLimits limits) {
    lowerLimits_ = limits;
    return *this;
  }
  // `-D name=body`, so a test can put a name in a macro body.
  ResolveFixture& define(std::string name, std::string body = {}) {
    defines_.emplace_back(std::move(name), std::move(body));
    return *this;
  }

  // True when the pipeline got as far as a resolution. A false result means the
  // diagnostic is in `ppErrors()` or `parseErrors()`.
  bool build() {
    const auto file = session_.addFile(name_, source_);
    if (!file.hasValue()) {
      return false;
    }

    pp::PPOptions ppOptions;
    ppOptions.defines = defines_;
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
    lowered_ = ast::lowerFile(*tree_, session_.symbols(), origins, lowerLimits_);
    for (const ast::AstError& error : lowered_.errors) {
      astErrorCodes_.emplace_back(ast::toString(error.code));
    }
    structural_ = ast::validate(lowered_.file);
    for (const ast::AstError& error : structural_) {
      astErrorCodes_.emplace_back(ast::toString(error.code));
    }

    resolved_ = resolve::resolveUnit(lowered_.file, session_.symbols(), options_);
    return true;
  }

  [[nodiscard]] const support::Session& session() const {
    return session_;
  }
  [[nodiscard]] const ast::LoweredFile& lowered() const {
    return lowered_.file;
  }
  [[nodiscard]] const resolve::ResolveOutput& resolved() const {
    return resolved_;
  }
  [[nodiscard]] const resolve::DefMap& map() const {
    return resolved_.map;
  }
  // Mutable: resolution binds the language's predefined names, so it interns.
  [[nodiscard]] support::Interner& symbols() {
    return session_.symbols();
  }
  [[nodiscard]] const std::vector<ast::AstError>& structural() const {
    return structural_;
  }

  [[nodiscard]] std::vector<std::string> ppErrors() const {
    return ppErrors_;
  }
  [[nodiscard]] std::vector<std::string> parseErrors() const {
    return parseErrors_;
  }
  [[nodiscard]] std::vector<std::string> astErrorCodes() const {
    return astErrorCodes_;
  }
  [[nodiscard]] std::vector<std::string> resolveErrorCodes() const {
    std::vector<std::string> out;
    for (const resolve::ResolveError& error : resolved_.errors) {
      out.emplace_back(resolve::toString(error.code));
    }
    return out;
  }
  [[nodiscard]] std::vector<std::string> resolveWarningCodes() const {
    std::vector<std::string> out;
    for (const resolve::ResolveError& warning : resolved_.warnings) {
      out.emplace_back(resolve::toString(warning.code));
    }
    return out;
  }
  [[nodiscard]] bool hasResolveError(std::string_view code) const {
    return contains(resolveErrorCodes(), code);
  }
  [[nodiscard]] bool hasResolveWarning(std::string_view code) const {
    return contains(resolveWarningCodes(), code);
  }
  [[nodiscard]] bool hasAstError(std::string_view code) const {
    return contains(astErrorCodes(), code);
  }

  // The textual forms, so a test asserts on what the tool prints.
  [[nodiscard]] std::string dump() const {
    return resolve::dumpDefMap(resolved_.map, lowered_.file, session_.symbols(), session_.sources(),
                               resolve::DefMapDumpOptions{true, true, true, false});
  }
  [[nodiscard]] std::string dumpAst() const {
    return ast::dumpAst(lowered_.file, {});
  }
  [[nodiscard]] std::string dumpItems() const {
    ast::AstDumpOptions options;
    options.items = true;
    return ast::dumpAst(lowered_.file, options);
  }

  // The interned spelling of a definition or a name.
  [[nodiscard]] std::string_view spelling(support::SymId name) const {
    return session_.symbols().lookup(name);
  }
  // The definition whose spelling is `name`, or nullptr.
  [[nodiscard]] const resolve::Def* defNamed(std::string_view name) const {
    for (const resolve::Def& def : resolved_.map.defs) {
      if (spelling(def.name) == name) {
        return &def;
      }
    }
    return nullptr;
  }
  [[nodiscard]] std::size_t defCount(std::string_view name) const {
    std::size_t count = 0;
    for (const resolve::Def& def : resolved_.map.defs) {
      if (spelling(def.name) == name) {
        ++count;
      }
    }
    return count;
  }
  // The unparsed remainder the parser wrapped in an `Error` node, if any.
  [[nodiscard]] bool bailedOut() const {
    return tree_ != nullptr && tree_->stats().bailedOut;
  }

private:
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
  std::vector<std::pair<std::string, std::string>> defines_;
  ast::LowerLimits lowerLimits_;
  resolve::ResolveOptions options_;

  support::Session session_;
  pp::PPResult pp_;
  std::optional<lex::TokenStream> stream_;
  syntax::TreeStore trees_{session_.arena()};
  const syntax::SyntaxTree* tree_ = nullptr;
  ast::LowerOutput lowered_;
  std::vector<ast::AstError> structural_;
  resolve::ResolveOutput resolved_;

  std::vector<std::string> ppErrors_;
  std::vector<std::string> parseErrors_;
  std::vector<std::string> astErrorCodes_;
};

} // namespace minc::test
