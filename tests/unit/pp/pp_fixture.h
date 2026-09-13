// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Run the preprocessor over a string and reduce the result to what a test wants
// to assert on: the *spellings* of the emitted tokens and the stable error codes.
//
// That shape is deliberate -- it is the same information `mincc pp` prints, so a
// test that reads like the tool cannot pass while the tool is wrong. The fixture
// also owns the `Session`, because every spelling is a view into source text
// that the session holds.
//
// `TempDir` exists because includes are filesystem questions: `#pragma once`, the
// missing-guard warning and an include cycle are all about file *identity*, and
// mocking the filesystem to test them would test the mock.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "pp/pp_error.h"
#include "pp/pp_record.h"
#include "pp/pp_report.h"
#include "pp/preprocessor.h"
#include "support/limits.h"
#include "support/session/session.h"
#include "support/source/source_file.h"

namespace minc::test {

struct PPOutcome {
  std::vector<std::string> spellings;
  std::vector<std::string> locations; // "file:line:col" per emitted token
  std::vector<std::string> errors;    // stable codes, in order
  std::vector<std::string> warnings;
  // The same warnings after the report's filters: a warning located in a system
  // header is dropped there, so a test needs both lists to tell "we never warned"
  // from "we warned and the header was not the user's to fix".
  std::vector<std::string> reportedWarnings;
  std::vector<std::string> messages;
  std::vector<std::string> defines;
  std::vector<std::string> includes; // "+path" read, "-path" elided by the guard

  // Spellings joined by one space: for tests about stringification (where
  // exactly one space is the rule) and about token counts.
  [[nodiscard]] std::string join() const {
    std::string out;
    for (std::size_t i = 0; i < spellings.size(); ++i) {
      if (i != 0) {
        out.push_back(' ');
      }
      out += spellings[i];
    }
    return out;
  }
  // Spellings concatenated with nothing between them: for tests about structure,
  // where a tree of parentheses does not care about the spaces.
  [[nodiscard]] std::string concat() const {
    std::string out;
    for (const std::string& spelling : spellings) {
      out += spelling;
    }
    return out;
  }
  [[nodiscard]] bool hasError(std::string_view code) const {
    return contains(errors, code);
  }
  [[nodiscard]] bool hasWarning(std::string_view code) const {
    return contains(warnings, code);
  }
  [[nodiscard]] bool hasReportedWarning(std::string_view code) const {
    return contains(reportedWarnings, code);
  }
  [[nodiscard]] bool hasDefine(std::string_view name) const {
    return contains(defines, name);
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
};

class PPFixture {
public:
  explicit PPFixture(std::string name = "test.mx") : name_(std::move(name)) {}

  PPFixture& source(std::string text) {
    source_ = std::move(text);
    return *this;
  }
  PPFixture& includeDir(std::string dir) {
    includeDirs_.push_back(std::move(dir));
    return *this;
  }
  // `-isystem`: searched after every `-I`, and the files found there are system
  // headers, which is what the warning suppression is about.
  PPFixture& systemDir(std::string dir) {
    systemDirs_.push_back(std::move(dir));
    return *this;
  }
  PPFixture& define(std::string name, std::string body = {}) {
    defines_.emplace_back(std::move(name), std::move(body));
    return *this;
  }
  PPFixture& undefine(std::string name) {
    undefines_.push_back(std::move(name));
    return *this;
  }
  PPFixture& epoch(std::int64_t seconds) {
    epoch_ = seconds;
    hasEpoch_ = true;
    return *this;
  }
  PPFixture& warnUndef(bool on = true) {
    warnUndef_ = on;
    return *this;
  }
  PPFixture& warnUnknownPragma(bool on = true) {
    warnUnknownPragma_ = on;
    return *this;
  }
  // The budgets, lowered so a test can prove a bound is a bound without
  // generating megabytes to reach the default.
  PPFixture& tokenBudget(std::size_t value) {
    tokenBudget_ = value;
    return *this;
  }
  PPFixture& byteBudget(std::size_t value) {
    byteBudget_ = value;
    return *this;
  }
  PPFixture& includeDepthBudget(std::size_t value) {
    includeDepth_ = value;
    return *this;
  }
  PPFixture& includeCountBudget(std::size_t value) {
    includesPerUnit_ = value;
    return *this;
  }
  // Turns the multiple-include optimization off, so a test can assert that
  // turning it off changes nothing -- the only way to trust it.
  PPFixture& optimizeIncludes(bool on) {
    optimizeIncludes_ = on;
    return *this;
  }

