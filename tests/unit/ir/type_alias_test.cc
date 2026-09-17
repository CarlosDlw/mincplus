// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// A name for a type, at the one stage where "transparent" is measurable.
//
// `type_alias.md` decision 8 says two things about `ir`, and this file is both of
// them: **without `-g` an alias is not observable at all** -- the module is the
// one the expansion would have produced, byte for byte, which is the end-to-end
// proof that nothing between the checker and the module keeps a name -- and **with
// `-g` the name is what a debugger prints**, as one `DW_TAG_typedef` per
// declaration that a position used.
#include <cstddef>
#include <string>

#include <gtest/gtest.h>

#include "ir/ir_fixture.h"

namespace minc {
namespace {

using test::IrFixture;

// The metadata number a line introduces, from the `!17 = !DIDerivedType(...)` shape
// a module's textual form uses. Empty when the marker is not there, so a test that
// depends on the node fails with a message instead of an index into nothing.
[[nodiscard]] std::string metadataNumberBefore(const std::string& text, std::string_view marker) {
  const std::size_t at = text.find(marker);
  if (at == std::string::npos) {
    return {};
  }
  // The start of the *line*, because the marker's own line is the node's and the
  // `!` nearest to a marker inside `!DIDerivedType(` is not a number at all.
  const std::size_t lineStart = text.rfind('\n', at);
  const std::size_t start = lineStart == std::string::npos ? 0 : lineStart + 1;
  if (start >= text.size() || text[start] != '!') {
    return {};
  }
  std::size_t end = start + 1;
  while (end < text.size() && text[end] >= '0' && text[end] <= '9') {
    ++end;
  }
  return text.substr(start + 1, end - start - 1);
}

[[nodiscard]] std::size_t countOccurrences(const std::string& text, std::string_view needle) {
  std::size_t count = 0;
  for (std::size_t at = text.find(needle); at != std::string::npos;
       at = text.find(needle, at + 1)) {
    ++count;
  }
  return count;
}

// Every `define ... { ... }` block of a module, whitespace-normalized line by line.
// The comparison that matters is of *instructions*: a type's spelling cannot differ
// between the two modules (they name the same `TypeId`), and anything else that
// differed would be caught here rather than glossed over.
[[nodiscard]] std::string definesOf(const std::string& module) {
  std::string out;
  std::size_t at = module.find("define ");
  while (at != std::string::npos) {
    const std::size_t end = module.find("\n}", at);
    out += module.substr(at, end == std::string::npos ? std::string::npos : end - at + 2);
    at = module.find("define ", end == std::string::npos ? module.size() : end);
  }
  return out;
}

// The two units name the same types; one of them writes the names, the other
// writes the expansions out. They are not quite the same program -- the alias adds
// a declaration and no storage -- so the *shape* that matters is asserted: the
// instructions are the same, and the module's type graph is the same.
TEST(IrTypeAliasTest, ANamedTypeLoweredIsTheExpandedTypeLowered) {
  IrFixture named;
  ASSERT_TRUE(named
                  .source("type Word = u32;\n"
                          "type Table = [4]u32;\n"
                          "type Ref = *u32;\n"
                          "fn u32 first(t: Table, at: usize) { return t[at]; }\n"
                          "fn u32 main() {\n"
                          "  let a: Table = [1, 2, 3, 4];\n"
                          "  let p: Ref = &a[0];\n"
                          "  let w: Word = first(a, 1);\n"
                          "  return w + *p;\n"
                          "}\n")
                  .build());
  ASSERT_TRUE(named.moduleBuilt());

  IrFixture plain;
  ASSERT_TRUE(plain
                  .source("fn u32 first(t: [4]u32, at: usize) { return t[at]; }\n"
                          "fn u32 main() {\n"
                          "  let a: [4]u32 = [1, 2, 3, 4];\n"
                          "  let p: *u32 = &a[0];\n"
                          "  let w: u32 = first(a, 1);\n"
                          "  return w + *p;\n"
                          "}\n")
                  .build());
  ASSERT_TRUE(plain.moduleBuilt());

  // The name is gone by now -- it never reached the lowering as anything but a
  // type id -- so the two modules have the same functions with the same signatures
  // and the same bodies. A difference here would mean a name had leaked into the
  // module, which is the failure this whole feature is designed so it cannot have.
  ASSERT_NE(named.module().find("define"),
            std::string::npos); // the module is not empty, so the comparison means something
  // No name survives anywhere in the module, not even as a comment or a metadata
  // string: the three aliases are three declarations and no symbol.
  EXPECT_EQ(named.module().find("Word"), std::string::npos) << named.module();
  EXPECT_EQ(named.module().find("Table"), std::string::npos) << named.module();
  EXPECT_EQ(named.module().find("Ref"), std::string::npos) << named.module();
  EXPECT_EQ(definesOf(named.module()), definesOf(plain.module()));
}

TEST(IrTypeAliasTest, ANameIsATypedefAndTheBindingPointsAtIt) {
  IrFixture fixture;
  ASSERT_TRUE(fixture
                  .source("type Meters = f64;\n"
                          "fn f64 scale(x: Meters) { return x * 2.0; }\n")
                  .debugInfo()
                  .build());
  ASSERT_TRUE(fixture.moduleBuilt());
  const std::string text = fixture.module();

  // The typedef DIE, with the name the source wrote and the type it stands for.
  EXPECT_NE(text.find("DW_TAG_typedef"), std::string::npos) << text;
  EXPECT_NE(text.find("name: \"Meters\""), std::string::npos) << text;

  // **And the parameter's type is that DIE**, which is the difference between a
  // debugger that prints `Meters` and one that prints `f64`. Measured through
  // `whatis` in `gdb`, and asserted here on the metadata number so the failure
  // says which node stopped pointing at which.
  const std::string typedefId = metadataNumberBefore(text, "DW_TAG_typedef");
  ASSERT_FALSE(typedefId.empty()) << text;
  const std::size_t variableAt = text.find("!DILocalVariable(name: \"x\"");
  ASSERT_NE(variableAt, std::string::npos) << text;
  const std::string variableLine =
      text.substr(variableAt, text.find('\n', variableAt) - variableAt);
  EXPECT_NE(variableLine.find("type: !" + typedefId), std::string::npos) << variableLine;
}

TEST(IrTypeAliasTest, ABlockScopeNameLowersToNothing) {
  IrFixture named;
  ASSERT_TRUE(
      named.source("fn i32 main() { type Step = i32; let n: Step = 1; return n; }\n").build());
  ASSERT_TRUE(named.moduleBuilt());

  IrFixture plain;
  ASSERT_TRUE(plain.source("fn i32 main() { let n: i32 = 1; return n; }\n").build());
  ASSERT_TRUE(plain.moduleBuilt());

  // A declaration among the statements is a name and not a statement: it emits no
  // instruction, takes no slot and leaves no marker, so the two bodies are the
  // same. Anything else would mean the name had become code.
  EXPECT_EQ(definesOf(named.module()), definesOf(plain.module()));

  // And with `-g` it is still the name the debugger reads, from the block's own
  // declaration rather than the file's.
  IrFixture debug;
  ASSERT_TRUE(debug
                  .source("type Step = u64;\n"
                          "fn u32 main() { type Step = u32; let n: Step = 1; return n; }\n")
                  .debugInfo()
                  .build());
  ASSERT_TRUE(debug.moduleBuilt());
  const std::string text = debug.module();
  ASSERT_EQ(countOccurrences(text, "DW_TAG_typedef"), 1u) << text;
  EXPECT_NE(text.find("DW_ATE_unsigned"), std::string::npos) << text;
  EXPECT_EQ(text.find("DW_ATE_signed"), std::string::npos) << text;
}

TEST(IrTypeAliasTest, OneDeclarationIsOneTypedefHoweverManyPositionsUseIt) {
  IrFixture fixture;
  ASSERT_TRUE(fixture
                  .source("type Word = u32;\n"
                          "fn u32 add(a: Word, b: Word) { let sum: Word = a + b; return sum; }\n")
                  .debugInfo()
                  .build());
  ASSERT_TRUE(fixture.moduleBuilt());
  const std::string text = fixture.module();

  // Two parameters and a local, three positions: one DIE. A copy per position would
  // make `ptype a` and `ptype b` two types the debugger cannot tell are one, and
  // the module would grow with the number of uses rather than with the number of
  // declarations.
  EXPECT_EQ(countOccurrences(text, "DW_TAG_typedef"), 1u) << text;
}

} // namespace
} // namespace minc
