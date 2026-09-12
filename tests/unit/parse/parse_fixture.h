// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Lex and parse a string, keeping everything the tree points into alive.
//
// A `SyntaxTree` is a view: its leaves are views into the source text and its
// green nodes live in an arena. The fixture owns all three in the right order,
// so a test can hold a tree without worrying about which of them to keep.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

#include "lex/token_stream.h"
#include "support/mem/arena.h"
#include "support/span/file_id.h"
#include "syntax/builder.h"
#include "syntax/dump.h"
#include "syntax/tree.h"

namespace minc::test {

class ParseFixture {
public:
  explicit ParseFixture(std::string text) : text_(std::move(text)) {
    stream_.emplace(lex::TokenStream::lex(support::kInvalidFile, text_));
    tree_ = syntax::buildSyntaxTree(arena_, *stream_, /*revision=*/0);
  }

  ParseFixture(const ParseFixture&) = delete;
  ParseFixture& operator=(const ParseFixture&) = delete;

  [[nodiscard]] bool built() const {
    return tree_.has_value();
  }
  [[nodiscard]] const syntax::SyntaxTree& tree() const {
    return *tree_;
  }
  [[nodiscard]] const lex::TokenStream& stream() const {
    return *stream_;
  }
  [[nodiscard]] std::size_t errorCount() const {
    return tree_->errors().size();
  }
  [[nodiscard]] bool hasErrors() const {
    return tree_->hasErrors();
  }
  [[nodiscard]] bool bailedOut() const {
    return tree_->stats().bailedOut;
  }
  [[nodiscard]] const std::string& source() const {
    return text_;
  }

  [[nodiscard]] std::string reconstruct() const {
    return tree_->reconstruct();
  }

  [[nodiscard]] std::string dump(bool showTrivia) const {
    syntax::DumpOptions options;
    options.showTrivia = showTrivia;
    return syntax::dumpTree(*tree_, "<test>", options);
  }

  // All the error messages, joined, so a failing assertion shows what went
  // wrong rather than just a count.
  [[nodiscard]] std::string errorMessages() const {
    std::string out;
    for (const parse::ParseError& error : tree_->errors()) {
      if (!out.empty()) {
        out += "; ";
      }
      out += error.codeName();
      out += ": ";
      out += error.message;
    }
    return out;
  }

private:
  // Declared first so it outlives the tree, whose leaves are views into it.
  std::string text_;
  std::optional<lex::TokenStream> stream_;
  support::Arena arena_;
  std::optional<syntax::SyntaxTree> tree_;
};

} // namespace minc::test
