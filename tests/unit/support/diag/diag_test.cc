// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include <cstddef>
#include <string>

#include <gtest/gtest.h>

#include "support/diag/diag_bag.h"
#include "support/diag/diag_renderer.h"
#include "support/limits.h"
#include "support/source/source_manager.h"

namespace minc::support {
namespace {

// How many times `needle` occurs in `haystack`. The excerpt tests below count
// occurrences of a source line, which is how "the line was printed twice"
// becomes an assertion instead of a screenshot.
[[nodiscard]] std::size_t countOccurrences(const std::string& haystack, const std::string& needle) {
  std::size_t count = 0;
  for (std::size_t at = haystack.find(needle); at != std::string::npos;
       at = haystack.find(needle, at + needle.size())) {
    ++count;
  }
  return count;
}

TEST(SeverityTest, Strings) {
  EXPECT_STREQ(toString(Severity::Note), "note");
  EXPECT_STREQ(toString(Severity::Warning), "warning");
  EXPECT_STREQ(toString(Severity::Error), "error");
}

TEST(DiagBagTest, CountsBySeverity) {
  DiagBag bag;
  EXPECT_TRUE(bag.empty());
  bag.error(Span(0, 0, 1), "bad");
  bag.warning(Span(0, 1, 2), "meh");
  bag.note(Span(0, 2, 3), "info");
  EXPECT_EQ(bag.size(), 3u);
  EXPECT_EQ(bag.errorCount(), 1u);
  EXPECT_EQ(bag.warningCount(), 1u);
  EXPECT_TRUE(bag.hasErrors());
  bag.clear();
  EXPECT_TRUE(bag.empty());
  EXPECT_FALSE(bag.hasErrors());
  EXPECT_EQ(bag.droppedCount(), 0u);
}

TEST(DiagBagTest, CodesPreserved) {
  DiagBag bag;
  bag.error(Span(0, 0, 1), "bad", "E0001");
  ASSERT_EQ(bag.size(), 1u);
  EXPECT_EQ(bag.all()[0].code, "E0001");
}

TEST(DiagBagTest, RetentionIsCapped) {
  DiagBag bag;
  const std::size_t extra = 5;
  for (std::size_t i = 0; i < kMaxDiagnostics + extra; ++i) {
    bag.error(Span(0, 0, 1), "cascade");
  }
  EXPECT_EQ(bag.size(), kMaxDiagnostics);
  EXPECT_EQ(bag.droppedCount(), extra);
  EXPECT_EQ(bag.errorCount(), kMaxDiagnostics);
}

TEST(DiagRenderTest, FormatsLocationAndCaret) {
  SourceManager sm;
  const FileId id = sm.addFile("m.mx", "fn int main() {}").value();
  DiagBag bag;
  bag.error(Span(id, 3, 6), "oops", "E0007");
  DiagRenderer render(&sm);
  const std::string out = render.render(bag.all()[0]);
  EXPECT_NE(out.find("m.mx:1:4:"), std::string::npos);
  EXPECT_NE(out.find("error[E0007]: oops"), std::string::npos);
  EXPECT_NE(out.find("  fn int main() {}\n"), std::string::npos);
  EXPECT_NE(out.find("  ^^^\n"), std::string::npos);
}

TEST(DiagRenderTest, UnknownFilePlaceholder) {
  DiagBag bag;
  bag.error(Span{}, "lost");
  DiagRenderer render(nullptr);
  const std::string out = render.render(bag.all()[0]);
  EXPECT_NE(out.find("<unknown>:?:?: error: lost"), std::string::npos);
}

TEST(DiagRenderTest, NotesAfterMessage) {
  SourceManager sm;
  const FileId id = sm.addFile("n.mx", "int x;\nint x;").value();
  Diagnostic d;
  d.severity = Severity::Error;
  d.span = Span(id, 10, 11);
  d.message = "redefined";
  d.notes.push_back(DiagNote{Span(id, 4, 5), "first defined here"});
  DiagRenderer render(&sm);
  const std::string out = render.render(d);
  EXPECT_NE(out.find("n.mx:2:4:"), std::string::npos);
  EXPECT_NE(out.find("note: first defined here"), std::string::npos);
  EXPECT_NE(out.find("n.mx:1:5:"), std::string::npos);
}

TEST(DiagRenderTest, EmptySpanRendersOneCaret) {
  SourceManager sm;
  const FileId id = sm.addFile("e.mx", "ab").value();
  DiagBag bag;
  bag.error(Span(id, 1, 1), "here");
  DiagRenderer render(&sm);
  const std::string out = render.render(bag.all()[0]);
  EXPECT_NE(out.find("e.mx:1:2:"), std::string::npos);
  EXPECT_NE(out.find("  ^\n"), std::string::npos);
}

// The line table used to be built from a moved-from string, so every
// diagnostic claimed line 1.
TEST(DiagRenderTest, PointsAtSecondLine) {
  SourceManager sm;
  const FileId id = sm.addFile("ml.mx", "int a;\nint b;\n").value();
  DiagBag bag;
  bag.error(Span(id, 7, 10), "boom");
  DiagRenderer render(&sm);
  const std::string out = render.render(bag.all()[0]);
  EXPECT_NE(out.find("ml.mx:2:1: error: boom"), std::string::npos);
  EXPECT_NE(out.find("  int b;\n"), std::string::npos);
  EXPECT_NE(out.find("  ^^^\n"), std::string::npos);
}

TEST(DiagRenderTest, CrlfSourceKeepsCaretOnTheToken) {
  SourceManager sm;
  const FileId id = sm.addFile("w.mx", "int a;\r\nint b;\r\n").value();
  DiagBag bag;
  bag.error(Span(id, 8, 9), "boom"); // offset 8 is the 'i' of the second line
  DiagRenderer render(&sm);
  const std::string out = render.render(bag.all()[0]);
  EXPECT_NE(out.find("w.mx:2:1: error: boom"), std::string::npos);
  EXPECT_NE(out.find("  int b;\n"), std::string::npos); // no stray CR
}

TEST(DiagRenderTest, TabsAreExpandedForTheCaret) {
  SourceManager sm;
  const FileId id = sm.addFile("t.mx", "\tint x;").value();
  DiagBag bag;
  bag.error(Span(id, 1, 4), "boom");
  DiagRenderer render(&sm);
  const std::string out = render.render(bag.all()[0]);
  EXPECT_NE(out.find("  " + std::string(4, ' ') + "^^^"), std::string::npos);
}

TEST(DiagRenderTest, MultibyteTextDoesNotShiftTheCaretByBytes) {
  SourceManager sm;
  // "é" is two bytes; the caret for the following token must stay at column 2.
  const FileId id = sm.addFile("u.mx", "\xC3\xA9x").value();
  DiagBag bag;
  bag.error(Span(id, 2, 3), "boom");
  DiagRenderer render(&sm);
  const std::string out = render.render(bag.all()[0]);
  EXPECT_NE(out.find("u.mx:1:3:"), std::string::npos);
  EXPECT_NE(out.find("  " + std::string(1, ' ') + "^"), std::string::npos);
}

TEST(DiagRenderTest, LongLinesAreTruncated) {
  SourceManager sm;
  const std::string longLine(kMaxRenderLineCols + 60, 'a');
  const FileId id = sm.addFile("long.mx", longLine).value();
  DiagBag bag;
  bag.error(Span(id, 0, 1), "boom");
  DiagRenderer render(&sm);
  const std::string out = render.render(bag.all()[0]);
  EXPECT_NE(out.find(std::string(kMaxRenderLineCols, 'a') + "..."), std::string::npos);
}

TEST(DiagRenderTest, DroppedDiagnosticsAreReported) {
  SourceManager sm;
  const FileId id = sm.addFile("cap.mx", "x").value();
  DiagBag bag;
  for (std::size_t i = 0; i < kMaxDiagnostics + 2; ++i) {
    bag.error(Span(id, 0, 1), "boom");
  }
  DiagRenderer render(&sm);
  const std::string out = render.renderAll(bag);
  EXPECT_NE(out.find("2 more diagnostic(s) suppressed"), std::string::npos);
}

TEST(DiagRenderTest, PlainModeHasNoEscapeSequences) {
  SourceManager sm;
  const FileId id = sm.addFile("p.mx", "ab").value();
  DiagBag bag;
  bag.error(Span(id, 0, 1), "boom");
  DiagRenderer render(&sm);
  EXPECT_EQ(render.render(bag.all()[0]).find('\x1b'), std::string::npos);
}

TEST(DiagRenderTest, AnsiModeColorsSeverityAndCaret) {
  SourceManager sm;
  const FileId id = sm.addFile("c.mx", "ab").value();
  DiagBag bag;
  bag.error(Span(id, 0, 1), "boom");
  DiagRenderer render(&sm, RenderOptions{ColorMode::Ansi, 4});
  const std::string out = render.render(bag.all()[0]);
  EXPECT_NE(out.find("\x1b[1;31m"), std::string::npos);
  EXPECT_NE(out.find("\x1b[1;32m"), std::string::npos);
  EXPECT_NE(out.find("\x1b[0m"), std::string::npos);
}

// The bug this pins: `sema` answers an unknown type name with an error and a
// `did you mean` note on the *same* span, so the line and the carets were printed
// twice and the output read as two diagnostics for one mistake.
TEST(DiagRenderTest, NoteOnTheSameSpanDoesNotRepeatTheExcerpt) {
  SourceManager sm;
  const FileId id = sm.addFile("n.mx", "fn isb main()").value();
  DiagBag bag;
  bag.error(Span(id, 3, 6), "`isb` is not a type", "sema-unknown-type");
  bag.note(Span(id, 3, 6), "did you mean `i8`?");
  DiagRenderer render(&sm);
  const std::string out = render.renderAll(bag);
  EXPECT_NE(out.find("note: did you mean `i8`?"), std::string::npos);
  EXPECT_EQ(countOccurrences(out, "  fn isb main()\n"), 1u);
  EXPECT_EQ(countOccurrences(out, "  " + std::string(2, ' ') + "^^^\n"), 1u);
}

// The other half of the rule: a note *elsewhere* is new information, so it keeps
// its own excerpt -- the "look here instead" case.
TEST(DiagRenderTest, NoteElsewhereKeepsItsOwnExcerpt) {
  SourceManager sm;
  const FileId id = sm.addFile("d.mx", "int a;\nint b;\n").value();
  DiagBag bag;
  bag.error(Span(id, 7, 10), "redefinition of `b`");
  bag.note(Span(id, 0, 1), "previous definition of `b` is here");
  DiagRenderer render(&sm);
  const std::string out = render.renderAll(bag);
  EXPECT_NE(out.find("  int a;\n"), std::string::npos);
  EXPECT_NE(out.find("  int b;\n"), std::string::npos);
}

// A note one column over is a different caret under the same line, which is a
// different picture -- the rule is the exact span, not the line.
TEST(DiagRenderTest, NoteOnTheSameLineAtADifferentSpanKeepsItsExcerpt) {
  SourceManager sm;
  const FileId id = sm.addFile("o.mx", "abcd").value();
  DiagBag bag;
  bag.error(Span(id, 0, 1), "first");
  bag.note(Span(id, 1, 2), "second");
  DiagRenderer render(&sm);
  EXPECT_EQ(countOccurrences(render.renderAll(bag), "  abcd\n"), 2u);
}

// `render` is the single-diagnostic form: with no sequence there is nothing to
// compare against, so it always prints the excerpt.
TEST(DiagRenderTest, SingleDiagnosticAlwaysHasItsExcerpt) {
  SourceManager sm;
  const FileId id = sm.addFile("s.mx", "ab").value();
  DiagBag bag;
  bag.note(Span(id, 0, 1), "alone");
  DiagRenderer render(&sm);
  EXPECT_NE(render.render(bag.all()[0]).find("  ab\n"), std::string::npos);
}

TEST(DiagRenderTest, RenderAllConcatenates) {
  SourceManager sm;
  const FileId id = sm.addFile("a.mx", "xy").value();
  DiagBag bag;
  bag.error(Span(id, 0, 1), "first");
  bag.warning(Span(id, 1, 2), "second");
  DiagRenderer render(&sm);
  const std::string out = render.renderAll(bag);
  EXPECT_NE(out.find("first"), std::string::npos);
  EXPECT_NE(out.find("second"), std::string::npos);
}

} // namespace
} // namespace minc::support
