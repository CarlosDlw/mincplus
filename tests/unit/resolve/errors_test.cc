// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "resolve/resolve.h"
#include "resolve/resolve_error.h"
#include "resolve/resolve_fixture.h"

namespace minc::test {
namespace {

TEST(ErrorsTest, UnknownNameIsOneErrorWithASuggestion) {
  ResolveFixture f;
  f.source("fn i32 add() { return 0; }\nfn i32 main() { return ad(); }\n");
  ASSERT_TRUE(f.build());

  ASSERT_EQ(f.resolved().errors.size(), 1u);
  const resolve::ResolveError& error = f.resolved().errors.front();
  EXPECT_EQ(error.code, resolve::ResolveErrorCode::UnknownName);
  EXPECT_NE(error.message.find("unknown name 'ad'"), std::string::npos);
  // The suggestion is a note pointing at the declaration the reader meant,
  // rather than a second error on the line already marked wrong.
  EXPECT_NE(error.note.find("did you mean 'add'"), std::string::npos);
  EXPECT_TRUE(error.noteSpan.valid());
  EXPECT_EQ(f.spelling(f.map().refs.back().suggestion), "add");
}

TEST(ErrorsTest, NoCascadeAroundAnUnknownName) {
  ResolveFixture f;
  f.source("fn i32 main()\n{\n  return unknown + unknown2 + 1;\n}\n");
  ASSERT_TRUE(f.build());

  // Two names, two errors -- not one per enclosing expression.
  EXPECT_EQ(f.resolved().errors.size(), 2u);
}

TEST(ErrorsTest, RedeclarationInOneScope) {
  ResolveFixture f;
  f.source("fn i32 main()\n{\n  let x = 1;\n  let x = 2;\n  return x;\n}\n");
  ASSERT_TRUE(f.build());

  EXPECT_TRUE(f.hasResolveError("resolve-redeclaration"));
  // The canonical declaration is the first one, and the second is kept as a
  // chain so the IDE can show the site without the table losing the meaning.
  EXPECT_EQ(f.defCount("x"), 2u);
  std::size_t chained = 0;
  for (const resolve::Def& def : f.map().defs) {
    if (def.nextRedundant.valid()) {
      ++chained;
    }
  }
  EXPECT_EQ(chained, 1u);
}

TEST(ErrorsTest, RepeatingAFunctionDeclarationIsNotAnError) {
  ResolveFixture f;
  f.source("fn i32 same() { return 0; }\nfn i32 same() { return 1; }\n");
  ASSERT_TRUE(f.build());

  // C compatibility: a declaration may be repeated, and a header included twice
  // declares its functions twice. The scope table keeps the canonical def and
  // the chain keeps the rest.
  EXPECT_FALSE(f.hasResolveError("resolve-redeclaration"));
  EXPECT_EQ(f.defCount("same"), 2u);
}

TEST(ErrorsTest, EveryDeclarationOfOneFunctionSharesOneIdentity) {
  // The pair `extern fn ...;` above `fn ... { }` is two *declarations* of one
  // function and must answer to one `DefId`. Every stage below keys its maps on
  // the id -- one type in `sema`, one `llvm::Function` in `ir` -- so two ids for
  // one name means two signatures and two symbols: the definition landed on
  // `f.1` and the program failed to link on "undefined reference to f".
  ResolveFixture f;
  f.source("extern fn i32 f();\n"
           "fn i32 f() { return 7; }\n"
           "fn i32 main() { return f(); }\n");
  ASSERT_TRUE(f.build());
  EXPECT_FALSE(f.hasResolveError("resolve-redeclaration"));
  ASSERT_EQ(f.defCount("f"), 2u);

  // Both file-scope items produced the *same* def, which is the answer the item
  // tree carries and the one `sema` and `ir` read.
  const resolve::DefMap& map = f.map();
  ASSERT_GE(map.itemDefs.size(), 3u);
  EXPECT_EQ(map.itemDefs[0], map.itemDefs[1]);
  EXPECT_NE(map.itemDefs[0], map.itemDefs[2]); // `main` is its own function

  // And the identity is reachable from the second site as well, which is what a
  // stage indexing *declaration sites* asks.
  std::vector<std::uint32_t> indexes;
  for (std::uint32_t i = 0; i < map.defs.size(); ++i) {
    if (f.spelling(map.defs[i].name) == "f") {
      indexes.push_back(i);
    }
  }
  ASSERT_EQ(indexes.size(), 2u);
  const resolve::DefId first{f.lowered().file(), indexes[0]};
  const resolve::DefId second{f.lowered().file(), indexes[1]};
  EXPECT_EQ(resolve::canonicalOf(map.defs[indexes[0]], first), first);
  EXPECT_EQ(resolve::canonicalOf(map.defs[indexes[1]], second), first);
  // The call in `main` counts against the canonical declaration, so "is this
  // function used" has one answer and not one per declaration.
  EXPECT_EQ(map.defs[indexes[0]].refCount, 1u);
  EXPECT_EQ(map.defs[indexes[1]].refCount, 0u);
}

TEST(ErrorsTest, ADeclarationWithNoDefinitionResolves) {
  // The declaration is the whole program's knowledge of the symbol: nothing in a
  // resolution depends on a body existing, and one that never arrives is the
  // linker's question rather than this stage's.
  ResolveFixture f;
  f.source("extern fn i32 puts(s: str);\n"
           "fn i32 main() { return puts(\"x\"); }\n");
  ASSERT_TRUE(f.build());

  EXPECT_FALSE(f.hasResolveError("resolve-redeclaration"));
  EXPECT_EQ(f.resolved().errors.size(), 0u);
  EXPECT_EQ(f.defCount("puts"), 1u);
  // `Extern` is not a linkage: a file-scope function is external either way, and
  // the declaration says *where the definition is*, which is a fact about the
  // body and not about visibility.
  ASSERT_NE(f.defNamed("puts"), nullptr);
  EXPECT_EQ(f.defNamed("puts")->linkage, resolve::Linkage::External);
}

TEST(ErrorsTest, ShadowIsAWarningAndOnlyWhenAsked) {
  const std::string source = "fn i32 main()\n{\n  let x = 1;\n  {\n    let x = 2;\n  }\n"
                             "  return 0;\n}\n";
  ResolveFixture quiet;
  quiet.source(source);
  ASSERT_TRUE(quiet.build());
  EXPECT_FALSE(quiet.hasResolveWarning("resolve-shadowed-name"));

  ResolveFixture loud;
  loud.source(source).warnShadow();
  ASSERT_TRUE(loud.build());
  EXPECT_TRUE(loud.hasResolveWarning("resolve-shadowed-name"));
}

TEST(ErrorsTest, UnusedIsAWarningAndOnlyWhenAsked) {
  const std::string source = "fn i32 main()\n{\n  let idle = 1;\n  return 0;\n}\n";
  ResolveFixture quiet;
  quiet.source(source);
  ASSERT_TRUE(quiet.build());
  EXPECT_FALSE(quiet.hasResolveWarning("resolve-unused-entity"));

  ResolveFixture loud;
  loud.source(source).warnUnused();
  ASSERT_TRUE(loud.build());
  EXPECT_TRUE(loud.hasResolveWarning("resolve-unused-entity"));
}

TEST(ErrorsTest, AnUnderscoreNameIsNotReportedAsUnused) {
  ResolveFixture f;
  f.source("fn i32 main()\n{\n  let _ignored = 1;\n  return 0;\n}\n");
  f.warnUnused();
  ASSERT_TRUE(f.build());

  // The convention every C compiler honors: a leading underscore says "I know".
  EXPECT_FALSE(f.hasResolveWarning("resolve-unused-entity"));
}

TEST(ErrorsTest, RefCountsAreWhatTheWarningsRead) {
  ResolveFixture f;
  f.source("fn i32 main()\n{\n  let used = 1;\n  return used;\n}\n");
  f.warnUnused();
  ASSERT_TRUE(f.build());

  const resolve::Def* used = f.defNamed("used");
  ASSERT_NE(used, nullptr);
  EXPECT_EQ(used->refCount, 1u);
  EXPECT_FALSE(f.hasResolveWarning("resolve-unused-entity"));
}

TEST(ErrorsTest, LimitsAreDiagnosticsAndNotAHang) {
  const std::string source = "fn i32 main()\n{\n  let a = 1;\n  return a;\n}\n";

  {
    ResolveFixture f;
    f.source(source);
    resolve::ResolveOptions options;
    options.maxDefs = 0; // lowered, never disabled: the bound has to be provable
    f.resolveOptions(options);
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasResolveError("resolve-limit-defs"));
  }
  {
    ResolveFixture f;
    f.source(source);
    resolve::ResolveOptions options;
    options.maxScopes = 1; // only the file scope: the function cannot open one
    f.resolveOptions(options);
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasResolveError("resolve-limit-scopes"));
  }
  {
    ResolveFixture f;
    f.source(source);
    resolve::ResolveOptions options;
    options.maxRefs = 0;
    f.resolveOptions(options);
    ASSERT_TRUE(f.build());
    EXPECT_TRUE(f.hasResolveError("resolve-limit-refs"));
  }
}

