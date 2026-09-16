// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The type model: identity, the built-ins, the target table, and the reader for
// a type position's identifier run.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
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
  // The bottom type, and the two questions about it that are different: its kind
  // is `never` (for a reader of the implementation) and its spelling is `!` (for
  // a reader of the source).
  EXPECT_EQ(toString(types.get(kTypeNever).kind), "never");
  EXPECT_EQ(types.spelling(kTypeNever), "!");
  EXPECT_TRUE(types.isNever(kTypeNever));
  EXPECT_FALSE(types.isNever(kTypeVoid));
  EXPECT_FALSE(types.isVoid(kTypeNever));
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

TEST(TypeStoreTest, TheCountIsPartOfTheIdentity) {
  TypeStore types;
  const TypeId four = types.arrayOf(kTypeI32, 4);
  ASSERT_TRUE(four.valid());
  // Structural interning, with the count inside the structure: two `[4]i32` are
  // one type, and a different count is a different type -- the decision every
  // other property of an array rests on (`arrays.md` decision 1).
  EXPECT_EQ(four, types.arrayOf(kTypeI32, 4));
  EXPECT_NE(four, types.arrayOf(kTypeI32, 8));
  EXPECT_NE(four, types.arrayOf(kTypeU32, 4));
  EXPECT_NE(four, kTypeI32);
  EXPECT_TRUE(types.isArray(four));
  EXPECT_TRUE(types.isAggregate(four));
  EXPECT_EQ(types.countOf(four), 4u);
  EXPECT_EQ(types.elementOf(four), kTypeI32);
  // What an array is not: not a scalar, not a pointer, not deferred, and it has
  // no pointee for `pointeeOf` to answer with.
  EXPECT_FALSE(types.isScalar(four));
  EXPECT_FALSE(types.isPointer(four));
  EXPECT_FALSE(types.isDeferred(four));
  EXPECT_EQ(types.pointeeOf(four), kInvalidType);
  // The canonical spelling is what a reader can type back, so the count is
  // written and the element is spelled by its own rule.
  EXPECT_EQ(types.spelling(four), "[4]i32");
  EXPECT_EQ(types.spelling(types.arrayOf(types.arrayOf(kTypeI32, 3), 2)), "[2][3]i32");
  EXPECT_EQ(types.spelling(types.arrayOf(types.pointerTo(kTypeI32), 4)), "[4]*i32");
  EXPECT_EQ(types.spelling(types.pointerTo(four)), "*[4]i32");
  // A pointer to an array and an array of pointers are two types, which is the
  // distinction C's declarator syntax is famous for losing.
  EXPECT_NE(types.pointerTo(four), types.arrayOf(types.pointerTo(kTypeI32), 4));
}

TEST(TypeStoreTest, TheCountIsAValueAndNotASpelling) {
  TypeStore types;
  // The count reaches the store already folded, so the store never sees how it
  // was written: `[16]i32`, `[0x10]i32` and a future `[N]i32` with `const N = 16`
  // are one call with one argument (`arrays.md` decision 19). If this were a
  // string, two spellings of one count would be two types, and the identity the
  // whole compiler compares would depend on how a reader typed a number.
  const TypeId sixteen = types.arrayOf(kTypeI32, 16);
  EXPECT_EQ(sixteen, types.arrayOf(kTypeI32, 0x10));
  EXPECT_EQ(types.countOf(sixteen), 16u);
  EXPECT_EQ(types.spelling(sixteen), "[16]i32");
}

