// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The table's own invariants, as tests.
//
// Every one of these is a property of the *data*, which is the point of putting
// the data in one place: the enumeration cannot drift from the rows, a row cannot
// ship undocumented, a name cannot be both a promise and a reservation, and a
// family's hole cannot point at nothing. None of them needs a program compiled to
// run, so a table typo fails here in milliseconds rather than as a wrong program
// three stages later.
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "builtins/builtin.h"
#include "tests/examples_dir.h"

namespace minc::builtins {
namespace {

TEST(BuiltinTableTest, EveryIdHasExactlyOneRowAndEveryRowOneId) {
  // The `static_assert` in the table makes the *count* a compile-time fact; this
  // is the other half -- that the ids are distinct and that the two lookups agree
  // with the walk, so no row is shadowed by an earlier duplicate.
  ASSERT_EQ(all().size(), kBuiltinIdCount);
  for (const BuiltinInfo& row : all()) {
    EXPECT_NE(row.id, BuiltinId::None);
    EXPECT_EQ(lookup(row.id), &row);
    EXPECT_EQ(lookup(row.spelling), &row);
    EXPECT_FALSE(row.spelling.empty());
  }
  // Distinct: a duplicate would make one id unreachable while `all()` still
  // reported its size, which is exactly the state a count cannot catch.
  for (std::size_t i = 0; i < all().size(); ++i) {
    for (std::size_t j = i + 1; j < all().size(); ++j) {
      EXPECT_NE(all()[i].id, all()[j].id) << all()[i].spelling;
      EXPECT_NE(all()[i].spelling, all()[j].spelling) << all()[i].spelling;
    }
  }
}

TEST(BuiltinTableTest, TheLookupsRefuseWhatIsNotThere) {
  EXPECT_EQ(lookup(BuiltinId::None), nullptr);
  EXPECT_EQ(lookup(""), nullptr);
  // Exact, and case-sensitive: the spelling is a program's identifier, and an
  // identifier that is nearly right is a different identifier.
  EXPECT_EQ(lookup("CLZ"), nullptr);
  EXPECT_EQ(lookup("clz_"), nullptr);
  EXPECT_EQ(lookup("__builtin_clz"), nullptr);
}

TEST(BuiltinTableTest, ARowAlwaysHasASentence) {
  for (const BuiltinInfo& row : all()) {
    EXPECT_FALSE(row.doc.empty()) << row.spelling;
    // A sentence, not a label: the doc line is what `mincc builtins` and the
    // reference page show in place of prose nobody wrote.
    EXPECT_GT(row.doc.size(), 12u) << row.spelling;
  }
}

TEST(BuiltinTableTest, TheSpellingClassAndTheStatusAgree) {
  // The two halves of the two-spelling design, stated as an invariant so that a
  // row cannot be *both* a name the language promises and one the program may not
  // take. A `Prelude` row is stable because every input has a defined answer --
  // which is also the reason it needs no prefix to warn about. A `Reserved` row is
  // the raw layer: named by the compiler, not promised, and refused in a user
  // declaration by `sema` and in a macro by the preprocessor.
  for (const BuiltinInfo& row : all()) {
    if (row.isPrelude()) {
      EXPECT_EQ(row.status, Status::Stable) << row.spelling;
      EXPECT_FALSE(isReservedPrefix(row.spelling)) << row.spelling;
      EXPECT_EQ(row.effect, Effect::None) << row.spelling;
    } else {
      EXPECT_EQ(row.status, Status::Internal) << row.spelling;
      EXPECT_TRUE(isReservedPrefix(row.spelling)) << row.spelling;
    }
  }
}

TEST(BuiltinTableTest, ARowWithAHoleFillsIt) {
  // `MatchArg` is "the type another argument established", so a row that uses one
  // must have an argument to establish it. Without this, a table typo would be a
  // row whose result type is decided by nothing -- and it would compile.
  for (const BuiltinInfo& row : all()) {
    for (std::size_t i = 0; i < row.signature.params.size(); ++i) {
      const BuiltinType param = row.signature.params[i];
      if (param != BuiltinType::MatchArg && param != BuiltinType::MatchArgPtr) {
        continue;
      }
      bool established = false;
      for (std::size_t j = 0; j < i; ++j) {
        const BuiltinType earlier = row.signature.params[j];
        if (earlier != BuiltinType::MatchArg && earlier != BuiltinType::MatchArgPtr) {
          established = true;
        }
      }
      EXPECT_TRUE(established) << row.spelling << " has a hole at argument " << i;
    }
  }
}

TEST(BuiltinTableTest, TheReservedPrefixIsARuleAndNotAList) {
  EXPECT_TRUE(isReservedPrefix("__builtin_trap"));
  EXPECT_TRUE(isReservedPrefix("__builtin_"));
  // The rule is the prefix *with* its separator: a name that merely begins with
  // the letters is a program's own name.
  EXPECT_FALSE(isReservedPrefix("__builtinx"));
  EXPECT_FALSE(isReservedPrefix("builtin_trap"));
  EXPECT_FALSE(isReservedPrefix("clz"));
  EXPECT_FALSE(isReservedPrefix(""));
}

TEST(BuiltinTableTest, TheSignatureRendersAsThePageShowsIt) {
  const BuiltinInfo* clz = lookup("clz");
  ASSERT_NE(clz, nullptr);
  EXPECT_EQ(signatureText(*clz), "(any-int) -> same");

  const BuiltinInfo* rotate = lookup("rotl");
  ASSERT_NE(rotate, nullptr);
  EXPECT_EQ(signatureText(*rotate), "(any-int, any-int) -> same");

  const BuiltinInfo* trap = lookup("__builtin_trap");
  ASSERT_NE(trap, nullptr);
  EXPECT_EQ(signatureText(*trap), "() -> !");
}

TEST(BuiltinTableTest, ASpanLivesInStaticStorage) {
  // The rows are read by stages that outlive any call: a `sema` checker holds a
  // pointer to a row across a whole unit, and the CLI renders the table after the
  // program is gone. A table built at run time would make both dangling.
  EXPECT_EQ(all().data(), all().data());
  EXPECT_EQ(lookup("clz"), &all().front());
}

// --- the anti-hardcode guard ------------------------------------------------
//
// A stage that matches a builtin by spelling is a stage that has a second copy of
// the table, and the copy is the one that goes stale. So the walk below, in the
// shape `ir/llvm_isolation_test.cc` established for the LLVM boundary, flags any
// file that *states* a reserved name in code.
//
// Two questions the rule has to answer, and both are answered by narrowing it
// rather than by listing exceptions:
//
//   - **a comment is not code.** Prose about the rule says `__builtin_` and must
//     be able to; a comparison or a string that spells it is a second copy of the
//     table. So a line comment is stripped before the search.
//   - **the driver is not a stage.** Its help text *names* the prefix for the
//     reader -- that is the one place a user meets it -- and the driver types and
//     lowers nothing, so a name in a sentence there cannot become a wrong answer
//     about a program.

[[nodiscard]] bool isSourceFile(const std::filesystem::path& path) {
  const std::string extension = path.extension().string();
  return extension == ".cc" || extension == ".h" || extension == ".cpp" || extension == ".hpp";
}

[[nodiscard]] bool statesThePrefix(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.good()) {
    return false;
  }
  std::string line;
  while (std::getline(in, line)) {
    const std::size_t comment = line.find("//");
    const std::string_view code = comment == std::string::npos
                                      ? std::string_view(line)
                                      : std::string_view(line).substr(0, comment);
    if (code.find("__builtin_") != std::string_view::npos) {
      return true;
    }
  }
  return false;
}

// The two trees that may state it: the module that owns the names, and the driver,
// whose help text is where a user reads about them.
[[nodiscard]] bool mayStateThePrefix(const std::string& relative) {
  return relative.rfind("builtins/", 0) == 0 || relative.rfind("driver/", 0) == 0;
}

TEST(BuiltinTableTest, OnlyTheTableStatesAReservedName) {
  const std::filesystem::path project = std::filesystem::path(test::kExamplesDir).parent_path();
  std::vector<std::string> offenders;
  std::error_code ec;
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::recursive_directory_iterator(project / "src", ec)) {
    if (!entry.is_regular_file(ec) || !isSourceFile(entry.path())) {
      continue;
    }
    const std::filesystem::path relative =
        std::filesystem::relative(entry.path(), project / "src", ec);
    if (mayStateThePrefix(relative.generic_string())) {
      continue;
    }
    if (statesThePrefix(entry.path())) {
      offenders.push_back(relative.generic_string());
    }
  }
  for (const std::string& offender : offenders) {
    ADD_FAILURE() << "a reserved builtin name outside src/builtins/: " << offender;
  }
}

// The `include/` tree as well: `builtin.h` states the rule, and a second header
// that named a builtin by spelling would put the table in two places.
TEST(BuiltinTableTest, NoHeaderOutsideTheModuleNamesABuiltin) {
  const std::filesystem::path project = std::filesystem::path(test::kExamplesDir).parent_path();
  std::vector<std::string> offenders;
  std::error_code ec;
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::recursive_directory_iterator(project / "include", ec)) {
    if (!entry.is_regular_file(ec) || !isSourceFile(entry.path())) {
      continue;
    }
    const std::filesystem::path relative =
        std::filesystem::relative(entry.path(), project / "include", ec);
    if (mayStateThePrefix(relative.generic_string())) {
      continue;
    }
    if (statesThePrefix(entry.path())) {
      offenders.push_back(relative.generic_string());
    }
  }
  for (const std::string& offender : offenders) {
    ADD_FAILURE() << "a reserved builtin name outside include/builtins/: " << offender;
  }
}

} // namespace
} // namespace minc::builtins