TEST(ErrorsTest, ALoweredScopeDepthStopsTheWalkInsteadOfLooping) {
  ResolveFixture f;
  f.source("fn i32 main()\n{\n  let outer = 1;\n  {\n    return outer;\n  }\n}\n");
  resolve::ResolveOptions options;
  options.maxScopeDepth = 1;
  f.resolveOptions(options);
  ASSERT_TRUE(f.build());

  // The walk is bounded, so a name in an outer scope is simply not found --
  // a diagnostic, not a hang.
  EXPECT_TRUE(f.hasResolveError("resolve-unknown-name"));
}

TEST(ErrorsTest, EveryCodeIsReachable) {
  std::set<std::string> reached;
  const auto collect = [&reached](const ResolveFixture& f) {
    for (const std::string& code : f.resolveErrorCodes()) {
      reached.insert(code);
    }
    for (const std::string& code : f.resolveWarningCodes()) {
      reached.insert(code);
    }
  };

  {
    ResolveFixture f;
    f.source("fn i32 main() { return away; }\n");
    ASSERT_TRUE(f.build());
    collect(f);
  }
  {
    ResolveFixture f;
    f.source("fn i32 main() { let x = 1; let x = 2; return x; }\n");
    ASSERT_TRUE(f.build());
    collect(f);
  }
  {
    ResolveFixture f;
    f.source("fn i32 main() { let idle = 1; return 0; }\n");
    f.warnUnused().warnShadow();
    ASSERT_TRUE(f.build());
    collect(f);
  }
  {
    ResolveFixture f;
    f.source("fn i32 main() { let x = 1; { let x = 2; } return 0; }\n");
    f.warnShadow();
    ASSERT_TRUE(f.build());
    collect(f);
  }
  {
    ResolveFixture f;
    f.source("fn i32 main() { return 0; }\n");
    resolve::ResolveOptions options;
    options.maxDefs = 0;
    f.resolveOptions(options);
    ASSERT_TRUE(f.build());
    collect(f);
  }
  {
    ResolveFixture f;
    f.source("fn i32 main() { return 0; }\n");
    resolve::ResolveOptions options;
    options.maxScopes = 1;
    f.resolveOptions(options);
    ASSERT_TRUE(f.build());
    collect(f);
  }
  {
    ResolveFixture f;
    // A name use is what the reference budget counts, so this input has one.
    f.source("fn i32 main() { let a = 1; return a; }\n");
    resolve::ResolveOptions options;
    options.maxRefs = 0;
    f.resolveOptions(options);
    ASSERT_TRUE(f.build());
    collect(f);
  }

  for (const resolve::ResolveErrorCode code : resolve::allResolveErrorCodes()) {
    EXPECT_TRUE(reached.count(std::string(resolve::toString(code))) != 0)
        << "no input produces " << resolve::toString(code);
  }
  EXPECT_EQ(reached.size(), resolve::allResolveErrorCodes().size());
}

} // namespace
} // namespace minc::test
