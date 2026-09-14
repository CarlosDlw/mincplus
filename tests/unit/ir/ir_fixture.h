// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Run the whole front end over a string and lower it.
//
// The `sema` fixture, one stage further: the lowering consumes the typed tree
// *and* the resolution, so both have to outlive it, and the `TypeStore` has to
// outlive the module because every access's type comes from it. The pipeline is
// the real one -- lex, preprocess, parse, lower, validate, resolve, check, lower
// to IR -- because the thing under test is the pipeline, and a fixture that
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
#include "ast/lower.h"
#include "ast/validate.h"
#include "ir/dump.h"
#include "ir/invariants.h"
#include "ir/ir.h"
#include "lex/token_stream.h"
#include "pp/preprocessor.h"
#include "resolve/resolve.h"
#include "sema/sema.h"
#include "sema/target.h"
#include "sema/type_store.h"
#include "sema/typed_ast.h"
#include "support/session/session.h"
#include "syntax/store.h"
#include "syntax/tree.h"

namespace minc::test {

class IrFixture {
public:
  explicit IrFixture(std::string name = "test.mx", sema::TargetInfo target = sema::defaultTarget())
      : name_(std::move(name)), store_(std::move(target)) {}

  IrFixture(const IrFixture&) = delete;
  IrFixture& operator=(const IrFixture&) = delete;

  IrFixture& source(std::string text) {
    source_ = std::move(text);
    return *this;
  }
  IrFixture& warnConversion(bool on = true) {
    options_.warnConversion = on;
    return *this;
  }

  // Runs the front end and then the lowering. False only for a failure *before*
  // checking (an unreadable file, a syntax tree that could not be built); a
  // typing error still lowers, so a test can look at both answers.
  bool build() {
    const auto file = session_.addFile(name_, source_);
    if (!file.hasValue()) {
      return false;
    }
    pp::PPOptions ppOptions;
    pp::Preprocessor preprocessor(session_, std::move(ppOptions));
    pp_ = preprocessor.run(file.value());

    stream_ = pp::preprocessedStream(pp_);
    tree_ = trees_.parse(*stream_, 0);
    if (tree_ == nullptr) {
      return false;
    }
    const ast::OriginTable origins{&*stream_};
    lowered_ = ast::lowerFile(*tree_, session_.symbols(), origins);
    structural_ = ast::validate(lowered_.file);
    resolved_ = resolve::resolveUnit(lowered_.file, session_.symbols(), resolveOptions_);

    typed_ = sema::checkUnit(lowered_.file, resolved_.map, session_.symbols(), store_, options_);
    result_ = ir::lowerUnit(lowered_.file, resolved_.map, typed_.typed, store_, session_.symbols());
    return true;
  }

  [[nodiscard]] const ir::IRResult& result() const {
    return result_;
  }
  [[nodiscard]] const sema::SemaOutput& typed() const {
    return typed_;
  }
  [[nodiscard]] bool moduleBuilt() const {
    return result_.module.built();
  }
  // The module's textual form, which is what a test asserts a *shape* against.
  [[nodiscard]] std::string module() const {
    return ir::dumpModule(result_.module);
  }

  // --- diagnostics -----------------------------------------------------------

  [[nodiscard]] std::vector<std::string> codes() const {
    std::vector<std::string> out;
    for (const ir::IRDiagnostic& diagnostic : result_.diagnostics) {
      out.emplace_back(std::string(ir::toString(diagnostic.code)));
    }
    return out;
  }
  [[nodiscard]] bool hasError(std::string_view code) const {
    for (const std::string& value : codes()) {
      if (value == code) {
        return true;
      }
    }
    return false;
  }
  [[nodiscard]] const std::vector<ir::IRDiagnostic>& diagnostics() const {
    return result_.diagnostics;
  }
  // The number of rules in `ir.md`'s assumption list that the module violates.
  // Zero for every program this compiler accepts.
  [[nodiscard]] std::size_t violations() const {
    return ir::scanModule(result_.module).size();
  }
  [[nodiscard]] std::vector<ir::IRDiagnostic> scan() const {
    return ir::scanModule(result_.module);
  }

private:
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
  sema::TypeStore store_;
  sema::SemaOutput typed_;
  // Declared last: it borrows from everything above for the handles' lifetime.
  ir::IRResult result_;
};

} // namespace minc::test
