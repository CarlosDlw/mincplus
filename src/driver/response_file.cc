// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "driver/response_file.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "support/expected/fallible.h"
#include "support/fs/fs.h"
#include "support/limits.h"
#include "support/source/file_io.h"

namespace minc::driver {
namespace {

// The tokenizer's rules, in one place because they are the part of this feature a
// user has to be able to predict:
//
//   * whitespace -- space, tab, newline, carriage return, form feed, vertical tab
//     -- separates words, and any run of it is one separator;
//   * `'` and `"` group a word, so a path with a space in it is writable;
//     a quote inside a word starts grouping and is not part of the text;
//   * `\` escapes the character after it **when that character is one of the
//     characters that would otherwise be special**, which is whitespace, a
//     quote, a backslash, `#` or `@`. Anywhere else it is an ordinary
//     backslash -- and that exception is the whole reason this rule is written
//     out: a backslash is a *path separator* on one of the platforms this
//     compiler must run on, and a rule that swallowed `C:\dir` would silently
//     rewrite a path. `gcc` and `clang` treat every backslash as an escape,
//     which is fine for a file that only ever holds a relative path and a trap
//     for one that does not;
//   * there are no comments. A `#` is an ordinary character, because a response
//     file names options and paths and neither is a language that has comments.
//
// An unterminated quote is an error and not a guess: the two readings (`a b` as
// one word or as two) are a whole command line apart, and a compiler that picks
// one silently is a compiler whose argument parsing nobody can predict.

[[nodiscard]] bool isSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// The characters `\` escapes. Anything else keeps both characters, so a Windows
// path survives.
[[nodiscard]] bool isEscapable(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v' || c == '\'' ||
         c == '"' || c == '\\' || c == '#' || c == '@';
}

// The words in one response file's text. `error` is filled and the result is left
// partial when a quote is not closed; the caller turns that into a sentence.
[[nodiscard]] std::vector<std::string> tokenize(std::string_view text, std::string& error) {
  std::vector<std::string> words;
  std::string word;
  bool inSingle = false;
  bool inDouble = false;
  bool started = false; // a word has begun, even if it is empty (`""`)

  for (std::size_t index = 0; index < text.size(); ++index) {
    const char c = text[index];

    if (c == '\'' && !inDouble) {
      inSingle = !inSingle;
      started = true;
      continue;
    }
    if (c == '"' && !inSingle) {
      inDouble = !inDouble;
      started = true;
      continue;
    }
    if (c == '\\' && index + 1 < text.size() && isEscapable(text[index + 1])) {
      word += text[index + 1];
      ++index;
      started = true;
      continue;
    }
    if (!inSingle && !inDouble && isSpace(c)) {
      if (started) {
        words.push_back(std::move(word));
        word.clear();
        started = false;
      }
      continue;
    }
    word += c;
    started = true;
  }

  if (inSingle || inDouble) {
    error = "unterminated quote";
    return {};
  }
  if (started) {
    words.push_back(std::move(word));
  }
  return words;
}

// Expands one file's worth of words. `chain` is the canonical path of every file
// already being expanded, which is what turns a cycle into a sentence instead of
// a depth limit being reached.
void expandFile(const std::string& path, std::string_view name, std::size_t depth,
                std::vector<std::string>& chain, std::vector<std::string>& out, std::string& error);

// One already-tokenized word: `@file` recurses, anything else is a word.
void expandWord(std::string word, std::size_t depth, std::vector<std::string>& chain,
                std::vector<std::string>& out, std::string& error) {
  if (word.empty() || word.front() != '@') {
    out.push_back(std::move(word));
    return;
  }
  if (word.size() == 1) {
    // A bare `@` names nothing. Reading it as a file called "" would produce a
    // sentence about a path nobody typed.
    error = "'@' is not a response file name";
    return;
  }
  if (depth >= support::kMaxResponseFileDepth) {
    error = "response file `" + word + "` is nested more than " +
            std::to_string(support::kMaxResponseFileDepth) + " deep";
    return;
  }
  expandFile(word.substr(1), word, depth, chain, out, error);
}

void expandFile(const std::string& path, std::string_view name, std::size_t depth,
                std::vector<std::string>& chain, std::vector<std::string>& out,
                std::string& error) {
  // The identity of the file, not the spelling: `@./a` and `@a` are the same
  // file, and a cycle check keyed on the text would miss one of them and then
  // depend on the depth limit. `canonicalPath` never throws and falls back to a
  // lexical normalization, which is the right failure here: the worst case is a
  // cycle that the depth limit catches.
  const std::string canonical = support::canonicalPath(path);
  for (const std::string& seen : chain) {
    if (seen == canonical) {
      error = "response file `" + std::string(name) + "` includes itself";
      return;
    }
  }

  const support::Fallible<std::string> text = support::readFileBytes(path);
  if (!text) {
    error = "cannot read response file `" + std::string(name) + "`: " + text.error();
    return;
  }

  std::string tokenError;
  std::vector<std::string> words = tokenize(*text, tokenError);
  if (!tokenError.empty()) {
    error = "in response file `" + std::string(name) + "`: " + tokenError;
    return;
  }

  chain.push_back(canonical);
  for (std::string& word : words) {
    expandWord(std::move(word), depth + 1, chain, out, error);
    if (!error.empty()) {
      break;
    }
  }
  chain.pop_back();
}

} // namespace

CommandLine expandResponseFiles(int argc, const char* const* argv) {
  CommandLine line;
  std::vector<std::string> chain;
  for (int index = 0; index < argc; ++index) {
    const char* raw = argv != nullptr ? argv[index] : nullptr;
    if (raw == nullptr) {
      continue;
    }
    // The program name is not the user's argument and is never a response file.
    if (index == 0) {
      line.words.emplace_back(raw);
      continue;
    }
    expandWord(std::string(raw), /*depth=*/0, chain, line.words, line.error);
    if (!line.error.empty()) {
      // The rest of the line is copied **unexpanded** rather than dropped, and
      // that is what keeps `-h` working: `mincc @missing -h` must still print the
      // page, which needs the `-h` after the file that failed to reach the
      // pre-scan. Nothing else can be done with these words -- the caller has an
      // error either way -- but a flag among them is not nothing.
      for (int rest = index; rest < argc; ++rest) {
        if (argv[rest] != nullptr) {
          line.words.emplace_back(argv[rest]);
        }
      }
      return line;
    }
  }
  return line;
}

std::vector<const char*> wordPointers(const std::vector<std::string>& words) {
  std::vector<const char*> pointers;
  pointers.reserve(words.size());
  for (const std::string& word : words) {
    pointers.push_back(word.c_str());
  }
  return pointers;
}

} // namespace minc::driver