TEST(TypeStoreTest, AnArrayElementMustBeAnObject) {
  TypeStore types;
  // Objects: an element can be stored, copied and addressed.
  EXPECT_TRUE(types.arrayOf(kTypeI32, 4).valid());
  EXPECT_TRUE(types.arrayOf(kTypeStr, 2).valid());
  EXPECT_TRUE(types.arrayOf(types.pointerTo(kTypeI32), 4).valid());
  EXPECT_TRUE(types.arrayOf(types.arrayOf(kTypeI32, 2), 3).valid());
  EXPECT_TRUE(types.arrayOf(kTypeBool, 8).valid());
  // And not objects: `void` and `!` produce no value, a function is not an
  // object, the poison propagates instead of becoming an array, and a *deferred*
  // literal has no width -- which is the one `isObject` and `isScalar` answer
  // differently on purpose (`arrays.md` decision 21).
  EXPECT_FALSE(types.arrayOf(kTypeVoid, 4).valid());
  EXPECT_FALSE(types.arrayOf(kTypeNever, 4).valid());
  EXPECT_FALSE(types.arrayOf(kTypeError, 4).valid());
  EXPECT_FALSE(types.arrayOf(kTypeIntLiteral, 4).valid());
  EXPECT_FALSE(types.arrayOf(kTypeFloatLiteral, 4).valid());
  EXPECT_FALSE(types.arrayOf(types.function(kTypeVoid, {}, false), 4).valid());
  EXPECT_TRUE(types.isScalar(kTypeIntLiteral));
  EXPECT_FALSE(types.isObject(kTypeIntLiteral));
  // Zero is not a count. `[0]T` has no address and no size, and the language
  // refuses it rather than defining one (`arrays.md` decision 5).
  EXPECT_FALSE(types.arrayOf(kTypeI32, 0).valid());
  EXPECT_EQ(types.countOf(kTypeI32), 0u); // and 0 is not a count
  EXPECT_EQ(types.elementOf(kTypeI32), kInvalidType);
}

TEST(TypeStoreTest, ATypeWhoseSizeIsNotANumberIsRefused) {
  TypeStore types;
  ASSERT_TRUE(types.arraySize(kTypeI32, 4).has_value());
  EXPECT_EQ(*types.arraySize(kTypeI32, 4), 16u);
  // The one checked multiply: a product that does not fit `size_t` is refused at
  // the count, so no layout, `alloca` or `memcpy` length later has to defend
  // against a wrapped number (`arrays.md` decision 20).
  const std::uint64_t everything = std::numeric_limits<std::uint64_t>::max();
  EXPECT_FALSE(types.arraySize(kTypeI64, everything).has_value());
  EXPECT_FALSE(types.arrayOf(kTypeI64, everything).valid());
  // A nested count overflows the same way, and the inner type is already built
  // and valid when it does.
  const TypeId small = types.arrayOf(kTypeI64, 2);
  ASSERT_TRUE(small.valid());
  EXPECT_EQ(*types.arraySize(kTypeI64, 3), 24u);
  EXPECT_FALSE(types.arraySize(small, everything).has_value());
  // And the largest product that does fit is accepted: the bound is the bound.
  EXPECT_EQ(*types.arraySize(kTypeU8, everything), std::numeric_limits<std::size_t>::max());
}

