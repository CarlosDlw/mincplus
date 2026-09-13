// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "support/diag/diag_renderer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "support/limits.h"
#include "support/source/source_file.h"
#include "support/source/source_manager.h"
#include "support/utf8/decode.h"

namespace minc::support {
namespace {

constexpr const char* kReset = "\x1b[0m";
constexpr const char* kCaretColor = "\x1b[1;32m";
constexpr const char* kTruncationMarker = "...";

[[nodiscard]] const char* severityColor(Severity severity, ColorMode mode) {
  if (mode != ColorMode::Ansi) {
    return "";
  }
  switch (severity) {
  case Severity::Error:
    return "\x1b[1;31m";
  case Severity::Warning:
    return "\x1b[1;35m";
  case Severity::Note:
    return "\x1b[1;36m";
  }
  return "";
}

[[nodiscard]] const SourceFile* fileFor(const SourceManager* sources, FileId id) {
  return sources != nullptr ? sources->find(id) : nullptr;
}

[[nodiscard]] std::uint32_t normalizeTabWidth(std::uint32_t tabWidth) {
  return tabWidth == 0 ? 1 : tabWidth;
}

// Display columns consumed by `prefix`, with tabs expanded. Counting scalars
// rather than bytes keeps the caret under the token in multi-byte source text.
[[nodiscard]] std::uint32_t displayWidth(std::string_view prefix, std::uint32_t tabWidth) {
  std::uint32_t column = 0;
  std::size_t offset = 0;
  while (offset < prefix.size()) {
    if (prefix[offset] == '\t') {
      column += tabWidth - (column % tabWidth);
      ++offset;
      continue;
    }
    const std::optional<utf8::Decoded> decoded = utf8::decodeOne(prefix, offset);
    offset += decoded.has_value() ? decoded->length : 1;
    ++column;
  }
  return column;
}

// Line text with tabs expanded and clipped to `maxColumns` display columns.
[[nodiscard]] std::string expandTabs(std::string_view line, std::uint32_t tabWidth,
                                     std::size_t maxColumns, bool& truncated) {
  std::string out;
  std::size_t column = 0;
  std::size_t offset = 0;
  while (offset < line.size()) {
    if (column >= maxColumns) {
      truncated = true;
      break;
    }
    if (line[offset] == '\t') {
      std::size_t next = column + (tabWidth - (column % tabWidth));
      if (next > maxColumns) {
        next = maxColumns; // clip the expansion instead of overshooting
      }
      out.append(next - column, ' ');
      column = next;
      ++offset;
      continue;
    }
    const std::optional<utf8::Decoded> decoded = utf8::decodeOne(line, offset);
    const std::size_t length = decoded.has_value() ? decoded->length : 1;
    out.append(line.substr(offset, length));
    ++column;
    offset += length;
  }
  return out;
}

void appendSnippet(std::ostringstream& out, const SourceFile& file, Span span,
                   const RenderOptions& options) {
  const std::uint32_t tabWidth = normalizeTabWidth(options.tabWidth);
  const LineCol pos = file.lookup(span.begin);
  const std::optional<std::string_view> line = file.lineText(pos.line);
  if (!line.has_value()) {
    return;
  }

  const std::optional<ByteRange> range = file.lines.lineRange(file.text, pos.line);
  const std::uint32_t lineBegin = range.has_value() ? range->begin : 0;
  const std::uint32_t lineEnd = lineBegin + static_cast<std::uint32_t>(line->size());

  // Clamp to this line: a multi-line span underlines its first line.
  const std::uint32_t caretBegin = std::min(std::max(span.begin, lineBegin), lineEnd);
  std::uint32_t caretEnd = std::min(span.end, lineEnd);
  if (caretEnd <= caretBegin) {
    caretEnd = caretBegin + 1; // an empty span still gets one visible caret
  }

  const std::uint32_t firstColumn = displayWidth(line->substr(0, caretBegin - lineBegin), tabWidth);
  std::uint32_t caretWidth =
      displayWidth(line->substr(0, caretEnd - lineBegin), tabWidth) - firstColumn;
  if (caretWidth == 0) {
    caretWidth = 1;
  }

  bool truncated = false;
  std::string shown = expandTabs(*line, tabWidth, kMaxRenderLineCols, truncated);
  if (truncated) {
    shown += kTruncationMarker;
  }

  const auto maxColumns = static_cast<std::uint32_t>(kMaxRenderLineCols);
  const std::uint32_t caretColumn = std::min(firstColumn, maxColumns);
  std::uint32_t width = std::min(caretWidth, maxColumns - caretColumn);
  if (width == 0) {
    width = 1;
  }

  out << "  " << shown << '\n';
  out << "  " << std::string(caretColumn, ' ');
  if (options.color == ColorMode::Ansi) {
    out << kCaretColor;
  }
  out << std::string(width, '^');
  if (options.color == ColorMode::Ansi) {
    out << kReset;
  }
  out << '\n';
}

// True when this diagnostic would print exactly what the one before it printed:
// a note anchored on the same span. Same file and same byte range, because the
// excerpt is a function of those two things -- a note one column over is a
// different caret under the same line, and that one is worth showing.
[[nodiscard]] bool repeatsSnippet(const Diagnostic& diag, const Diagnostic* previous) {
  if (previous == nullptr || diag.severity != Severity::Note) {
    return false;
  }
  return diag.span.file == previous->span.file && diag.span.begin == previous->span.begin &&
         diag.span.end == previous->span.end;
}

void appendNote(std::ostringstream& out, const SourceManager* sources, const DiagNote& note) {
  const SourceFile* file = fileFor(sources, note.span.file);
  if (file != nullptr && note.span.valid()) {
    const LineCol pos = file->lookup(note.span.begin);
    out << file->path << ':' << pos.line << ':' << pos.col << ": note: " << note.message << '\n';
  } else {
    out << "<unknown>:?:?: note: " << note.message << '\n';
  }
}

} // namespace

DiagRenderer::DiagRenderer(const SourceManager* sources, RenderOptions options)
    : sources_(sources), options_(options) {}

std::string DiagRenderer::render(const Diagnostic& diag) const {
  return render(diag, /*withSnippet=*/true);
}

std::string DiagRenderer::render(const Diagnostic& diag, bool withSnippet) const {
  std::ostringstream out;
  const SourceFile* file = fileFor(sources_, diag.span.file);
  const bool located = file != nullptr && diag.span.valid();

  if (located) {
    const LineCol pos = file->lookup(diag.span.begin);
    out << file->path << ':' << pos.line << ':' << pos.col << ": ";
  } else {
    out << "<unknown>:?:?: ";
  }

  out << severityColor(diag.severity, options_.color) << toString(diag.severity);
  if (!diag.code.empty()) {
    out << '[' << diag.code << ']';
  }
  out << ": " << diag.message;
  if (options_.color == ColorMode::Ansi) {
    out << kReset;
  }
  out << '\n';

  if (located && withSnippet) {
    appendSnippet(out, *file, diag.span, options_);
  }
  for (const DiagNote& note : diag.notes) {
    appendNote(out, sources_, note);
  }
  return out.str();
}

std::string DiagRenderer::renderAll(const DiagBag& bag) const {
  std::string out;
  const Diagnostic* previous = nullptr;
  for (const Diagnostic& diag : bag.all()) {
    out += render(diag, /*withSnippet=*/!repeatsSnippet(diag, previous));
    previous = &diag;
  }
  if (bag.droppedCount() > 0) {
    out += kTruncationMarker;
    out += ' ';
    out += std::to_string(bag.droppedCount());
    out += " more diagnostic(s) suppressed (limit ";
    out += std::to_string(kMaxDiagnostics);
    out += ")\n";
  }
  return out;
}

} // namespace minc::support
