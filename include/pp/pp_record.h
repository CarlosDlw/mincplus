// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// What the preprocessor did, in a form tools can read.
//
// A batch compile does not need any of this; the language server, the `pp`
// command and the tests do. It is recorded by the compiler itself rather than by
// tooling-only code, so it cannot rot: if the record is wrong, `mincc pp` is
// wrong the same day.
//
// Every span here is a *spelling* location -- a real byte range in a real file,
// never a synthesized one -- so a consumer can always point at the source that
// caused the entry.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "pp/pp_token.h"
#include "support/intern/sym_id.h"

namespace minc::pp {

enum class DirectiveKind : std::uint8_t {
  Null,   // `#` alone
  Define, // `#define`
  Undef,  // `#undef`
  Include,
  IncludeNext,
  Conditional, // any of `#if`, `#ifdef`, `#elif`, `#else`, `#endif`
  Pragma,
  Line,
  Error,   // `#error`
  Warning, // `#warning`
};

struct DirectiveRecord {
  SourceLoc span; // the whole directive, `#` through the last token before the newline
  DirectiveKind kind = DirectiveKind::Null;
  // The macro name for `#define`/`#undef`, the conditional's controlling name
  // for `#ifdef`/`#ifndef`, otherwise invalid.
  support::SymId macro = support::kInvalidSym;
  // For a conditional branch, whether this branch's tokens are being emitted.
  bool taken = false;
  // For a define, whether anything was actually stored (false for an allowed
  // identical redefinition or a rejected one).
  bool changed = false;
};

struct InclusionRecord {
  SourceLoc directive; // the `#include`
  support::FileId included = support::kInvalidFile;
  std::string path; // the path as resolved, for the include graph
  // How deep the *including* file was: 0 for the main file, so the include
  // graph can be printed as a tree without re-walking it.
  std::uint32_t depth = 0;
  bool isSystem = false;
  // True when the multiple-include optimization elided the file: the second and
  // later includes of a guarded header, and every include after `#pragma once`.
  bool guardSkipped = false;
  // True when the file was actually read and lexed for this inclusion.
  bool read = false;
};

// Marks "this entry has no produced range yet" in an `ExpansionSite` while its
// replacement list is still being scanned.
inline constexpr std::uint32_t kNoSite = 0xFFFFFFFFu;

// One macro invocation and the output range it produced. This is the answer to
// "what did this expand to?", which is otherwise re-derived by every consumer.
struct ExpansionSite {
  SourceLoc invocation;
  support::SymId macro = support::kInvalidSym;
  std::uint32_t producedBegin = 0; // range in the emitted token stream
  std::uint32_t producedEnd = kNoSite;
  ExpansionId frame = kNoExpansion;
};

// Counters a driver can print without walking the vectors. Kept next to them so
// a new counter cannot be forgotten by the writer or the reader.
struct PPStats {
  std::uint32_t directives = 0;
  std::uint32_t includeDirectives = 0;
  std::uint32_t filesRead = 0;
  std::uint32_t guardSkipped = 0;
  std::uint32_t macrosDefined = 0;
  std::uint32_t tokensEmitted = 0;
  std::uint32_t expansions = 0;
};

class PPRecord {
public:
  void addDirective(DirectiveRecord record) {
    directives_.push_back(record);
    ++stats_.directives;
  }
  // Counted before the record is moved into the vector: the counters are derived
  // from it, and a moved-from one is not a question to ask.
  void addInclude(InclusionRecord record) {
    ++stats_.includeDirectives;
    if (record.guardSkipped) {
      ++stats_.guardSkipped;
    }
    if (record.read) {
      ++stats_.filesRead;
    }
    includes_.push_back(std::move(record));
  }
  void addExpansion(ExpansionSite site) {
    expansions_.push_back(site);
    ++stats_.expansions;
  }
  // The range an invocation produced is only known once its replacement list has
  // been scanned to the end, so the entry is created on push and closed on pop.
  void closeExpansion(std::size_t index, std::uint32_t producedEnd) {
    if (index < expansions_.size()) {
      expansions_[index].producedEnd = producedEnd;
    }
  }

  [[nodiscard]] bool recording() const {
    return recording_;
  }
  void setRecording(bool on) {
    recording_ = on;
  }

  [[nodiscard]] const std::vector<DirectiveRecord>& directives() const {
    return directives_;
  }
  [[nodiscard]] const std::vector<InclusionRecord>& includes() const {
    return includes_;
  }
  [[nodiscard]] const std::vector<ExpansionSite>& expansions() const {
    return expansions_;
  }
  [[nodiscard]] const PPStats& stats() const {
    return stats_;
  }
  PPStats& stats() {
    return stats_;
  }

  void clear() {
    directives_.clear();
    includes_.clear();
    expansions_.clear();
    stats_ = PPStats{};
  }

private:
  bool recording_ = true;
  std::vector<DirectiveRecord> directives_;
  std::vector<InclusionRecord> includes_;
  std::vector<ExpansionSite> expansions_;
  PPStats stats_;
};

} // namespace minc::pp
