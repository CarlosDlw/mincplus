// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The whole front end, once, for every command that runs it.
//
// `check` and `ir` run exactly the same pipeline -- load, preprocess, parse,
// lower, validate, resolve, type-check -- and differ only in what they do with
// the answer. Two copies of that sequence would be two places for the stages to
// be wired differently, which is the kind of divergence that shows up as "the
// IR command accepts a file `check` rejects". So the sequence lives here, the
// artifacts it produces live here (a token stream is a view into the
// preprocessed text, and a lowered tree's spans index it too), and a command
// reads the units out.
//
// One session, one tree store, one resolve store and one sema context for the
// invocation, exactly as `check` had it inline: the last one is the point,
// because it owns the compilation's type store, so a type decided in the first
// input is *the same* `TypeId` in the second.
#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

#include "ast/ast.h"
#include "pp/preprocessor.h"
#include "resolve/resolve.h"
#include "resolve/store.h"
#include "sema/sema.h"
#include "sema/target.h"
#include "support/intern/interner.h"
#include "support/session/session.h"
#include "support/span/file_id.h"
#include "support/term/terminal.h"
#include "syntax/store.h"

namespace minc::driver {

// `DW_AT_producer`: the compiler and its version, as one string.
//
// Here rather than at each call site because it is part of the module's identity
// and two commands must not disagree about it -- a line table that says `minc+`
// from one command and `mincc 0.1.0` from another is a bug report nobody can act
// on. A function and not a constant because the version is generated.
[[nodiscard]] std::string producerString();

// Everything the pipeline needs, in one struct, so a command builds it from its
// request rather than passing eight arguments down.
struct FrontEndOptions {
  std::vector<std::pair<std::string, std::string>> defines;
  std::vector<std::string> undefines;
  std::vector<std::string> includeDirs;
  std::vector<std::string> systemDirs;
  sema::TargetInfo target = sema::defaultTarget();
  bool warnConversion = false;
  bool warnUnused = false;
  bool warnShadow = false;
  support::ColorMode diagnosticColor = support::ColorMode::Plain;
};

// One input's artifacts. The pointers are owned by the `FrontEnd` that produced
// them and stay valid for its lifetime; a unit whose earlier stage failed has
// nulls for the stages that never ran.
struct FrontEndUnit {
  std::string path;
  support::FileId file = support::kInvalidFile;
  std::uint32_t revision = 0;
  const ast::LoweredFile* lowered = nullptr;
  const resolve::ResolveOutput* resolved = nullptr;
  const sema::SemaOutput* typed = nullptr;
  std::size_t errors = 0;
  std::size_t warnings = 0;
};

// Runner for one invocation. Construct it, call `run` with the inputs, then read
// `units()`. The diagnostics are already in the bag `run` rendered to `err`;
// what a command does with the artifacts is its own business.
class FrontEnd {
public:
  explicit FrontEnd(const FrontEndOptions& options);
  FrontEnd(const FrontEnd&) = delete;
  FrontEnd& operator=(const FrontEnd&) = delete;

  // Runs every input in order, rendering each unit's diagnostics to `err`.
  // Returns false when any unit produced an error -- including one that could
  // not be read at all.
  bool run(const std::vector<std::string>& inputs, std::ostream& err);

  [[nodiscard]] const std::vector<FrontEndUnit>& units() const {
    return units_;
  }
  // The compilation's type store, through its owning context: what a command
  // needs to spell a type or print the whole table.
  [[nodiscard]] sema::Context& sema() {
    return sema_;
  }
  // The interner the artifacts' `SymId`s come from, and the session that owns
  // the text they point into. A command that lowers (or that prints a
  // diagnostic whose span it has to resolve) needs both.
  [[nodiscard]] support::Interner& symbols() {
    return session_.symbols();
  }
  [[nodiscard]] support::Session& session() {
    return session_;
  }

private:
  FrontEndOptions options_;
  support::Session session_;
  syntax::TreeStore trees_;
  resolve::ResolveStore resolves_;
  sema::Context sema_;
  // The preprocessed units, alive for the whole invocation: a token stream is a
  // view into the preprocessed text, and a lowered tree's spans index it too.
  // A `vector` is used with `reserve`, so the views stay valid.
  std::vector<pp::PPResult> preprocessed_;
  std::vector<ast::LoweredFile> lowered_;
  std::vector<FrontEndUnit> units_;
};

} // namespace minc::driver
