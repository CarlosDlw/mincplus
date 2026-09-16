// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The access record: the half of `memory.md` that the lowering must not
// re-derive. Each dereference is written down where it happens -- the obligation
// and the provenance the optimizer may assume -- so `src/ir` materialises the
// access instead of deciding what the pointer was allowed to reach.
//
// Two properties are the point of the tests, and the rest support them:
//
//   * **The tables are closed.** An `AccessKind` or `ProvenanceKind` added
//     without a name is caught by enumeration, the same rule the error-code
//     tables follow, because a value nobody can print is a value nobody can
//     diagnose.
//   * **The provenance is sound and incomplete on purpose.** It answers
//     `Object` only for an address this unit named and moved by arithmetic
//     since; everything the compiler cannot name answers `Foreign`, and that is
//     the answer that assumes nothing. A wrong `Object` would let the optimizer
//     reason about an access that may not be there.
#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

#include "sema/sema_fixture.h"
#include "sema/type_store.h"
#include "sema/typed_ast.h"

namespace minc::test {
namespace {

TEST(AccessTest, TheTablesHaveOneRowPerKind) {
  EXPECT_EQ(sema::accessKindInfos().size(), sema::allAccessKinds().size());
  for (const sema::AccessKind kind : sema::allAccessKinds()) {
    EXPECT_NE(sema::toString(kind), "unknown");
  }
  EXPECT_EQ(sema::provenanceKindInfos().size(), sema::allProvenanceKinds().size());
  for (const sema::ProvenanceKind kind : sema::allProvenanceKinds()) {
    EXPECT_NE(sema::toString(kind), "unknown");
  }
  EXPECT_EQ(sema::extentKindInfos().size(), sema::allExtentKinds().size());
  for (const sema::ExtentKind kind : sema::allExtentKinds()) {
    EXPECT_NE(sema::toString(kind), "unknown");
  }
}

// --- the extent ------------------------------------------------------------------
//
// The three answers the checked build's bounds guard is read from (`checks.md`).
// The tests are separate per base because the three bases are different *kinds*
// of object, and one test asserting "the extent is right" over all three would not
// say which one changed when it failed.

TEST(AccessTest, AnArraySubscriptRecordsTheCountItsTypeGives) {
  SemaFixture f;
  f.source("fn i32 main() { let table: [4]i32 = [1, 2, 3, 4]; let i: i32 = 1; "
           "return table[i]; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_EQ(f.accessCount(), 1u);
  const sema::AccessObligation& access = f.typed().accesses().front();
  EXPECT_EQ(access.extentKind, sema::ExtentKind::Count);
  EXPECT_EQ(access.extent, 4u);
}

TEST(AccessTest, ASliceSubscriptRecordsALengthAndNotACount) {
  // The length is a *value* -- the descriptor's `len` word -- so the record says
  // which kind of extent it is and carries no number: `Count` with a `0` would be
  // the record claiming an empty object.
  SemaFixture f;
  f.source("fn i32 at(s: []i32, i: i32) { return s[i]; }\n"
           "fn i32 main() { return 0; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_EQ(f.accessCount(), 1u);
  const sema::AccessObligation& access = f.typed().accesses().front();
  EXPECT_EQ(access.extentKind, sema::ExtentKind::Length);
  EXPECT_EQ(access.extent, 0u);
}

TEST(AccessTest, APointerSubscriptRecordsNoExtent) {
  // The object a pointer names is not in this unit, which is the case `memory.md`
  // assigns to a shadow memory: the record says so rather than inventing a number
  // (`checks.md`, *What is deliberately not here*).
  SemaFixture f;
  f.source("fn i32 at(p: *i32, i: i32) { return p[i]; }\n"
           "fn i32 main() { return 0; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_EQ(f.accessCount(), 1u);
  const sema::AccessObligation& access = f.typed().accesses().front();
  EXPECT_EQ(access.extentKind, sema::ExtentKind::Unknown);
  EXPECT_EQ(access.extent, 0u);
}

TEST(AccessTest, ADerefRecordsNoExtent) {
  SemaFixture f;
  f.source("fn i32 head(p: *i32) { return *p; }\n"
           "fn i32 main() { return 0; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_EQ(f.accessCount(), 1u);
  EXPECT_EQ(f.typed().accesses().front().extentKind, sema::ExtentKind::Unknown);
}

TEST(AccessTest, TakingAnAddressRecordsNoAccess) {
  // `&x` reads nothing, so it is not an access; only the dereference is.
  SemaFixture f;
  f.source("fn i32 main() { let x: i32 = 1; let p: *i32 = &x; return 0; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.accessCount(), 0u);
}

TEST(AccessTest, ArithmeticRecordsNoAccess) {
  SemaFixture f;
  f.source("fn i32 main() { let x: i32 = 1; let y: i32 = x + 1; return y; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_EQ(f.accessCount(), 0u);
}

TEST(AccessTest, ADerefOfAUnitObjectIsObjectProvenance) {
  // The one producer the syntactic proof can name: `&x`, moved only by
  // arithmetic since.
  SemaFixture f;
  f.source("fn i32 main() { let x: i32 = 1; let n: i32 = 0; *(&x + n) = 2; return *(&x + n); }\n");
  ASSERT_TRUE(f.build());
  ASSERT_EQ(f.accessCount(), 2u);
  for (const std::string& record : f.accessRecords()) {
    EXPECT_TRUE(record.find("object") != std::string::npos) << record;
  }
}

TEST(AccessTest, ADerefOfABindingIsForeign) {
  // `p` holds a value that came out of memory, so the compiler cannot name what
  // it points at. `Foreign` is the answer that assumes nothing, and it is the
  // deliberate cost of a proof that is sound and incomplete.
  SemaFixture f;
  f.source("fn i32 main() { let x: i32 = 1; let p: *i32 = &x; *p = 2; return *p; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_EQ(f.accessCount(), 2u);
  for (const std::string& record : f.accessRecords()) {
    EXPECT_TRUE(record.find("foreign") != std::string::npos) << record;
  }
}

TEST(AccessTest, ADerefOfAParameterIsForeign) {
  // A parameter's value is a value the caller chose, so nothing here is proved
  // about it.
  SemaFixture f;
  f.source("fn i32 f(p: *i32) { return *p; }\nfn i32 main() { return 0; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_EQ(f.accessCount(), 1u);
  EXPECT_TRUE(f.accessRecords().front().find("foreign") != std::string::npos);
}

TEST(AccessTest, TheAccessedTypeIsThePointeeAndNotThePointer) {
  // The record's type carries the access's width and alignment, so it is the
  // pointee's type; the pointer's own type would say `*i32`, which has the width
  // of an address and not of the load.
  SemaFixture f;
  f.source("fn i32 main() { let x: i32 = 1; let p: *i32 = &x; let y: i32 = *p; return y; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_EQ(f.accessCount(), 1u);
  EXPECT_TRUE(f.accessRecords().front().find("i32") != std::string::npos);
  EXPECT_TRUE(f.accessRecords().front().find("*i32") == std::string::npos);
}

TEST(AccessTest, AnIndexIsOneAccess) {
  SemaFixture f;
  f.source("fn i32 main() { let x: i32 = 1; let p: *i32 = &x; let a: i32 = p[0]; "
           "let b: i32 = p[1]; return a + b; }\n");
  ASSERT_TRUE(f.build());
  // One per `p[i]`; the pointer arithmetic inside the definition is not a second
  // access.
  EXPECT_EQ(f.accessCount(), 2u);
}

TEST(AccessTest, AStoreThroughAPointerIsRecordedAtTheStorePlace) {
  // The node the lowering stands on when it emits the store is the `*p` itself,
  // so the record is keyed on that node and the lookup is the identity.
  SemaFixture f;
  f.source("fn i32 main() { let x: i32 = 1; let p: *i32 = &x; *p = 3; return 0; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_EQ(f.accessCount(), 1u);
  const sema::AccessObligation& access = f.typed().accesses().front();
  EXPECT_EQ(f.lowered().spellingOf(access.place), "*p");
  EXPECT_EQ(sema::toString(access.kind), "ordinary");
}

TEST(AccessTest, AnyPlaceInTheRecordNamesItsOwnNode) {
  SemaFixture f;
  f.source("fn i32 main() { let x: i32 = 1; let p: *i32 = &x; let a: i32 = *p; "
           "let b: i32 = p[0]; return a + b; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_EQ(f.accessCount(), 2u);
  // The record is in the walk's order, which is source order, so the list reads
  // the way the source does without a sort.
  EXPECT_EQ(f.lowered().spellingOf(f.typed().accesses()[0].place), "*p");
  EXPECT_EQ(f.lowered().spellingOf(f.typed().accesses()[1].place), "p[0]");
}

TEST(AccessTest, EveryRecordedTypeIsOneTheIrCanMap) {
  // The same property the coercion record holds: an access whose type is
  // deferred has no width, and one whose type is the poison has no size. Neither
  // can be materialised, so neither may reach the artifact.
  SemaFixture f;
  f.source("fn i32 main() { let a: u8 = 1; let b: i64 = 2; let f: f32 = 1.0; "
           "let pa: *u8 = &a; let pb: *i64 = &b; let pf: *f32 = &f; "
           "*pa = 1; *pb = 2; *pf = 3.0; return 0; }\n");
  ASSERT_TRUE(f.build());
  ASSERT_EQ(f.accessCount(), 3u);
  for (const sema::AccessObligation& access : f.typed().accesses()) {
    EXPECT_TRUE(access.type.valid());
    EXPECT_FALSE(f.types().isDeferred(access.type));
    EXPECT_FALSE(f.types().isError(access.type));
    EXPECT_FALSE(f.types().isVoid(access.type));
  }
}

TEST(AccessTest, ARerefusedAccessIsNotRecorded) {
  // A refused access describes a program already reported and never lowered.
  // Recording it would put an obligation in the artifact for a node whose type
  // is the poison, which is exactly the entry that cannot be materialised.
  SemaFixture f;
  f.source("fn i32 main() { let x: i32 = 1; return *x; }\n");
  ASSERT_TRUE(f.build());
  EXPECT_TRUE(f.hasError("sema-deref-not-pointer"));
  EXPECT_EQ(f.accessCount(), 0u);
}

TEST(AccessTest, TheDumpShowsTheAccessRecord) {
  SemaFixture f;
  f.source("fn i32 main() { let x: i32 = 1; let p: *i32 = &x; *p = 2; return 0; }\n");
  ASSERT_TRUE(f.build());
  const std::string dump = f.dump();
  EXPECT_TRUE(dump.find("# accesses 1") != std::string::npos) << dump;
  EXPECT_TRUE(dump.find("[access ordinary foreign i32]") != std::string::npos) << dump;
}

TEST(AccessTest, TheDumpNamesTheExtentItHas) {
  // The extent is printed only when there is one, and it is the checked build's
  // reader that wants it in the dump: `a[i]` and `s[i]` are checked against
  // different things and the record is where that difference is visible.
  SemaFixture f;
  f.source("fn i32 main() { let table: [4]i32 = [1, 2, 3, 4]; let i: i32 = 1; "
           "let view: []i32 = table[..]; return table[i] + view[i]; }\n");
  ASSERT_TRUE(f.build());
  const std::string dump = f.dump();
  EXPECT_TRUE(dump.find("[access ordinary object count 4 i32]") != std::string::npos) << dump;
  EXPECT_TRUE(dump.find("[access ordinary foreign length i32]") != std::string::npos) << dump;
}

} // namespace
} // namespace minc::test
