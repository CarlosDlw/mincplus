// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Every `.mx` file in `examples/` must lex cleanly.
//
// The examples are the language's de facto specification for the surface that
// exists, so a change to the lexer that breaks one of them must fail here
// rather than be noticed later by hand. This also keeps the examples honest:
// they cannot drift into syntax the lexer does not actually accept.
#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "lex/lex_report.h"
#include "lex/token_kind.h"
#include "lex/token_stream.h"
#include "support/diag/diag_bag.h"
#include "support/source/source_file.h"
#include "support/source/source_manager.h"
#include "tests/examples_dir.h"

namespace minc::lex {
namespace {

// Sorted so the test is deterministic across platforms and file systems.
std::vector<std::filesystem::path> exampleFiles() {
  std::vector<std::filesystem::path> files;
  std::error_code ec;
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(test::kExamplesDir, ec)) {
    if (entry.is_regular_file(ec) && entry.path().extension() == ".mx") {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

std::string nameOf(const std::filesystem::path& path) {
  return path.filename().string();
}

TEST(ExamplesTest, ArePresentAndComplete) {
  const std::vector<std::filesystem::path> files = exampleFiles();
  ASSERT_GE(files.size(), 5u) << "examples/ is missing files";

  // The first two are the ones the README and the roadmap point at.
  EXPECT_EQ(nameOf(files[0]), "001_main_func.mx");
  EXPECT_EQ(nameOf(files[1]), "002_variables.mx");
}

TEST(ExamplesTest, EveryExampleLexesWithoutErrors) {
  for (const std::filesystem::path& path : exampleFiles()) {
    const std::string name = nameOf(path);

    // Loaded through SourceManager, so the example is held to the same
    // boundary as real input: size limit, BOM, NUL, and UTF-8 validity.
    support::SourceManager sources;
    const support::Fallible<support::FileId> id = sources.loadFromDisk(path.string());
    ASSERT_TRUE(id.hasValue()) << name << ": " << id.error();

    const support::SourceFile* file = sources.find(id.value());
    ASSERT_NE(file, nullptr) << name;

    const TokenStream stream = TokenStream::lex(file->id, file->text);
    ASSERT_TRUE(stream.lossless()) << name;

    support::DiagBag bag;
    const std::size_t problems = reportLexErrors(stream, bag);
    EXPECT_EQ(problems, 0u) << name;
    // Report each one: a bare count would hide what went wrong.
    for (const support::Diagnostic& diagnostic : bag.all()) {
      ADD_FAILURE() << name << ": " << diagnostic.code << ": " << diagnostic.message;
    }

    // A file that is nothing but trivia would pass the checks above while
    // testing nothing, so require an actual program shape.
    bool hasFunction = false;
    for (const Token& token : stream.tokens()) {
      if (token.kind == TokenKind::KwFn) {
        hasFunction = true;
      }
    }
    EXPECT_TRUE(hasFunction) << name << " has no `fn` declaration";
    EXPECT_FALSE(stream.text().empty()) << name;
  }
}

// LF only, exactly one trailing newline, no trailing blank line. This is not
// cosmetic: a CRLF example would produce a different token stream for the same
// commit depending on who checked it out, which is the bug .gitattributes is
// there to prevent.
TEST(ExamplesTest, LineEndingsAndTrailingNewlineAreCanonical) {
  for (const std::filesystem::path& path : exampleFiles()) {
    const std::string name = nameOf(path);
    support::SourceManager sources;
    const support::Fallible<support::FileId> id = sources.loadFromDisk(path.string());
    ASSERT_TRUE(id.hasValue()) << name << ": " << id.error();
    const support::SourceFile* file = sources.find(id.value());
    ASSERT_NE(file, nullptr) << name;

    const std::string_view text = file->text;
    EXPECT_EQ(text.find('\r'), std::string_view::npos) << name << " contains a CR";
    ASSERT_FALSE(text.empty()) << name;
    EXPECT_EQ(text.back(), '\n') << name << " does not end with a newline";
    EXPECT_NE(text.size() >= 2 ? text[text.size() - 2] : '\n', '\n')
        << name << " ends with a blank line";
  }
}

// The examples exist to be pointed at, so every token the dump would show must
// also be reachable from them: this asserts the set of kinds they exercise
// covers the language surface the first version claims.
TEST(ExamplesTest, CoverTheFirstVersionSurface) {
  support::SourceManager sources;
  std::vector<bool> seen(256, false);
  std::size_t files = 0;

  for (const std::filesystem::path& path : exampleFiles()) {
    const support::Fallible<support::FileId> id = sources.loadFromDisk(path.string());
    ASSERT_TRUE(id.hasValue()) << path.string() << ": " << id.error();
    const support::SourceFile* file = sources.find(id.value());
    ASSERT_NE(file, nullptr);

    const TokenStream stream = TokenStream::lex(file->id, file->text);
    for (const Token& token : stream.tokens()) {
      const auto index = static_cast<std::size_t>(token.kind);
      if (index < seen.size()) {
        seen[index] = true;
      }
    }
    ++files;
  }
  ASSERT_GE(files, 5u);

  const std::vector<TokenKind> required{
      TokenKind::KwFn,          TokenKind::KwLet,
      TokenKind::KwConst,       TokenKind::KwReturn,
      TokenKind::Identifier,    TokenKind::IntegerLiteral,
      TokenKind::FloatLiteral,  TokenKind::CharLiteral,
      TokenKind::StringLiteral, TokenKind::LineComment,
      TokenKind::BlockComment,  TokenKind::Whitespace,
      TokenKind::Newline,       TokenKind::LParen,
      TokenKind::RParen,        TokenKind::LBrace,
      TokenKind::RBrace,        TokenKind::Semicolon,
      TokenKind::Colon, // `let x: i32` and `less ? a : b`
      TokenKind::Equal,         TokenKind::Plus,
      TokenKind::Minus,         TokenKind::Star,
      TokenKind::Slash,         TokenKind::Percent,
      TokenKind::PlusPlus,      TokenKind::MinusMinus,
      TokenKind::LessLess,      TokenKind::GreaterGreater,
      TokenKind::Amp,           TokenKind::Pipe,
      TokenKind::Caret,         TokenKind::Tilde,
      TokenKind::Less,          TokenKind::LessEqual,
      TokenKind::Greater,       TokenKind::GreaterEqual,
      TokenKind::EqualEqual,    TokenKind::BangEqual,
      TokenKind::AmpAmp,        TokenKind::PipePipe,
      TokenKind::Bang,          TokenKind::Question,
      TokenKind::PlusEqual,     TokenKind::MinusEqual,
      TokenKind::StarEqual,     TokenKind::SlashEqual,
      TokenKind::PercentEqual,  TokenKind::AmpEqual,
      TokenKind::PipeEqual,     TokenKind::CaretEqual,
      TokenKind::LessLessEqual, TokenKind::GreaterGreaterEqual,
  };

  // `Comma` is deliberately absent: nothing in the decided surface takes a
  // comma yet (no parameter lists, no aggregate initializers), so requiring it
  // would force an example to use syntax the language has not agreed on.
  for (const TokenKind kind : required) {
    EXPECT_TRUE(seen[static_cast<std::size_t>(kind)]) << "no example uses " << toString(kind);
  }
}

} // namespace
} // namespace minc::lex