TEST(TypeStoreTest, ArrayLayoutFollowsTheElement) {
  const std::optional<TargetInfo> sysvTarget = targetFromName(kTripleLinuxAmd64);
  ASSERT_TRUE(sysvTarget.has_value());
  TypeStore types{*sysvTarget};
  const TypeId ints = types.arrayOf(kTypeI32, 4);
  ASSERT_TRUE(ints.valid());
  EXPECT_EQ(types.sizeOf(ints), 16u);
  EXPECT_EQ(types.alignOf(ints), 4u);
  // Three bytes of `u8` occupy three bytes: the alignment is the *element's*, so
  // a `[3]u8` inside a future `struct` is three bytes and not a word of padding.
  const TypeId bytes = types.arrayOf(kTypeU8, 3);
  EXPECT_EQ(types.sizeOf(bytes), 3u);
  EXPECT_EQ(types.alignOf(bytes), 1u);
  EXPECT_EQ(types.sizeOf(types.arrayOf(kTypeBool, 8)), 8u);
  EXPECT_EQ(types.alignOf(types.arrayOf(kTypeBool, 8)), 1u);
  // Nesting multiplies the size and keeps the innermost alignment.
  const TypeId grid = types.arrayOf(types.arrayOf(kTypeI32, 3), 2);
  EXPECT_EQ(types.sizeOf(grid), 24u);
  EXPECT_EQ(types.alignOf(grid), 4u);
  // A pointer element: the count times the pointer, aligned like one.
  EXPECT_EQ(types.sizeOf(types.arrayOf(types.pointerTo(kTypeI32), 4)), 32u);
  EXPECT_EQ(types.alignOf(types.arrayOf(types.pointerTo(kTypeI32), 4)), 8u);
  // The element's *complete* size, padding included: an `f80` occupies its
  // 16-byte slot, so two of them are 32 bytes and not 20 (`arrays.md` decision
  // 6). Getting this wrong is a wrong answer, not a crash.
  const TypeId wides = types.arrayOf(kTypeF80, 2);
  EXPECT_EQ(types.sizeOf(wides), 32u);
  EXPECT_EQ(types.alignOf(wides), 16u);
  // A `str` is a pointer, so an array of them is an array of pointers.
  EXPECT_EQ(types.sizeOf(types.arrayOf(kTypeStr, 2)), 16u);
}

TEST(TypeStoreTest, ASliceIsTwoWordsWhateverItViews) {
  // The descriptor's layout, and the three properties that decide things
  // downstream: it is the *pointer* width twice, its alignment is the pointer's
  // and not the element's, and the element does not change either number. A
  // `[]u8` is eight bytes aligned to eight on a 64-bit target, which is what a
  // future `struct` holding one will pad to -- and a `[]i32` beside it needs no
  // padding at all.
  const std::optional<TargetInfo> sysvTarget = targetFromName(kTripleLinuxAmd64);
  ASSERT_TRUE(sysvTarget.has_value());
  TypeStore types{*sysvTarget};
  const TypeId ints = types.sliceOf(kTypeI32);
  ASSERT_TRUE(ints.valid());
  EXPECT_EQ(types.sizeOf(ints), 16u);
  EXPECT_EQ(types.alignOf(ints), 8u);
  const TypeId bytes = types.sliceOf(kTypeU8);
  ASSERT_TRUE(bytes.valid());
  EXPECT_EQ(types.sizeOf(bytes), 16u);
  EXPECT_EQ(types.alignOf(bytes), 8u);
  // The element is part of the *identity* and not of the layout: two slices of
  // different elements are two types that happen to be the same size, which is
  // the opposite of the array rule above and is exactly why `isSlice` is asked
  // instead of `sizeOf`.
  EXPECT_NE(ints, bytes);
  EXPECT_EQ(types.spelling(ints), "[]i32");
  EXPECT_EQ(types.spelling(bytes), "[]u8");
  // A slice of an aggregate is still two words: the descriptor names storage, it
  // does not contain it.
  const TypeId rows = types.sliceOf(types.arrayOf(kTypeI32, 4));
  ASSERT_TRUE(rows.valid());
  EXPECT_EQ(types.sizeOf(rows), 16u);
  EXPECT_EQ(types.spelling(rows), "[][4]i32");

  // Same two words on a different 64-bit ABI -- the layout follows the *pointer
  // width* and not the OS, which is the one thing a slice needs to agree with
  // `isize` about.
  const std::optional<TargetInfo> windowsTarget = targetFromName(kTripleWindowsAmd64);
  ASSERT_TRUE(windowsTarget.has_value());
  TypeStore windows{*windowsTarget};
  EXPECT_EQ(windows.sizeOf(windows.sliceOf(kTypeI32)), 16u);

  // A 32-bit target halves it, and the length is the index width there too -- the
  // same integer a `getelementptr` index is made of, so nothing truncates.

  const std::optional<TargetInfo> i386Target = targetFromName(kTripleLinuxI386);
  ASSERT_TRUE(i386Target.has_value());
  TypeStore i386{*i386Target};
  EXPECT_EQ(i386.sizeOf(i386.sliceOf(kTypeI32)), 8u);
  EXPECT_EQ(i386.alignOf(i386.sliceOf(kTypeI32)), 4u);
}

