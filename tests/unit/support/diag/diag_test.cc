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

// The rendering side of `-ferror-limit`: the bound is on what is *shown*, and
// the sequence stops rather than skipping, because a note that survives its own
// error reads as an explanation of whatever is above it.
TEST(DiagRenderTest, TheErrorLimitStopsTheSequenceAndSaysSo) {
  DiagBag bag;
  bag.error(Span{}, "first");
  bag.note(Span{}, "a note about the first");
  bag.error(Span{}, "second");
  bag.warning(Span{}, "a warning after the stop");

  RenderOptions options;
  options.errorLimit = 1;
  DiagRenderer render(nullptr, options);

  const std::string out = render.renderAll(bag);
  EXPECT_NE(out.find("first"), std::string::npos);
  EXPECT_NE(out.find("a note about the first"), std::string::npos);
  EXPECT_EQ(out.find("second"), std::string::npos);
  EXPECT_EQ(out.find("a warning after the stop"), std::string::npos);
  // Counted, and named with the flag that would raise it.
  EXPECT_NE(out.find("2 more diagnostic(s) not shown (-ferror-limit=1)"), std::string::npos);
}

TEST(DiagRenderTest, TheDefaultLimitShowsEverythingThatWasKept) {
  DiagBag bag;
  for (int index = 0; index < 5; ++index) {
    bag.error(Span{}, "error " + std::to_string(index));
  }
  const std::string out = DiagRenderer(nullptr).renderAll(bag);
  for (int index = 0; index < 5; ++index) {
    EXPECT_NE(out.find("error " + std::to_string(index)), std::string::npos) << index;
  }
  EXPECT_EQ(out.find("not shown"), std::string::npos);
}

TEST(DiagRenderTest, TheLimitIsSpentAcrossBags) {
  // The front end clears the bag per input, so a limit carried only in the bag
  // would be a limit *per file*: ten files with one error each would print ten
  // errors under `-ferror-limit=1`. The count is in and out for that reason, and
  // every rendering that hides something says so -- including one that shows
  // nothing, which is the answer to "what happened to my second file".
  DiagRenderer render(nullptr, RenderOptions{ColorMode::Plain, 4, /*errorLimit=*/1});
  std::size_t shown = 0;

  const auto bagWith = [](const std::string& message) {
    DiagBag bag;
    bag.error(Span{}, message);
    return bag;
  };
  const std::string first = render.renderAll(bagWith("one"), shown);
  const std::string second = render.renderAll(bagWith("two"), shown);

  EXPECT_NE(first.find("one"), std::string::npos);
  // Nothing was left out of the first bag, so nothing is claimed.
  EXPECT_EQ(first.find("not shown"), std::string::npos);
  EXPECT_EQ(second.find("two"), std::string::npos);
  EXPECT_NE(second.find("1 more diagnostic(s) not shown (-ferror-limit=1)"), std::string::npos);
  EXPECT_EQ(shown, 1u);
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
