// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// `@file`: the words in a file are the words on the line.
//
// The expansion is tested as *text in, words out*, because that is what it is:
// no option is interpreted here, so a test can pin the tokenizer's rules (a
// quoted path, an escaped space, a backslash that is a path separator) without
// inventing a whole command line for each one.
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "driver/response_file.h"
#include "support/limits.h"

namespace minc::driver {
namespace {

// A directory that removes itself, so a test that writes response files cannot
// leak them into the repository or into the next run.
class ScratchDir {
public:
  ScratchDir() {
    std::error_code error;
    const std::filesystem::path base = std::filesystem::temp_directory_path(error);
    std::random_device device;
    for (int attempt = 0; attempt < 64; ++attempt) {
      const std::filesystem::path candidate =
          base / ("minc-rsp-" + std::to_string(device()) + "-" + std::to_string(attempt));
      if (std::filesystem::create_directory(candidate, error)) {
        path_ = candidate;
        return;
      }
    }
  }
  ~ScratchDir() {
    if (!path_.empty()) {
      std::error_code ignored;
      (void)std::filesystem::remove_all(path_, ignored);
    }
  }
  ScratchDir(const ScratchDir&) = delete;
  ScratchDir& operator=(const ScratchDir&) = delete;

  [[nodiscard]] bool valid() const {
    return !path_.empty();
  }
  [[nodiscard]] std::string file(std::string_view name) const {
    return (path_ / name).string();
  }
  // Writes `text` and returns the path it landed on.
  [[nodiscard]] std::string write(std::string_view name, std::string_view text) const {
    const std::string full = file(name);
    std::ofstream out(full, std::ios::binary);
    out << text;
    return full;
  }

private:
  std::filesystem::path path_;
};

// Expands one argument list, with `argv[0]` supplied the way `main` supplies it.
[[nodiscard]] CommandLine expand(const std::vector<std::string>& arguments) {
  std::vector<std::string> all{"mincc"};
  all.insert(all.end(), arguments.begin(), arguments.end());
  const std::vector<const char*> pointers = wordPointers(all);
  return expandResponseFiles(static_cast<int>(pointers.size()), pointers.data());
}

// The words from index 1 on, which is what a test is ever interested in.
[[nodiscard]] std::vector<std::string> tail(const CommandLine& line) {
  return std::vector<std::string>(line.words.begin() + 1, line.words.end());
}

TEST(ResponseFileTest, TheWordsOfAFileAreTheWordsOfTheLine) {
  const ScratchDir dir;
  ASSERT_TRUE(dir.valid());
  const std::string path = dir.write("a.rsp", "-DFOO=1 check a.mx\n");

  const CommandLine line = expand({"@" + path});
  EXPECT_TRUE(line.error.empty()) << line.error;
  const std::vector<std::string> words = tail(line);
  const std::vector<std::string> expected{"-DFOO=1", "check", "a.mx"};
  EXPECT_EQ(words, expected);
  // `argv[0]` is the program name and is never a response file.
  ASSERT_FALSE(line.words.empty());
  EXPECT_EQ(line.words.front(), "mincc");
}

TEST(ResponseFileTest, WhitespaceSeparatesAndRunsOfItCountOnce) {
  const ScratchDir dir;
  ASSERT_TRUE(dir.valid());
  const std::string path = dir.write("a.rsp", "  a\tb\n\n  c \r\n d  ");

  const CommandLine line = expand({"@" + path});
  EXPECT_TRUE(line.error.empty()) << line.error;
  const std::vector<std::string> expected{"a", "b", "c", "d"};
  EXPECT_EQ(tail(line), expected);
}

TEST(ResponseFileTest, QuotesGroupAPathWithASpace) {
  const ScratchDir dir;
  ASSERT_TRUE(dir.valid());
  const std::string single = dir.write("s.rsp", "'a b' \"c d\" e\\ f");
  const std::string joined = dir.write("j.rsp", "'a b'\"c d\"");

  const CommandLine one = expand({"@" + single});
  EXPECT_TRUE(one.error.empty()) << one.error;
  const std::vector<std::string> expected{"a b", "c d", "e f"};
  EXPECT_EQ(tail(one), expected);

  // Quoting groups and nothing more: two quoted runs with no space between them
  // are one word, the way they are in a shell.
  const CommandLine two = expand({"@" + joined});
  EXPECT_TRUE(two.error.empty()) << two.error;
  const std::vector<std::string> joinedExpected{"a bc d"};
  EXPECT_EQ(tail(two), joinedExpected);
}

TEST(ResponseFileTest, ABackslashOnlyEscapesWhatWouldOtherwiseBeSpecial) {
  const ScratchDir dir;
  ASSERT_TRUE(dir.valid());
  // The rule that matters: a Windows path survives, because `\d` and `\f` are not
  // escapable and the backslash is therefore a path separator. `gcc` and `clang`
  // eat every backslash, which silently rewrites `C:\dir\file` to `C:dirfile`.
  const std::string path =
      dir.write("a.rsp", "C:\\dir\\file.mx\nC:\\a\\ b.mx\na\\@b\n#not-a-comment");

  const CommandLine line = expand({"@" + path});
  EXPECT_TRUE(line.error.empty()) << line.error;
  const std::vector<std::string> expected{
      "C:\\dir\\file.mx", // a path separator, kept
      "C:\\a b.mx",       // an escaped space, joined
      "a@b",              // an escaped `@`, so not a response file
      "#not-a-comment",   // no comments: `#` is an ordinary character
  };
  EXPECT_EQ(tail(line), expected);
}

TEST(ResponseFileTest, AnUnterminatedQuoteIsAnError) {
  const ScratchDir dir;
  ASSERT_TRUE(dir.valid());
  const std::string path = dir.write("a.rsp", "check \"a b\n");

  const CommandLine line = expand({"@" + path});
  EXPECT_NE(line.error.find("unterminated quote"), std::string::npos) << line.error;
  EXPECT_NE(line.error.find("a.rsp"), std::string::npos) << line.error;
}

TEST(ResponseFileTest, AMissingFileIsAnErrorThatNamesIt) {
  const CommandLine line = expand({"@no/such/file.rsp"});
  EXPECT_NE(line.error.find("no/such/file.rsp"), std::string::npos) << line.error;
  EXPECT_FALSE(line.error.empty());
}

TEST(ResponseFileTest, ABareAtIsNotAFileName) {
  const CommandLine line = expand({"@"});
  EXPECT_NE(line.error.find("response file name"), std::string::npos) << line.error;
}

TEST(ResponseFileTest, AnAtInTheMiddleOfAnArgumentIsNotAResponseFile) {
  const CommandLine line = expand({"a@b", "-DX=@"});
  EXPECT_TRUE(line.error.empty()) << line.error;
  const std::vector<std::string> expected{"a@b", "-DX=@"};
  EXPECT_EQ(tail(line), expected);
}

TEST(ResponseFileTest, ANestedFileIsExpandedWhereItSits) {
  const ScratchDir dir;
  ASSERT_TRUE(dir.valid());
  const std::string inner = dir.write("inner.rsp", "-DB=2");
  const std::string outer = dir.write("outer.rsp", "-DA=1\n@" + inner + "\ncheck");

  const CommandLine line = expand({"@" + outer, "a.mx"});
  EXPECT_TRUE(line.error.empty()) << line.error;
  const std::vector<std::string> expected{"-DA=1", "-DB=2", "check", "a.mx"};
  EXPECT_EQ(tail(line), expected);
}

TEST(ResponseFileTest, ACycleIsRefusedByName) {
  const ScratchDir dir;
  ASSERT_TRUE(dir.valid());
  const std::string a = dir.file("a.rsp");
  const std::string b = dir.file("b.rsp");
  // Written through the same names the expansion is given, so the cycle is the
  // one the identity check has to see.
  std::ofstream(a) << "@" << b << "\n";
  std::ofstream(b) << "@" << a << "\n";

  const CommandLine line = expand({"@" + a});
  EXPECT_NE(line.error.find("includes itself"), std::string::npos) << line.error;
}

TEST(ResponseFileTest, ADepthBeyondTheLimitIsRefused) {
  const ScratchDir dir;
  ASSERT_TRUE(dir.valid());
  // One file per level, each naming the next, deeper than the bound. The bound is
  // what makes a chain nobody noticed a diagnostic instead of a hang.
  const std::size_t levels = support::kMaxResponseFileDepth + 2;
  for (std::size_t level = 0; level < levels; ++level) {
    const std::string name = "d" + std::to_string(level) + ".rsp";
    if (level + 1 == levels) {
      std::ofstream(dir.file(name)) << "check\n";
    } else {
      std::ofstream(dir.file(name))
          << "@" << dir.file("d" + std::to_string(level + 1) + ".rsp") << "\n";
    }
  }

  const CommandLine line = expand({"@" + dir.file("d0.rsp")});
  EXPECT_NE(line.error.find("nested more than"), std::string::npos) << line.error;
}

TEST(ResponseFileTest, TheRestOfTheLineIsKeptWhenAFileCannotBeRead) {
  // `-h` after the file that failed has to survive: the driver's rule is that a
  // flag outranks a problem elsewhere on the line, and it can only honor that
  // rule if the words after the failure are still there.
  const CommandLine line = expand({"@no/such/file.rsp", "-h"});
  EXPECT_FALSE(line.error.empty());
  // The argument that failed is kept as it was written, and so is everything
  // after it: a caller with an error still wants the `-h`.
  ASSERT_EQ(line.words.size(), 3u);
  EXPECT_EQ(line.words[1], "@no/such/file.rsp");
  EXPECT_EQ(line.words[2], "-h");
}

TEST(ResponseFileTest, ANullArgumentAndANullArgvAreNotDereferenced) {
  // Some C runtimes pass a null for a program with no arguments, and a compiler
  // should not be the program that crashes on it.
  const CommandLine none = expandResponseFiles(0, nullptr);
  EXPECT_TRUE(none.words.empty());
  EXPECT_TRUE(none.error.empty());

  const char* argv[] = {"mincc", nullptr, "a.mx"};
  const CommandLine line = expandResponseFiles(3, argv);
  ASSERT_EQ(line.words.size(), 2u);
  EXPECT_EQ(line.words[1], "a.mx");
}

TEST(ResponseFileTest, WordPointersBorrowFromTheWords) {
  const std::vector<std::string> words{"mincc", "check", "a.mx"};
  const std::vector<const char*> pointers = wordPointers(words);
  ASSERT_EQ(pointers.size(), words.size());
  for (std::size_t index = 0; index < words.size(); ++index) {
    EXPECT_EQ(std::string_view(pointers[index]), words[index]);
  }
}

} // namespace
} // namespace minc::driver