TEST(TypeStoreTest, ASliceIsAnObjectAndNotAnArray) {
  // The two predicates every consumer of an aggregate asks, answered differently
  // for the two kinds: a view *is* an object -- it can be a binding, a parameter,
  // or the source of a copy -- and it is not an array, so an access through one
  // has no extent in its type and `countOf` says 0 rather than inventing a
  // number. `elementOf` is shared, because the question is the same.
  TypeStore types;
  const TypeId slice = types.sliceOf(kTypeI32);
  ASSERT_TRUE(slice.valid());
  EXPECT_TRUE(types.isSlice(slice));
  EXPECT_FALSE(types.isArray(slice));
  EXPECT_TRUE(types.isAggregate(slice));
  EXPECT_TRUE(types.isObject(slice));
  EXPECT_EQ(types.countOf(slice), 0u);
  EXPECT_EQ(types.elementOf(slice), kTypeI32);
  // The element rule the array shares, and the one thing that can be refused: a
  // view has to be a view of something with a representation.
  EXPECT_FALSE(types.sliceOf(kTypeVoid).valid());
  EXPECT_FALSE(types.sliceOf(kTypeNever).valid());
  // Interning: one `[]i32`, whatever asked for it.
  EXPECT_EQ(types.sliceOf(kTypeI32), slice);
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
  EXPECT_EQ(sysv.sizeOf(kTypeNever), 0u); // and no values to have one

  const std::optional<TargetInfo> windowsTarget = targetFromName(kTripleWindowsAmd64);
  ASSERT_TRUE(windowsTarget.has_value());
  TypeStore windows{*windowsTarget};
  // Same widths for these two -- the difference is the *spelling* `long`, which
  // the specifier reader resolves, not the types themselves.
  EXPECT_EQ(windows.sizeOf(kTypeI64), 8u);
  EXPECT_EQ(windows.target().longBits, 32u);
}

TEST(TypeStoreTest, TheI386AlignmentIsTheAbisAndNotTheWidth) {
  // i386 is the target whose ABI disagrees with the width, and it disagrees in
  // both numbers: `i64` and `f64` are aligned to four bytes (`i64:32:64`,
  // `f64:32:64`), and the x87 format is *twelve* bytes in a four-byte slot
  // (`f80:32`) rather than the sixteen System V gives it. A store lays a value at
  // the alignment its type states, and `src/ir` compares that number against
  // LLVM's own data layout, so "the width" here is not an approximation -- it is
  // a refusal of a program that is not wrong.
  const std::optional<TargetInfo> i386Target = targetFromName(kTripleLinuxI386);
  ASSERT_TRUE(i386Target.has_value());
  TypeStore i386{*i386Target};

  EXPECT_EQ(i386.sizeOf(kTypeI64), 8u); // the size is still the width
  EXPECT_EQ(i386.alignOf(kTypeI64), 4u);
  EXPECT_EQ(i386.alignOf(kTypeF64), 4u);
  EXPECT_EQ(i386.sizeOf(kTypeF64), 8u);
  EXPECT_EQ(i386.alignOf(kTypeF80), 4u);
  EXPECT_EQ(i386.sizeOf(kTypeF80), 12u); // 10 bytes of value in a 4-byte slot

  // The consequences, which is where a wrong number stops being cosmetic: an
  // array's stride is the element's *complete* size, so `[2]f80` is 24 bytes on
  // this ABI and 32 on System V -- and a `sizeof` or a copy length taken from
  // either would be wrong by eight bytes on the other.
  EXPECT_EQ(i386.sizeOf(i386.arrayOf(kTypeF80, 2)), 24u);
  EXPECT_EQ(i386.alignOf(i386.arrayOf(kTypeF80, 2)), 4u);

  // A 32-bit pointer is four bytes and aligned to four, so a slice descriptor is
  // eight and `usize` follows the pointer rather than a constant.
  EXPECT_EQ(i386.sizeOf(kTypeStr), 4u);
  EXPECT_EQ(i386.alignOf(i386.sliceOf(kTypeI32)), 4u);
  EXPECT_EQ(i386.sizeOf(i386.sliceOf(kTypeI32)), 8u);

  // And the four-byte `i128` row is *not* narrowed with them: i386's layout
  // states `i128:128`, so a 128-bit value is aligned to sixteen there like
  // everywhere else. One field per width rather than one rule is what keeps that
  // true.
  EXPECT_EQ(i386.alignOf(kTypeI128), 16u);
  EXPECT_EQ(i386.alignOf(kTypeU128), 16u);
}

