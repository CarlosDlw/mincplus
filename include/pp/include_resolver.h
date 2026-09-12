// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Turning `#include` into a file, exactly once.
//
// Three guarantees live here, and each one is a bug in the alternative:
//
//   * **A file is read once per compilation.** The bytes are read, validated and
//     lexed by `SourceManager` and then remembered by *identity*, so a header
//     reached through a symlink, a `..` path or a differently-cased name is one
//     file. Re-opening a header to ask whether it changed is a
//     time-of-check/time-of-use hole, so it is not done at all.
//   * **The search order is an explicit list.** Nothing is read from the
//     environment: a build that depends on `CPATH` is not reproducible, and
//     reproducibility is a property this stage is responsible for.
//   * **The multiple-include optimization only fires on the canonical pattern.**
//     Near-misses are refused, because a heuristic that silently changes meaning
//     is worse than a slow include.
//
// The resolver deliberately knows nothing about macros. It answers "which file,
// and is it already known?"; the preprocessor answers "and is its guard already
// defined?", which is the only part that needs the macro table.
//
// Design record: `docs/architectures/preprocessor.md`.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "lex/token_stream.h"
#include "pp/pp_token.h"
#include "support/expected/fallible.h"
#include "support/fs/fs.h"
#include "support/intern/sym_id.h"
#include "support/session/session.h"
#include "support/span/file_id.h"

namespace minc::pp {

// The ordered search lists, as the driver built them. The directory of the
// including file is *not* here: it is per-include, so the resolver adds it.
struct IncludeSearchLists {
  std::vector<std::string> quote;  // `-I`
  std::vector<std::string> system; // `-isystem`, and the target's built-in list
};

struct IncludeOpen {
  support::FileId file = support::kInvalidFile;
  std::string path; // as resolved, for the include graph
  std::string dir;  // directory of `path`, for the next include's `""` search
  bool isSystem = false;
  // True when the bytes were read and lexed for this inclusion, false when the
  // call was answered from the identity cache.
  bool read = false;
  support::FileIdentity identity;
};

class IncludeResolver {
public:
  IncludeResolver(support::Session& session, IncludeSearchLists lists);

  [[nodiscard]] const IncludeSearchLists& lists() const {
    return lists_;
  }

  // Resolves and opens `name`. `fromDir` is the directory of the including
  // file, used for the `""` form; `includeNext` starts the search *after* the
  // directory `fromDir` was found in. The error is a human-readable message
  // naming the file and the directories that were searched.
  [[nodiscard]] support::Fallible<IncludeOpen> open(const std::string& name, bool angle,
                                                    const std::string& fromDir, bool includeNext);

  // `#pragma once`.
  void markOnce(const support::FileIdentity& identity) {
    once_.insert(identity);
  }
  [[nodiscard]] bool isOnce(const support::FileIdentity& identity) const {
    return once_.find(identity) != once_.end();
  }

  // The canonical include-guard macro, recorded when the pattern was seen.
  void noteGuard(const support::FileIdentity& identity, support::SymId guard) {
    guards_.insert_or_assign(identity, guard);
  }
  [[nodiscard]] std::optional<support::SymId> guardOf(const support::FileIdentity& identity) const {
    const auto it = guards_.find(identity);
    return it == guards_.end() ? std::nullopt : std::optional<support::SymId>(it->second);
  }

  // How many times a file was actually read in this translation unit. Two means
  // the guard optimization did not apply, which is what the missing-guard
  // warning is about.
  void noteOpened(const support::FileIdentity& identity) {
    ++openCounts_[identity];
  }
  [[nodiscard]] std::uint32_t openCount(const support::FileIdentity& identity) const {
    const auto it = openCounts_.find(identity);
    return it == openCounts_.end() ? 0U : it->second;
  }

  [[nodiscard]] std::uint32_t filesRead() const {
    return filesRead_;
  }

  void clear();

private:
  [[nodiscard]] std::vector<std::string> searchOrder(const std::string& fromDir, bool angle,
                                                     bool includeNext) const;
  [[nodiscard]] std::optional<support::FileId> cached(const support::FileIdentity& identity) const;

  support::Session* session_;
  IncludeSearchLists lists_;
  // Identity -> FileId, so a file is read once even when reached by another
  // name. The FileId is the SourceManager's, which is where the text lives.
  std::unordered_map<support::FileIdentity, support::FileId, support::FileIdentityHash> files_;
  std::unordered_map<support::FileIdentity, support::SymId, support::FileIdentityHash> guards_;
  std::unordered_map<support::FileIdentity, std::uint32_t, support::FileIdentityHash> openCounts_;
  std::unordered_set<support::FileIdentity, support::FileIdentityHash> once_;
  std::uint32_t filesRead_ = 0;
};

// The canonical include-guard pattern, recognized from a just-lexed file:
//
//   #ifndef GUARD
//   #define GUARD
//   ... everything else ...
//   #endif      <- the last meaningful token in the file
//
// Returns the guard macro, or nothing when the file does not match exactly.
// Conservative by construction: any significant token after the closing
// `#endif`, or any nesting that does not close there, is a refusal.
[[nodiscard]] std::optional<support::SymId> sniffIncludeGuard(const lex::TokenStream& stream,
                                                              support::Interner& symbols);

} // namespace minc::pp
