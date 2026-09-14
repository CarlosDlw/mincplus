// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "driver/help_render.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "sema/target.h"

namespace minc::driver {
namespace {

// The escape sequences, in one place: bold for a heading, and nothing else. A
// page has exactly two jobs -- be readable and be greppable -- and both are hurt
// by more decoration than this.
[[nodiscard]] std::string bold(std::string_view text, support::ColorMode color) {
  if (color == support::ColorMode::Plain || text.empty()) {
    return std::string(text);
  }
  return "\x1b[1m" + std::string(text) + "\x1b[0m";
}

[[nodiscard]] std::string spaces(unsigned count) {
  return std::string(count, ' ');
}

// Appends `text` word-wrapped, assuming the caller has already left the output
// positioned at column `indent`. A word longer than the line is not split: it is
// left over-long, because a broken path or a broken identifier is unreadable and
// one wide line is not.
void appendWrapped(std::string& out, std::string_view text, unsigned indent, unsigned width) {
  // A floor, not a target: below a couple of dozen columns there is no wrapping
  // that is readable, only one word per line. The *columns* of a table give way
  // to a narrow terminal instead (see `columnFor`), which is what keeps every
  // other line inside the width that was asked for.
  const unsigned available = std::max(width, 24U);

  unsigned column = indent;
  std::size_t at = 0;
  bool first = true;
  while (at < text.size()) {
    std::size_t end = text.find(' ', at);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    const std::string_view word = text.substr(at, end - at);
    at = end + 1;
    if (word.empty()) {
      continue;
    }
    if (first) {
      out += word;
      column = indent + static_cast<unsigned>(word.size());
      first = false;
      continue;
    }
    if (column + 1U + word.size() > available) {
      out += '\n';
      out += spaces(indent);
      out += word;
      column = indent + static_cast<unsigned>(word.size());
    } else {
      out += ' ';
      out += word;
      column += 1U + static_cast<unsigned>(word.size());
    }
  }
}

// The last line of a page: the hint that makes the rest discoverable. Wrapped at
// column zero, because a sentence that names two commands is longer than 80
// columns often enough that leaving it unwrapped was measurable.
// The width of a table's left column: the widest cell plus two, capped so a long
// name cannot take the page, and then capped again by the terminal, because a
// column that eats half the width leaves no room for the text it introduces.
[[nodiscard]] unsigned columnFor(std::size_t widest, unsigned cap, unsigned width) {
  const unsigned wanted = static_cast<unsigned>(std::min<std::size_t>(widest + 2U, cap));
  return std::min(wanted, std::max(width / 2U, 12U));
}

void appendClosing(std::string& out, std::string_view text, unsigned width) {
  appendWrapped(out, text, 0, width);
  out += '\n';
}

// A field's paragraphs: the text is split on blank lines and each paragraph is
// wrapped on its own, so a section break in the source is one in the output.
void appendParagraphs(std::string& out, std::string_view text, unsigned indent, unsigned width) {
  std::size_t at = 0;
  while (at <= text.size()) {
    std::size_t end = text.find("\n\n", at);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    const std::string_view paragraph = text.substr(at, end - at);
    if (!paragraph.empty()) {
      out += spaces(indent);
      appendWrapped(out, paragraph, indent, width);
      out += '\n';
    }
    if (end == text.size()) {
      break;
    }
    out += '\n';
    at = end + 2;
  }
}

// The left column of an option row: `-h, --help`, `-o PATH`, `--emit KIND`.
[[nodiscard]] std::string invocationOf(const OptionSpec& option) {
  std::string left;
  const bool longName = option.name.size() > 2 && option.name[1] == '-';
  if (option.shortName != '\0' && longName) {
    left += '-';
    left += option.shortName;
    left += ", ";
  }
  left += option.name;
  if (!option.valueName.empty()) {
    left += ' ';
    left += option.valueName;
  }
  return left;
}

// The description column, and the rows under it.
//
// The column is the widest invocation in the group plus two, capped: past the
// cap the descriptions start on the next line instead of being pushed off the
// right edge, which is what keeps a long option name and an 80-column terminal
// from producing a page that reads as two columns of noise.
void appendOptionGroup(std::string& out, const OptionGroup& group, unsigned width,
                       support::ColorMode color) {
  if (!group.title.empty()) {
    out += bold(group.title, color);
    out += ":\n";
  }
  std::size_t widest = 0;
  for (const OptionId id : group.ids) {
    widest = std::max(widest, invocationOf(optionById(id)).size());
  }
  constexpr unsigned kColumnCap = 28;
  const unsigned column = columnFor(widest, kColumnCap, width);

  for (const OptionId id : group.ids) {
    const OptionSpec& option = optionById(id);
    const std::string left = invocationOf(option);
    out += "  ";
    if (left.size() + 2U > column) {
      // Too wide to share a line with its own description.
      out += left;
      out += '\n';
      out += spaces(2U + column);
    } else {
      out += left;
      out += spaces(column - static_cast<unsigned>(left.size()));
    }
    // The values and the default are part of the *text*, not appended after it:
    // a suffix added to a line that `appendWrapped` has already filled to the
    // width is a line wider than the terminal, which is how a 60-column page
    // ended up with 63-column rows before this was measured.
    std::string description(option.help);
    if (!option.values.empty()) {
      description += " (";
      for (std::size_t index = 0; index < option.values.size(); ++index) {
        if (index != 0) {
          description += ", ";
        }
        description += option.values[index];
      }
      description += ')';
    }
    if (!option.defaultValue.empty()) {
      description += " [";
      description += option.defaultValue;
      description += ']';
    }
    appendWrapped(out, description, 2U + column, width);
    out += '\n';
  }
}

void appendGroups(std::string& out, std::span<const OptionGroup> groups, unsigned width,
                  support::ColorMode color) {
  bool first = true;
  for (const OptionGroup& group : groups) {
    if (!first) {
      out += '\n';
    }
    first = false;
    appendOptionGroup(out, group, width, color);
  }
}

// A two-column table of labelled facts, aligned to the widest label in it. Both
// columns wrap: the label never wraps (a wrapped label is unreadable), and the
// text always does.
void appendFacts(std::string& out, std::span<const Fact> facts, unsigned width,
                 support::ColorMode color) {
  (void)color;
  if (facts.empty()) {
    return;
  }
  std::size_t widest = 0;
  for (const Fact& fact : facts) {
    widest = std::max(widest, fact.label.size());
  }
  const unsigned column = static_cast<unsigned>(widest) + 2U;
  for (const Fact& fact : facts) {
    out += "  ";
    out += fact.label;
    out += spaces(column - static_cast<unsigned>(fact.label.size()));
    appendWrapped(out, fact.text, 2U + column, width);
    out += '\n';
  }
}

void appendList(std::string& out, std::string_view title, std::span<const std::string_view> lines,
                unsigned width, support::ColorMode color) {
  if (lines.empty()) {
    return;
  }
  out += bold(title, color);
  out += ":\n";
  for (const std::string_view line : lines) {
    out += "  ";
    appendWrapped(out, line, 2, width);
    out += '\n';
  }
}

} // namespace

std::string renderOverview(PageStyle style) {
  const ProgramSpec& program = programSpec();
  std::string out;
  // Wrapped like everything else: the tagline is 54 columns, which is over the
  // width of a terminal that is narrower than that. The escape sequences go
  // around the wrapped block, not around each line, so a pipe sees one of each.
  std::string tagline;
  appendWrapped(tagline, program.tagline, 0, style.width);
  out += bold(tagline, style.color);
  out += "\n\n";

  for (std::size_t index = 0; index < program.usage.size(); ++index) {
    out += index == 0 ? "Usage: " : "       ";
    out += program.usage[index];
    out += '\n';
  }
  out += '\n';

  appendFacts(out, program.facts, style.width, style.color);
  out += '\n';

  // The command list, from the `brief` shapes. The summary is what a reader is
  // scanning for; the invocation is there to be recognised.
  std::size_t widest = 0;
  std::vector<std::string> invocations;
  invocations.reserve(allCommands().size());
  for (const CommandSpec& spec : allCommands()) {
    invocations.emplace_back(spec.brief);
    widest = std::max(widest, invocations.back().size());
  }
  constexpr unsigned kCommandColumnCap = 30;
  const unsigned column = columnFor(widest, kCommandColumnCap, style.width);

  out += bold("Commands", style.color);
  out += ":\n";
  for (std::size_t index = 0; index < allCommands().size(); ++index) {
    const CommandSpec& spec = allCommands()[index];
    out += "  ";
    out += invocations[index];
    if (invocations[index].size() + 2U <= column) {
      out += spaces(column - static_cast<unsigned>(invocations[index].size()));
    } else {
      out += '\n';
      out += spaces(2U + column);
    }
    appendWrapped(out, spec.summary, 2U + column, style.width);
    if (!spec.implemented) {
      out += " (not implemented yet)";
    }
    out += '\n';
  }
  out += '\n';

  appendGroups(out, program.groups, style.width, style.color);
  out += '\n';

  if (!program.environment.empty()) {
    out += bold("Environment", style.color);
    out += ":\n";
    appendFacts(out, program.environment, style.width, style.color);
    out += '\n';
  }

  if (!program.exitStatus.empty()) {
    out += bold("Exit status", style.color);
    out += ":\n";
    appendFacts(out, program.exitStatus, style.width, style.color);
    out += '\n';
  }

  // The line that makes the rest discoverable, which is the whole point of the
  // overview being short. Wrapped like everything else, so a narrow terminal
  // does not turn one sentence into one line past the edge.
  appendClosing(out,
                std::string("Run '") + std::string(program.name) +
                    " help <command>' for one command's page.",
                style.width);
  return out;
}

std::string renderCommand(Command command, PageStyle style) {
  const CommandSpec& spec = commandSpec(command);
  std::string out;
  std::string summary;
  appendWrapped(summary, spec.summary, 0, style.width);
  out += bold(summary, style.color);
  out += "\n\n";

  for (std::size_t index = 0; index < spec.usage.size(); ++index) {
    out += index == 0 ? "Usage: " : "       ";
    out += spec.usage[index];
    out += '\n';
  }
  out += '\n';

  if (!spec.description.empty()) {
    appendParagraphs(out, spec.description, 0, style.width);
    out += '\n';
  }

  appendGroups(out, spec.groups, style.width, style.color);
  out += '\n';

  appendList(out, "Examples", spec.examples, style.width, style.color);
  if (!spec.examples.empty()) {
    out += '\n';
  }

  if (!spec.seeAlso.empty()) {
    std::string names;
    for (const std::string_view name : spec.seeAlso) {
      if (!names.empty()) {
        names += ", ";
      }
      names += name;
    }
    out += "See also: ";
    out += names;
    out += '\n';
    out += '\n';
  }

  appendClosing(out,
                std::string("Run '") + std::string(programSpec().name) +
                    " help' for the list of commands, or '" + std::string(programSpec().name) +
                    " help <command>' for another one.",
                style.width);
  return out;
}

std::string renderVersion(const VersionFacts& facts, bool verbose) {
  std::string out;
  out += facts.program;
  out += ' ';
  out += facts.version;
  out += '\n';
  if (!verbose) {
    return out;
  }
  // Key: value, aligned, in the shape `rustc -vV` made the convention: the four
  // facts that decide whether a bug report is reproducible.
  // One past the longest key (`default target:`) so every value starts in the
  // same column, and a longer key added later pushes only its own line out.
  constexpr std::size_t kWidth = 16;
  const auto line = [&out](std::string_view key, std::string_view value) {
    out += key;
    out += std::string(kWidth > key.size() ? kWidth - key.size() : 1U, ' ');
    out += value;
    out += '\n';
  };
  line("binary:", facts.program);
  line("host:", facts.hostTriple);
  line("default target:", sema::kDefaultTriple);
  line("LLVM:", facts.llvmVersion);
  return out;
}

} // namespace minc::driver