  [[nodiscard]] PPOutcome run() const {
    support::Session session;
    const auto file = session.addFile(name_, source_);
    PPOutcome outcome;
    if (!file.hasValue()) {
      outcome.errors.push_back(file.error());
      return outcome;
    }

    pp::PPOptions options;
    options.defines = defines_;
    options.undefines = undefines_;
    options.includes.quote = includeDirs_;
    options.includes.system = systemDirs_;
    options.warnUndef = warnUndef_;
    options.warnUnknownPragma = warnUnknownPragma_;
    options.optimizeIncludes = optimizeIncludes_;
    options.budgets.expandedTokens = tokenBudget_;
    options.budgets.preprocessedBytes = byteBudget_;
    options.budgets.includeDepth = includeDepth_;
    options.budgets.includesPerUnit = includesPerUnit_;
    if (hasEpoch_) {
      options.sourceDateEpoch = epoch_;
    }

    pp::Preprocessor preprocessor(session, std::move(options));
    const pp::PPResult result = preprocessor.run(file.value());

    for (const pp::PPToken& token : result.tokens) {
      // Trivia is in the preprocessed stream -- the tree builder needs it -- but
      // it is not what these tests are about: they assert on the tokens a macro
      // *produced*, and the whitespace between them is the emitter's business and
      // is covered where it belongs, by the tree over the preprocessed text.
      if (pp::isPPTrivia(token.kind)) {
        continue;
      }
      outcome.spellings.emplace_back(preprocessor.spelling(token));
      if (const support::SourceFile* source = session.sources().find(token.loc.spelling.file)) {
        const support::LineCol at = source->lookup(token.loc.spelling.offset);
        outcome.locations.push_back(source->path + ":" + std::to_string(at.line) + ":" +
                                    std::to_string(at.col));
      } else {
        outcome.locations.push_back("<synthetic>");
      }
    }
    for (const pp::PPError& error : result.errors) {
      outcome.errors.emplace_back(pp::toString(error.code));
      outcome.messages.push_back(error.message);
    }
    for (const pp::PPError& warning : result.warnings) {
      outcome.warnings.emplace_back(pp::toString(warning.code));
      outcome.messages.push_back(warning.message);
    }
    // The report step, run for real: the suppression of system-header warnings
    // lives there, so a test that only read `result.warnings` would assert the
    // opposite of what the tool does.
    {
      support::DiagBag reported;
      (void)pp::reportPPWarnings(result.warnings, preprocessor.expansions(), reported,
                                 &session.symbols(), result.systemRegions);
      for (const support::Diagnostic& diagnostic : reported.all()) {
        outcome.reportedWarnings.emplace_back(diagnostic.code);
      }
    }
    for (const pp::MacroInfo* macro : preprocessor.macros().all()) {
      if (macro->builtin == pp::BuiltinKind::None) {
        outcome.defines.emplace_back(preprocessor.symbol(macro->name));
      }
    }
    for (const pp::InclusionRecord& inclusion : preprocessor.record().includes()) {
      outcome.includes.push_back((inclusion.guardSkipped ? "-" : "+") + inclusion.path);
    }
    return outcome;
  }

  // The same input twice, once with the optimization and once without it.
  [[nodiscard]] static std::pair<PPOutcome, PPOutcome>
  withAndWithoutOptimization(const PPFixture& fixture) {
    PPFixture off = fixture;
    off.optimizeIncludes(false);
    return {fixture.run(), off.run()};
  }

private:
  std::string name_;
  std::string source_;
  std::vector<std::string> includeDirs_;
  std::vector<std::string> systemDirs_;
  std::vector<std::pair<std::string, std::string>> defines_;
  std::vector<std::string> undefines_;
  std::int64_t epoch_ = 0;
  bool hasEpoch_ = false;
  bool warnUndef_ = false;
  bool warnUnknownPragma_ = false;
  bool optimizeIncludes_ = true;
  std::size_t tokenBudget_ = support::kMaxExpandedTokens;
  std::size_t byteBudget_ = support::kMaxPreprocessedBytes;
  std::size_t includeDepth_ = support::kMaxIncludeDepth;
  std::size_t includesPerUnit_ = support::kMaxIncludesPerUnit;
};

// A directory that removes itself, so a test that needs real files cannot leak
// them into the repository or into the next run.
class TempDir {
public:
  TempDir() {
    std::error_code error;
    const std::filesystem::path base = std::filesystem::temp_directory_path(error);
    static std::uint64_t counter = 0;
    ++counter;
    // A clock reading plus a counter: unique across concurrent test binaries
    // without a platform-specific `getpid`.
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path path =
        base / ("minc-pp-test-" + std::to_string(stamp) + "-" + std::to_string(counter));
    std::filesystem::create_directories(path, error);
    path_ = path.generic_string();
  }
  ~TempDir() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::string& path() const {
    return path_;
  }
  [[nodiscard]] std::string mkdir(const std::string& name) const {
    const std::string full = path_ + "/" + name;
    std::error_code error;
    std::filesystem::create_directories(full, error);
    return full;
  }
  // Not `nodiscard`: most callers only want the side effect, and the ones that
  // want the path say so by using the result.
  std::string write(const std::string& name, std::string_view text) const {
    const std::string full = path_ + "/" + name;
    std::ofstream out(full, std::ios::binary);
    out << text;
    return full;
  }

private:
  std::string path_;
};

} // namespace minc::test
