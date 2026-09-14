// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The type model: identity, the built-ins, the target table, and the reader for
// a type position's identifier run.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "sema/target.h"
#include "sema/type.h"
#include "sema/type_store.h"
#include "sema/typespec.h"

namespace minc::sema {
namespace {

[[nodiscard]] std::vector<std::string_view> words(std::initializer_list<std::string_view> list) {
  return std::vector<std::string_view>(list);
}

TEST(TypeStoreTest, TheBuiltInsHaveTheConstantsIds) {
  TypeStore types;
  // The fixed order is what makes a dump byte-stable and lets a test name a
  // built-in instead of looking it up.
  EXPECT_EQ(types.count(), kFirstInternedType);
  EXPECT_EQ(toString(types.get(kTypeError).kind), "error");
  EXPECT_EQ(toString(types.get(kTypeVoid).kind), "void");
  EXPECT_EQ(toString(types.get(kTypeBool).kind), "bool");
  EXPECT_EQ(toString(types.get(kTypeChar).kind), "char");
  EXPECT_EQ(toString(types.get(kTypeIntLiteral).kind), "integer-literal");
  EXPECT_EQ(toString(types.get(kTypeFloatLiteral).kind), "float-literal");
  EXPECT_EQ(toString(types.get(kTypeStr).kind), "str");
  EXPECT_EQ(types.spelling(kTypeI32), "i32");
  EXPECT_EQ(types.spelling(kTypeU8), "u8");
  EXPECT_EQ(types.spelling(kTypeF80), "f80");
}

TEST(TypeStoreTest, IdentityIsStructureAndNotSpelling) {
  TypeStore types;
  // Interning is the answer to "are these the same type?". Two calls with the
  // same shape are one `TypeId`, which is what stops a header's `int` and a
  // body's `i32` from being different types.
  EXPECT_EQ(types.signedInt(32), kTypeI32);
  EXPECT_EQ(types.unsignedInt(8), kTypeU8);
  EXPECT_EQ(types.floatOf(64), kTypeF64);
  EXPECT_NE(types.signedInt(32), types.unsignedInt(32));
  EXPECT_NE(types.signedInt(32), types.signedInt(64));
}

TEST(TypeStoreTest, AFunctionTypeIsIdentifiedByItsSignature) {
  TypeStore types;
  const TypeId first = types.function(kTypeI32, {}, /*variadic=*/false);
  const TypeId second = types.function(kTypeI32, {}, /*variadic=*/false);
  const TypeId different = types.function(kTypeVoid, {}, /*variadic=*/false);
  // `f(i32)` and `f(i32, ...)` are two types: the marker is part of the
  // signature, so interning them together would give one LLVM signature to two
  // different calls.
  const TypeId varargs = types.function(kTypeI32, {}, /*variadic=*/true);
  EXPECT_TRUE(first.valid());
  EXPECT_EQ(first, second);
  EXPECT_NE(first, different);
  EXPECT_NE(first, varargs);
  EXPECT_FALSE(types.isVariadic(first));
  EXPECT_TRUE(types.isVariadic(varargs));
  EXPECT_EQ(types.spelling(varargs), "fn i32(...)");
  EXPECT_EQ(types.spelling(first), "fn i32()");
}

TEST(TypeStoreTest, SizesFollowTheTarget) {
  const std::optional<TargetInfo> sysvTarget = targetFromName(kTripleLinuxAmd64);
  ASSERT_TRUE(sysvTarget.has_value());
  TypeStore sysv{*sysvTarget};
  EXPECT_EQ(sysv.sizeOf(kTypeI64), 8u);
  EXPECT_EQ(sysv.sizeOf(kTypeF80), 16u); // 10 bytes of value in a 16-byte slot
  EXPECT_EQ(sysv.sizeOf(kTypeStr), 8u);  // a pointer
  EXPECT_EQ(sysv.sizeOf(kTypeVoid), 0u); // no object representation
  EXPECT_EQ(sysv.sizeOf(kTypeError), 0u);

  const std::optional<TargetInfo> windowsTarget = targetFromName(kTripleWindowsAmd64);
  ASSERT_TRUE(windowsTarget.has_value());
  TypeStore windows{*windowsTarget};
  // Same widths for these two -- the difference is the *spelling* `long`, which
  // the specifier reader resolves, not the types themselves.
  EXPECT_EQ(windows.sizeOf(kTypeI64), 8u);
  EXPECT_EQ(windows.target().longBits, 32u);
}

TEST(TypeSpecTest, ThePrimitivesAreWholeTypes) {
  TypeStore types;
  EXPECT_EQ(readTypeSpec(words({"i32"}), types).type, kTypeI32);
  EXPECT_EQ(readTypeSpec(words({"u128"}), types).type, kTypeU128);
  EXPECT_EQ(readTypeSpec(words({"f80"}), types).type, kTypeF80);
  EXPECT_EQ(readTypeSpec(words({"bool"}), types).type, kTypeBool);
  EXPECT_EQ(readTypeSpec(words({"void"}), types).type, kTypeVoid);
  EXPECT_EQ(readTypeSpec(words({"char"}), types).type, kTypeChar);
  EXPECT_EQ(readTypeSpec(words({"str"}), types).type, kTypeStr);
}

TEST(TypeSpecTest, APrimitiveCannotBeCombined) {
  TypeStore types;
  const TypeSpecResult combined = readTypeSpec(words({"i32", "int"}), types);
  EXPECT_FALSE(combined.ok);
  // Nothing to suggest: both words are understood, the *combination* is wrong.
  EXPECT_TRUE(combined.unknownWord.empty());
  EXPECT_NE(combined.message.find("cannot be combined"), std::string::npos);
}

TEST(TypeSpecTest, PointerSizedSpellingsResolveToThePointerType) {
  TypeStore types;
  // Not types of their own: `isize` on LP64 *is* `i64`, or the identity the
  // store is built on would have two ids for one type.
  EXPECT_EQ(readTypeSpec(words({"isize"}), types).type, kTypeI64);
  EXPECT_EQ(readTypeSpec(words({"usize"}), types).type, kTypeU64);
  EXPECT_EQ(readTypeSpec(words({"size_t"}), types).type, kTypeU64);
  EXPECT_EQ(readTypeSpec(words({"ptrdiff_t"}), types).type, kTypeI64);
}

TEST(TypeSpecTest, TheCWidthsComeFromTheTargetAndNotTheHost) {
  const std::optional<TargetInfo> sysvTarget = targetFromName(kTripleLinuxAmd64);
  ASSERT_TRUE(sysvTarget.has_value());
  TypeStore sysv{*sysvTarget};
  EXPECT_EQ(readTypeSpec(words({"long"}), sysv).type, kTypeI64);
  EXPECT_EQ(readTypeSpec(words({"long", "long"}), sysv).type, kTypeI64);
  EXPECT_EQ(readTypeSpec(words({"int"}), sysv).type, kTypeI32);
  EXPECT_EQ(readTypeSpec(words({"short", "int"}), sysv).type, kTypeI16);
  EXPECT_EQ(readTypeSpec(words({"short"}), sysv).type, kTypeI16);
  EXPECT_EQ(readTypeSpec(words({"unsigned", "long", "long", "int"}), sysv).type, kTypeU64);
  EXPECT_EQ(readTypeSpec(words({"long", "int"}), sysv).type, kTypeI64);
  EXPECT_EQ(readTypeSpec(words({"unsigned"}), sysv).type, kTypeU32);
  EXPECT_EQ(readTypeSpec(words({"long", "double"}), sysv).type, kTypeF80);

  const std::optional<TargetInfo> windowsTarget = targetFromName(kTripleWindowsAmd64);
  ASSERT_TRUE(windowsTarget.has_value());
  TypeStore windows{*windowsTarget};
  // LLP64: `long` is 32 bits even though a pointer is 64.
  EXPECT_EQ(readTypeSpec(words({"long"}), windows).type, kTypeI32);
  EXPECT_EQ(readTypeSpec(words({"unsigned", "long"}), windows).type, kTypeU32);
  EXPECT_EQ(readTypeSpec(words({"long", "long"}), windows).type, kTypeI64);
  // And `long double` is what MSVC calls `double`.
  EXPECT_EQ(readTypeSpec(words({"long", "double"}), windows).type, kTypeF64);
}

TEST(TypeSpecTest, CharTakesSignednessBecauseTheLanguageDoesNot) {
  TypeStore types;
  // `char` alone is the `.mx` unsigned byte; the C escape hatches are `i8`/`u8`.
  EXPECT_EQ(readTypeSpec(words({"signed", "char"}), types).type, kTypeI8);
  EXPECT_EQ(readTypeSpec(words({"unsigned", "char"}), types).type, kTypeU8);
  EXPECT_EQ(readTypeSpec(words({"char"}), types).type, kTypeChar);
}

TEST(TypeSpecTest, TheShorthandsExpandAndCarryTheTargetRule) {
  const std::optional<TargetInfo> sysvTarget = targetFromName(kTripleLinuxAmd64);
  ASSERT_TRUE(sysvTarget.has_value());
  TypeStore sysv{*sysvTarget};
  EXPECT_EQ(readTypeSpec(words({"uint"}), sysv).type, kTypeU32);
  EXPECT_EQ(readTypeSpec(words({"__int128"}), sysv).type, kTypeI128);
  EXPECT_EQ(readTypeSpec(words({"unsigned", "__int128"}), sysv).type, kTypeU128);
  // A shorthand is expanded, not special-cased, so it earns the same refusal a
  // spelled-out type does.
  const TypeSpecResult bad = readTypeSpec(words({"uint", "i32"}), sysv);
  EXPECT_FALSE(bad.ok);
}

TEST(TypeSpecTest, EveryRejectionHasASentence) {
  TypeStore types;
  struct Case {
    std::vector<std::string_view> words;
    std::string_view fragment;
    bool unknown;
  };
  const std::vector<Case> cases = {
      {{"signed", "unsigned"}, "cannot both appear", true},
      {{"short", "long"}, "cannot both appear", false},
      {{"long", "long", "long"}, "three times", true},
      {{"long", "float"}, "cannot apply to", false},
      {{"short", "double"}, "cannot apply to", false},
      {{"unsigned", "float"}, "cannot apply to", false},
      {{"unsigned", "double"}, "cannot apply to", false},
      {{"int", "char"}, "cannot both appear", true},
      {{"i33"}, "is not a type", true},
      // An empty spelling has no word to point a suggestion at, so it is a
      // malformed type and not an unknown name.
      {{""}, "is not a type", false},
  };
  for (const Case& one : cases) {
    const TypeSpecResult result = readTypeSpec(one.words, types);
    EXPECT_FALSE(result.ok) << result.message;
    EXPECT_NE(result.message.find(one.fragment), std::string::npos) << result.message;
    EXPECT_EQ(!result.unknownWord.empty(), one.unknown) << result.message;
  }
}

TEST(TypeSpecTest, AnEmptyRunIsRefusedRatherThanGuessed) {
  TypeStore types;
  const TypeSpecResult result = readTypeSpec({}, types);
  EXPECT_FALSE(result.ok);
  // A missing type is the parser's finding; defaulting to `int` here would hide
  // it behind a type nobody wrote.
  EXPECT_NE(result.message.find("expected a type name"), std::string::npos);
}

TEST(TypeSpecTest, EveryNameInTheSuggestionTableIsATypeOnItsOwn) {
  // The table is what a "did you mean ...?" searches. Every word in it has to be
  // a word the reader understands on its own, or the compiler would suggest a
  // spelling that then fails -- the worst possible answer to a typo.
  TypeStore types;
  for (const std::string_view name : typeNames()) {
    const TypeSpecResult result = readTypeSpec(words({name}), types);
    EXPECT_TRUE(result.ok) << name << ": " << result.message;
  }
  bool sawUint = false;
  for (const std::string_view name : typeNames()) {
    sawUint = sawUint || name == "uint";
  }
  EXPECT_TRUE(sawUint);
}

} // namespace
} // namespace minc::sema