TEST(TypeSpecTest, AnArrayTypeReadsInsideOut) {
  TypeStore types;
  TypePart star;
  star.isStar = true;
  TypePart four;
  four.isArray = true;
  four.hasCount = true;
  four.count = 4;
  TypePart three = four;
  three.count = 3;
  TypePart two = four;
  two.count = 2;
  TypePart word;
  word.word = "i32";

  // `[4]i32` is the element type under the count, and the count is a *value*.
  const TypePart one[] = {four, word};
  const TypeSpecResult array = readType(one, types);
  ASSERT_TRUE(array.ok) << array.message;
  EXPECT_EQ(array.type, types.arrayOf(kTypeI32, 4));

  // `*[4]i32`: the array is one object and the pointer points at all of it.
  const TypePart pointerToArray[] = {star, four, word};
  const TypeSpecResult pointer = readType(pointerToArray, types);
  ASSERT_TRUE(pointer.ok) << pointer.message;
  EXPECT_EQ(pointer.type, types.pointerTo(types.arrayOf(kTypeI32, 4)));
  EXPECT_EQ(types.spelling(pointer.type), "*[4]i32");

  // `[4]*i32`: four pointers. One `*` moved across the count is the difference,
  // and it is the difference C's declarator syntax is famous for losing.
  const TypePart arrayOfPointers[] = {four, star, word};
  const TypeSpecResult pointers = readType(arrayOfPointers, types);
  ASSERT_TRUE(pointers.ok) << pointers.message;
  EXPECT_EQ(pointers.type, types.arrayOf(types.pointerTo(kTypeI32), 4));
  EXPECT_EQ(types.spelling(pointers.type), "[4]*i32");
  EXPECT_NE(pointers.type, pointer.type);

  // Nesting: the part nearest the words is the innermost, so `[2][3]i32` is two
  // arrays of three and the size is the product.
  const TypePart nested[] = {two, three, word};
  const TypeSpecResult grid = readType(nested, types);
  ASSERT_TRUE(grid.ok) << grid.message;
  EXPECT_EQ(types.spelling(grid.type), "[2][3]i32");
  EXPECT_EQ(types.sizeOf(grid.type), 24u);

  // A constructor written *after* the words is one mistake, and the sentence
  // names the side it belongs on -- the same refusal a `*i32` after the type
  // earns, one part over.
  const TypePart after[] = {word, four};
  const TypeSpecResult wrongSide = readType(after, types);
  EXPECT_FALSE(wrongSide.ok);
  EXPECT_NE(wrongSide.message.find("`[N]` before the element type"), std::string::npos)
      << wrongSide.message;

  // A constructor with nothing under it names what is missing, rather than
  // reporting "expected a type name" about a position the reader can see is a
  // pointer or an array.
  const TypePart noElement[] = {four};
  const TypeSpecResult bare = readType(noElement, types);
  EXPECT_FALSE(bare.ok);
  EXPECT_NE(bare.message.find("expected the element type of the array"), std::string::npos)
      << bare.message;
  const TypePart noPointee[] = {star};
  const TypeSpecResult bareStar = readType(noPointee, types);
  EXPECT_FALSE(bareStar.ok);
  EXPECT_NE(bareStar.message.find("expected the type the pointer points to"), std::string::npos)
      << bareStar.message;
}

