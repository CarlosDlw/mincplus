// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// What the rendered page must be, measured: it fits the width it was given, it is
// ASCII, it is colored only when it was asked to be, and a usage line is never
// broken.
//
// These are properties rather than snapshots on purpose. A golden file for a
// help page fails every time a description is improved, and passes every time the
// wrapping is wrong in a way the golden file was generated with.
#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "driver/command_spec.h"
#include "driver/help_render.h"
#include "support/term/terminal.h"

namespace minc::driver {
namespace {

[[nodiscard]] std::vector<std::string> linesOf(const std::string& text) {
  std::vector<std::string> lines;
  std::size_t at = 0;
  while (at < text.size()) {
    std::size_t end = text.find('\n', at);
    if (end == std::string::npos) {
      end = text.size();
    }
    lines.push_back(text.substr(at, end - at));
    at = end + 1;
  }
  return lines;
}

// A usage line is an invocation a reader copies, so it is printed as written and
// is the one thing allowed to be wider than the terminal. Everything else is
// prose and wraps.
[[nodiscard]] bool isUsageLine(const std::string& line, const std::vector<std::string>& usages) {
  for (const std::string& usage : usages) {
    if (line.find(usage) != std::string::npos) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] std::vector<std::string> usagesOf(Command command) {
  std::vector<std::string> usages;
  for (const std::string_view usage : commandSpec(command).usage) {
    usages.emplace_back(usage);
  }
  for (const std::string_view usage : programSpec().usage) {
    usages.emplace_back(usage);
  }
  return usages;
}

constexpr unsigned kWidths[] = {50, 60, 72, 80, 100, 120};

TEST(HelpRenderTest, NoLineIsWiderThanTheTerminal) {
  for (const unsigned width : kWidths) {
    const PageStyle style{width, support::ColorMode::Plain};
    const std::string overview = renderOverview(style);
    for (const std::string& line : linesOf(overview)) {
      if (isUsageLine(line, usagesOf(Command::Build))) {
        continue;
      }
      EXPECT_LE(line.size(), width) << "overview at " << width << ": " << line;
    }
    for (const CommandSpec& spec : allCommands()) {
      const std::string page = renderCommand(spec.command, style);
      for (const std::string& line : linesOf(page)) {
        if (isUsageLine(line, usagesOf(spec.command))) {
          continue;
        }
        EXPECT_LE(line.size(), width) << spec.name << " at " << width << ": " << line;
      }
    }
  }
}

TEST(HelpRenderTest, UsageLinesArePrintedAsWritten) {
  for (const CommandSpec& spec : allCommands()) {
    const std::string page = renderCommand(spec.command, PageStyle{40, support::ColorMode::Plain});
    for (const std::string_view usage : spec.usage) {
      EXPECT_NE(page.find(usage), std::string::npos)
          << spec.name << ": " << usage << " was wrapped or dropped";
    }
  }
}

TEST(HelpRenderTest, EveryPageIsAscii) {
  for (const unsigned width : kWidths) {
    const PageStyle style{width, support::ColorMode::Ansi};
    std::vector<std::string> pages{renderOverview(style)};
    for (const CommandSpec& spec : allCommands()) {
      pages.push_back(renderCommand(spec.command, style));
    }
    for (const std::string& page : pages) {
      for (const char c : page) {
        const auto byte = static_cast<unsigned char>(c);
        // Escape, newline and tab are the only bytes outside the printable range;
        // a box-drawing character or a smart quote is not, and is what this
        // catches.
        EXPECT_TRUE(c == '\n' || c == '\t' || c == 0x1B || (byte >= 0x20U && byte <= 0x7EU))
            << "non-ASCII byte " << static_cast<int>(byte);
      }
    }
  }
}

TEST(HelpRenderTest, ColourAppearsOnlyWhenItWasAskedFor) {
  const std::string plain = renderOverview(PageStyle{80, support::ColorMode::Plain});
  EXPECT_EQ(plain.find('\x1b'), std::string::npos);

  const std::string ansi = renderOverview(PageStyle{80, support::ColorMode::Ansi});
  EXPECT_NE(ansi.find('\x1b'), std::string::npos);

  // Coloring must not change a single character of the text: strip the two
  // sequences and the pages are identical, which is what makes a colored page
  // still greppable.
  std::string stripped;
  for (std::size_t index = 0; index < ansi.size(); ++index) {
    if (ansi[index] != '\x1b') {
      stripped += ansi[index];
      continue;
    }
    const std::size_t end = ansi.find('m', index);
    ASSERT_NE(end, std::string::npos);
    index = end;
  }
  EXPECT_EQ(stripped, plain);
}

TEST(HelpRenderTest, NoLineHasTrailingWhitespace) {
  for (const unsigned width : kWidths) {
    const PageStyle style{width, support::ColorMode::Plain};
    std::vector<std::string> pages{renderOverview(style)};
    for (const CommandSpec& spec : allCommands()) {
      pages.push_back(renderCommand(spec.command, style));
    }
    for (const std::string& page : pages) {
      for (const std::string& line : linesOf(page)) {
        if (line.empty()) {
          continue;
        }
        EXPECT_NE(line.back(), ' ') << "trailing space at " << width << ": [" << line << "]";
      }
    }
  }
}

TEST(HelpRenderTest, TheOverviewIsShortAndEveryCommandPageIsNot) {
  const PageStyle style{80, support::ColorMode::Plain};
  const std::string overview = renderOverview(style);
  // The overview may not grow into a wall: it holds the command list and the
  // global options and nothing that belongs to one command.
  EXPECT_LT(linesOf(overview).size(), 60u);
  for (const CommandSpec& spec : allCommands()) {
    const std::string page = renderCommand(spec.command, style);
    EXPECT_GT(page.size(), spec.summary.size()) << spec.name;
  }
}

TEST(HelpRenderTest, TheVersionBlockIsOneLineUnlessItIsVerbose) {
  const VersionFacts facts{"mincc", "0.1.0", "x86_64-pc-linux-gnu", "22.1.8"};
  const std::string shortForm = renderVersion(facts, /*verbose=*/false);
  EXPECT_EQ(linesOf(shortForm).size(), 1u);
  EXPECT_EQ(shortForm, "mincc 0.1.0\n");

  const std::string block = renderVersion(facts, /*verbose=*/true);
  const std::vector<std::string> lines = linesOf(block);
  // The first line plus one per fact: binary, host, default target, LLVM. A
  // fact added to the block without a line here is the test saying so.
  ASSERT_EQ(lines.size(), 5u);
  EXPECT_EQ(lines.front(), "mincc 0.1.0");
  // The values line up in one column: a block of key-value rows that does not
  // align is harder to read than no block at all.
  std::size_t column = std::string::npos;
  for (std::size_t index = 1; index < lines.size(); ++index) {
    const std::size_t at = lines[index].find_first_not_of(" \t", lines[index].find(':') + 1);
    ASSERT_NE(at, std::string::npos) << lines[index];
    if (column == std::string::npos) {
      column = at;
    }
    EXPECT_EQ(at, column) << lines[index];
  }
  EXPECT_NE(block.find("22.1.8"), std::string::npos);
}

} // namespace
} // namespace minc::driver
