// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "pp/include_resolver.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pp_internal.h"
#include "support/fs/fs.h"

namespace minc::pp {
namespace {

// The significant tokens of a stream, with the `#` tokens readable as such.
struct DirectiveToken {
  const lex::Token* token = nullptr;
  std::string_view spelling;
};

[[nodiscard]] bool isIdentifier(const DirectiveToken& token, std::string_view name) {
  return token.token->is(lex::TokenKind::Identifier) && token.spelling == name;
}

} // namespace

IncludeResolver::IncludeResolver(support::Session& session, IncludeSearchLists lists)
    : session_(&session), lists_(std::move(lists)) {}

void IncludeResolver::clear() {
  files_.clear();
  guards_.clear();
  openCounts_.clear();
  once_.clear();
  filesRead_ = 0;
}

std::vector<std::string> IncludeResolver::searchOrder(const std::string& fromDir, bool angle,
                                                      bool includeNext) const {
  std::vector<std::string> order;
  // The including file's own directory is searched only for the `""` form, only
  // from the file that is doing the including, and never for `#include_next`
  // (whose whole purpose is to skip it) -- which is why it is not in `lists_`.
  if (!angle && !includeNext && !fromDir.empty()) {
    order.push_back(fromDir);
  }
  for (const std::string& dir : lists_.quote) {
    order.push_back(dir);
  }
  for (const std::string& dir : lists_.system) {
    order.push_back(dir);
  }

  if (!includeNext) {
    return order;
  }

  // `#include_next` starts *after* the directory the current file came from.
  // Matching is by normalized path so `-I include` and `-I ./include` are the
  // same entry, and an unmatched directory means the whole list is searched
  // (which is what happens for the primary source file).
  const std::string wanted = support::normalizePath(fromDir);
  for (std::size_t i = 0; i < order.size(); ++i) {
    if (support::normalizePath(order[i]) == wanted) {
      order.erase(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(i) + 1);
      return order;
    }
  }
  return order;
}

std::optional<support::FileId>
IncludeResolver::cached(const support::FileIdentity& identity) const {
  const auto it = files_.find(identity);
  return it == files_.end() ? std::nullopt : std::optional<support::FileId>(it->second);
}

std::optional<IncludeResolver::Resolved> IncludeResolver::resolvePath(const std::string& name,
                                                                      bool angle,
                                                                      const std::string& fromDir,
                                                                      bool includeNext) const {
  // An absolute path in the directive names one file and no search happens; the
  // protection for `#include "/etc/passwd"` is that it is resolved, recorded and
  // bounded, not that it is forbidden.
  if (support::isAbsolutePath(name)) {
    if (support::isRegularFile(name)) {
      return Resolved{name, /*isSystem=*/false};
    }
    return std::nullopt;
  }

  const std::vector<std::string> order = searchOrder(fromDir, angle, includeNext);
  // Entries past the quote list (and the including file's own directory, which
  // the `""` form puts in front of it) are system entries.
  const std::size_t quotePrefix = (angle ? 0U : 1U) + lists_.quote.size();
  for (std::size_t i = 0; i < order.size(); ++i) {
    const std::string candidate = support::joinPath(order[i], name);
    if (support::isRegularFile(candidate)) {
      return Resolved{candidate, /*isSystem=*/i >= quotePrefix};
    }
  }
  return std::nullopt;
}

bool IncludeResolver::exists(const std::string& name, bool angle, const std::string& fromDir,
                             bool includeNext) const {
  return resolvePath(name, angle, fromDir, includeNext).has_value();
}

support::Expected<IncludeOpen, IncludeFailure> IncludeResolver::open(const std::string& name,
                                                                     bool angle,
                                                                     const std::string& fromDir,
                                                                     bool includeNext) {
  const std::optional<Resolved> found = resolvePath(name, angle, fromDir, includeNext);
  if (!found.has_value()) {
    std::string message = "include file '" + name + "' not found";
    const std::vector<std::string> order = searchOrder(fromDir, angle, includeNext);
    if (!order.empty()) {
      message += "; searched:";
      for (const std::string& dir : order) {
        message += " " + dir;
      }
    }
    return support::makeUnexpected(
        IncludeFailure{IncludeFailureKind::NotFound, std::move(message)});
  }

  const std::string& chosen = found->path;
  const support::FileIdentity identity = support::identifyFile(chosen);
  IncludeOpen result;
  result.path = chosen;
  result.dir = support::directoryOf(chosen);
  result.isSystem = found->isSystem;
  result.identity = identity;

  if (const std::optional<support::FileId> known = cached(identity)) {
    // Read once: the bytes are already in the SourceManager, so a second
    // inclusion costs a map lookup instead of a disk read.
    result.file = *known;
    return result;
  }

  support::Fallible<support::FileId> id = session_->loadFromDisk(chosen);
  if (!id) {
    // The file is there and the search list did its job; what failed is reading
    // it. Saying "not found" here would send the user to fix their include path.
    return support::makeUnexpected(
        IncludeFailure{IncludeFailureKind::Unreadable, std::move(id.error())});
  }
  result.file = id.value();
  result.read = true;
  ++filesRead_;
  files_.emplace(identity, result.file);
  return result;
}

std::optional<support::SymId> sniffIncludeGuard(const lex::TokenStream& stream,
                                                support::Interner& symbols) {
  const std::string_view text = stream.text();

  std::vector<DirectiveToken> significant;
  significant.reserve(stream.significantCount());
  for (const std::uint32_t index : stream.significantIndices()) {
    const lex::Token& token = stream.tokens()[index];
    // The stream's end-of-file token counts as significant (it is the parser's
    // stop condition), but it is not part of the file's *text*, so including it
    // would make the "is `#endif` last?" test fail for every file.
    if (token.is(lex::TokenKind::EndOfFile)) {
      continue;
    }
    significant.push_back(DirectiveToken{&token, text.substr(token.offset, token.length)});
  }

  // `#ifndef GUARD` must be the first thing in the file.
  if (significant.size() < 6) {
    return std::nullopt;
  }
  std::size_t cursor = 0;
  const auto expectHash = [&]() {
    if (cursor >= significant.size() || !detail::isHash(*significant[cursor].token)) {
      return false;
    }
    ++cursor;
    return true;
  };
  if (!expectHash() || !isIdentifier(significant[cursor], "ifndef")) {
    return std::nullopt;
  }
  ++cursor;
  if (significant[cursor].token->kind != lex::TokenKind::Identifier) {
    return std::nullopt;
  }
  const std::string_view guardName = significant[cursor].spelling;
  ++cursor;

  // `#define GUARD` must follow immediately, with the same name.
  if (!expectHash() || !isIdentifier(significant[cursor], "define")) {
    return std::nullopt;
  }
  ++cursor;
  if (significant[cursor].token->kind != lex::TokenKind::Identifier ||
      significant[cursor].spelling != guardName) {
    return std::nullopt;
  }
  ++cursor;

  // Everything else must be inside that conditional, and the `#endif` that
  // closes it must be the last significant token.
  int depth = 1;
  while (cursor < significant.size()) {
    const DirectiveToken& token = significant[cursor];
    if (!detail::isHash(*token.token)) {
      ++cursor;
      continue;
    }
    const std::size_t hashIndex = cursor;
    ++cursor;
    if (cursor >= significant.size()) {
      return std::nullopt;
    }
    if (isIdentifier(significant[cursor], "if") || isIdentifier(significant[cursor], "ifdef") ||
        isIdentifier(significant[cursor], "ifndef")) {
      ++depth;
    } else if (isIdentifier(significant[cursor], "endif")) {
      --depth;
      if (depth == 0) {
        // Only trivia may follow; a significant token means the file has
        // content outside the guard, and then the optimization is not safe.
        return cursor + 1 == significant.size()
                   ? std::optional<support::SymId>(symbols.intern(guardName))
                   : std::nullopt;
      }
    }
    (void)hashIndex;
    ++cursor;
  }
  return std::nullopt;
}

} // namespace minc::pp