TEST(TypeSpecTest, EachArrayRefusalNamesWhatToWrite) {
  TypeStore types;
  TypePart word;
  word.word = "i32";
  TypePart group;
  group.isArray = true;
  TypePart letters;
  letters.word = "void";

  // `[]T`: no count at all, so this is the slice -- a view, and none of the
  // count rules apply on the way to it. One element rule does, and it is the
  // only one that can still refuse.
  const TypePart slice[] = {group, word};
  const TypeSpecResult view = readType(slice, types);
  ASSERT_TRUE(view.ok) << view.message;
  EXPECT_TRUE(view.type.valid());
  EXPECT_TRUE(types.isSlice(view.type));
  EXPECT_EQ(types.spelling(view.type), "[]i32");

  const TypePart suffix[] = {group, letters};
  const TypeSpecResult ofVoidSlice = readType(suffix, types);
  EXPECT_FALSE(ofVoidSlice.ok);
  EXPECT_NE(ofVoidSlice.message.find("cannot be a slice element"), std::string::npos)
      << ofVoidSlice.message;

  // `[0]T`: written, and impossible. Distinct from `[]` above -- which is why the
  // part carries `hasCount` and not just a number.
  TypePart zero = group;
  zero.hasCount = true;
  zero.count = 0;
  const TypePart emptyArray[] = {zero, word};
  const TypeSpecResult none = readType(emptyArray, types);
  EXPECT_FALSE(none.ok);
  EXPECT_NE(none.message.find("count of an array type is at least 1"), std::string::npos)
      << none.message;

  // A count that no 64-bit number can hold. The fold happens where the spelling
  // is, and the part carries the fact that it failed.
  TypePart huge = group;
  huge.hasCount = true;
  huge.countOverflow = true;
  const TypePart tooBig[] = {huge, word};
  const TypeSpecResult wide = readType(tooBig, types);
  EXPECT_FALSE(wide.ok);
  EXPECT_NE(wide.message.find("has to be a number that fits in 64 bits"), std::string::npos)
      << wide.message;

  // An element that cannot be stored: `void`, `!` and a function have no object
  // representation, so an array of one has no elements.
  TypePart four = group;
  four.hasCount = true;
  four.count = 4;
  const TypePart ofVoid[] = {four, letters};
  const TypeSpecResult element = readType(ofVoid, types);
  EXPECT_FALSE(element.ok);
  EXPECT_NE(element.message.find("cannot be an array element"), std::string::npos)
      << element.message;

  // A size that is not a number: the array's own bytes have to fit, and the
  // refusal names the count it could not honour (`arrays.md` decision 20).
  TypePart everything = group;
  everything.hasCount = true;
  everything.count = std::numeric_limits<std::uint64_t>::max();
  TypePart wide64;
  wide64.word = "i64";
  const TypePart impossible[] = {everything, wide64};
  const TypeSpecResult hugeArray = readType(impossible, types);
  EXPECT_FALSE(hugeArray.ok);
  EXPECT_NE(hugeArray.message.find("larger than this target can address"), std::string::npos)
      << hugeArray.message;
}

TEST(TypeSpecTest, TheBottomTypeIsAWholeRunOfItsOwn) {
  TypeStore types;
  TypePart bang;
  bang.isBang = true;
  // Alone, it is a type: `fn ! f()` is a return type like any other, and the
  // reader's answer is the one the rest of the pipeline keys on.
  const TypeSpecResult alone = readType(std::span<const TypePart>(&bang, 1), types);
  ASSERT_TRUE(alone.ok) << alone.message;
  EXPECT_EQ(alone.type, kTypeNever);

  // Combined with anything, it is a spelling with no meaning to give -- and the
  // message says what to stop doing, because there is no `*!` and no `!i32` for
  // the reader to fall back on.
  TypePart star;
  star.isStar = true;
  const TypePart combined[] = {star, bang};
  const TypeSpecResult bad = readType(combined, types);
  EXPECT_FALSE(bad.ok);
  // No `unknownWord`: both tokens are understood, and `!` is not a misspelling
  // of anything to suggest.
  EXPECT_TRUE(bad.unknownWord.empty());
  EXPECT_NE(bad.message.find("`!` is a type on its own"), std::string::npos) << bad.message;
}

TEST(TypeSpecTest, ThePrimitivesAreWholeTypes) {
  TypeStore types;
  EXPECT_EQ(readTypeSpec(words({"i32"}), types).type, kTypeI32);
  EXPECT_EQ(readTypeSpec(words({"u128"}), types).type, kTypeU128);
  // `f80` is asked of a target that has x87, and not of the host: it is a
  // *format*, so a machine with no x87 refuses the word (`f80` on an Apple
  // silicon host is the case that made this test host-dependent).
  const std::optional<TargetInfo> x87 = targetFromName(kTripleLinuxAmd64);
  ASSERT_TRUE(x87.has_value());
  TypeStore withX87{*x87};
  EXPECT_EQ(readTypeSpec(words({"f80"}), withX87).type, kTypeF80);
  EXPECT_EQ(readTypeSpec(words({"bool"}), types).type, kTypeBool);
  EXPECT_EQ(readTypeSpec(words({"void"}), types).type, kTypeVoid);
  EXPECT_EQ(readTypeSpec(words({"char"}), types).type, kTypeChar);
  EXPECT_EQ(readTypeSpec(words({"str"}), types).type, kTypeStr);
}

TEST(TypeSpecTest, Float80IsRefusedWhereTheMachineHasNoX87) {
  // The one type whose existence is a property of the *machine* and not of the
  // width table: `f80` is x87's arithmetic, so AArch64 and RISC-V have no such
  // type at all, and the refusal is the **checker's**. Refusing it in the lowering
  // instead -- which is what the compiler did -- let `check` accept a program that
  // `build` could not compile, and "if the checker lets it pass, it must run" is
  // the property every other stage is built on.
  const std::optional<TargetInfo> arm = targetFromName(kTripleLinuxAarch64);
  ASSERT_TRUE(arm.has_value());
  ASSERT_FALSE(arm->hasFloat80());
  TypeStore aarch64{*arm};
  const TypeSpecResult refused = readTypeSpec(words({"f80"}), aarch64);
  EXPECT_FALSE(refused.ok);
  EXPECT_NE(refused.message.find("x87"), std::string::npos) << refused.message;
  EXPECT_NE(refused.message.find(kTripleLinuxAarch64), std::string::npos) << refused.message;
  // Nothing to suggest: `f80` is a word this reader knows, and "did you mean
  // `i8`?" is not a repair anybody can use.
  EXPECT_TRUE(refused.unknownWord.empty());
  // The portable spelling still works, and it is the format the target *does*
  // state: IEEE binary128 on AArch64 Linux, which is not `f80` and not `f64`.
  EXPECT_EQ(readTypeSpec(words({"long", "double"}), aarch64).type, aarch64.floatOf(128));

  // And on a machine that has it -- including the Windows ones, where `long
  // double` is spelled but is a `double` under MSVC -- `f80` is the type it names,
  // while `long double` stays the ABI's answer for the spelling.
  const std::optional<TargetInfo> windows = targetFromName(kTripleWindowsAmd64);
  ASSERT_TRUE(windows.has_value());
  EXPECT_TRUE(windows->hasFloat80());
  TypeStore msvc{*windows};
  EXPECT_EQ(readTypeSpec(words({"f80"}), msvc).type, kTypeF80);
  EXPECT_EQ(readTypeSpec(words({"long", "double"}), msvc).type, kTypeF64);
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
