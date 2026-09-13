// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "sema/typespec.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace minc::sema {
namespace {

// The `.mx` primitives. Each is a *whole* type: combining one with a C
// specifier is the error the reader exists to catch, and it is named rather than
// folded so the message can say which word was at fault.
struct Primitive {
  std::string_view name;
  TypeKind kind;
  std::uint16_t bits;
  bool isSigned;
  bool usesPointerWidth;
};

constexpr Primitive kPrimitives[] = {
    {"i8", TypeKind::Int, 8, true, false},
    {"i16", TypeKind::Int, 16, true, false},
    {"i32", TypeKind::Int, 32, true, false},
    {"i64", TypeKind::Int, 64, true, false},
    {"i128", TypeKind::Int, 128, true, false},
    {"u8", TypeKind::Int, 8, false, false},
    {"u16", TypeKind::Int, 16, false, false},
    {"u32", TypeKind::Int, 32, false, false},
    {"u64", TypeKind::Int, 64, false, false},
    {"u128", TypeKind::Int, 128, false, false},
    {"f32", TypeKind::Float, 32, false, false},
    {"f64", TypeKind::Float, 64, false, false},
    {"f80", TypeKind::Float, 80, false, false},
    // Pointer-sized: `isize`/`usize` are *spelled* for the intent, and resolve to
    // the pointer-sized type rather than to a type of their own (see `type.h`).
    {"isize", TypeKind::Int, 0, true, true},
    {"usize", TypeKind::Int, 0, false, true},
    {"ssize_t", TypeKind::Int, 0, true, true},
    {"ptrdiff_t", TypeKind::Int, 0, true, true},
    {"size_t", TypeKind::Int, 0, false, true},
    // Scalars and the non-value type.
    {"bool", TypeKind::Bool, 0, false, false},
    {"char", TypeKind::Char, 0, false, false},
    {"str", TypeKind::Str, 0, false, false},
    {"void", TypeKind::Void, 0, false, false},
};

// The C specifier words. Kept as strings rather than an enum because they are
// compared against spellings, and the table is what `typeNames()` hands to the
// suggestion search.
constexpr std::string_view kCSpecifiers[] = {"signed", "unsigned", "short", "long",  "long",
                                             "int",    "char",     "float", "double"};

// Shorthands whose meaning is a *whole run* of specifiers. Written down rather
// than folded into the state machine below, because `uint` is not "unsigned
// applied to something" -- it is another way to write `unsigned int`, and the
// only difference a reader should ever see is the spelling they typed. Kept to
// exactly what `README.md` promises: the combined `uint`, and the compiler's
// `__int128` extension, which needs two words on the unsigned side.
struct Alias {
  std::string_view words[2];
  std::string_view expands[2];
};

constexpr Alias kAliases[] = {
    {{"uint", ""}, {"unsigned", "int"}},
    {{"__int128", ""}, {"i128", ""}},
    {{"unsigned", "__int128"}, {"u128", ""}},
};

// How many words an alias consumes: two when its second word is part of the
// spelling (`unsigned __int128`), one otherwise.
[[nodiscard]] constexpr std::size_t aliasLength(const Alias& alias) {
  return alias.words[1].empty() ? 1 : 2;
}

[[nodiscard]] const Primitive* findPrimitive(std::string_view word) {
  for (const Primitive& primitive : kPrimitives) {
    if (primitive.name == word) {
      return &primitive;
    }
  }
  return nullptr;
}

[[nodiscard]] bool isCSpecifier(std::string_view word) {
  for (const std::string_view specifier : kCSpecifiers) {
    if (specifier == word) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] TypeSpecResult fail(std::string message, std::string_view word = {}) {
  TypeSpecResult result;
  result.message = std::move(message);
  result.unknownWord = word;
  return result;
}

[[nodiscard]] TypeSpecResult ok(TypeId type) {
  TypeSpecResult result;
  result.type = type;
  result.ok = true;
  return result;
}

[[nodiscard]] std::string quoted(std::string_view word) {
  return "`" + std::string(word) + "`";
}

} // namespace

std::span<const std::string_view> typeNames() {
  static const std::vector<std::string_view> names = [] {
    std::vector<std::string_view> all;
    all.reserve(std::size(kPrimitives) + std::size(kCSpecifiers));
    for (const Primitive& primitive : kPrimitives) {
      all.push_back(primitive.name);
    }
    for (const std::string_view specifier : kCSpecifiers) {
      all.push_back(specifier);
    }
    for (const Alias& alias : kAliases) {
      all.push_back(alias.words[0]);
    }
    return all;
  }();
  return names;
}

TypeSpecResult readTypeSpec(std::span<const std::string_view> words, TypeStore& types) {
  const TargetInfo& target = types.target();
  if (words.empty()) {
    return fail("expected a type name");
  }

  // The shorthands are *expansions*, applied before anything is interpreted:
  // after this the reader sees only specifier words and primitives, so a
  // shorthand cannot reach a code path a spelled-out type does not, and the
  // error for `uint i32` is the same error `unsigned int i32` earns. Longest
  // match first, so `unsigned __int128` is read as one alias and not as
  // `unsigned` followed by `__int128`.
  std::vector<std::string_view> expanded;
  expanded.reserve(words.size() + 2);
  for (std::size_t i = 0; i < words.size();) {
    const Alias* matched = nullptr;
    for (const Alias& alias : kAliases) {
      const std::size_t length = aliasLength(alias);
      if (i + length > words.size() || words[i] != alias.words[0]) {
        continue;
      }
      if (length == 2 && words[i + 1] != alias.words[1]) {
        continue;
      }
      matched = &alias;
      break;
    }
    if (matched == nullptr) {
      expanded.push_back(words[i]);
      ++i;
      continue;
    }
    for (const std::string_view piece : matched->expands) {
      if (!piece.empty()) {
        expanded.push_back(piece);
      }
    }
    i += aliasLength(*matched);
  }
  words = std::span<const std::string_view>(expanded.data(), expanded.size());

  // A run made only of C specifier words is a C sequence, and the state machine
  // below owns it. This has to be decided *before* the primitive scan, because
  // `char` is both a `.mx` primitive and a C specifier word: `signed char` is
  // the C spelling of `i8` and must not be refused as "a primitive combined with
  // another word".
  bool allCSpecifiers = words.size() > 1;
  for (const std::string_view word : words) {
    if (!isCSpecifier(word)) {
      allCSpecifiers = false;
      break;
    }
  }

  // A primitive is the whole run. `unsigned i32` is not "unsigned applied to
  // i32" -- `i32` is not a C base type and has no width to modify -- so the
  // message says that instead of inventing a reading.
  for (const std::string_view word : words) {
    if (allCSpecifiers) {
      break;
    }
    const Primitive* primitive = findPrimitive(word);
    if (primitive == nullptr) {
      continue;
    }
    if (words.size() == 1) {
      switch (primitive->kind) {
      case TypeKind::Void:
        return ok(kTypeVoid);
      case TypeKind::Bool:
        return ok(kTypeBool);
      case TypeKind::Char:
        return ok(kTypeChar);
      case TypeKind::Str:
        return ok(kTypeStr);
      case TypeKind::Float:
        return ok(types.floatOf(primitive->bits));
      default:
        break;
      }
      const std::uint16_t bits = primitive->usesPointerWidth ? target.pointerBits : primitive->bits;
      return ok(primitive->isSigned ? types.signedInt(bits) : types.unsignedInt(bits));
    }
    for (const std::string_view other : words) {
      if (other != word) {
        // No `unknownWord`: both words are understood, and the reader's job is to
        // name the *combination* as wrong. Offering a spelling suggestion here
        // would be answering a question nobody asked.
        return fail(quoted(word) + " is a complete type and cannot be combined with " +
                    quoted(other));
      }
    }
  }

  // The C specifier sequence. One of each group: a signedness, a length, a base.
  bool sawSigned = false;
  bool sawUnsigned = false;
  unsigned shortCount = 0;
  unsigned longCount = 0;
  std::string_view base;
  for (const std::string_view word : words) {
    if (word == "signed") {
      if (sawSigned || sawUnsigned) {
        return fail("`signed` and `unsigned` cannot both appear", word);
      }
      sawSigned = true;
    } else if (word == "unsigned") {
      if (sawUnsigned || sawSigned) {
        return fail("`signed` and `unsigned` cannot both appear", word);
      }
      sawUnsigned = true;
    } else if (word == "short") {
      if (shortCount != 0) {
        return fail("`short` appears more than once", word);
      }
      ++shortCount;
    } else if (word == "long") {
      ++longCount;
      if (longCount > 2) {
        return fail("`long` appears three times; the longest form is `long long`", word);
      }
    } else if (word == "int" || word == "char" || word == "float" || word == "double") {
      if (!base.empty()) {
        return fail(quoted(base) + " and " + quoted(word) + " cannot both appear", word);
      }
      base = word;
    } else if (isCSpecifier(word)) {
      return fail(quoted(word) + " cannot be repeated here", word);
    } else {
      return fail(quoted(word) + " is not a type", word);
    }
  }

  if (shortCount != 0 && longCount != 0) {
    return fail("`short` and `long` cannot both appear");
  }
  if (longCount != 0 && (base == "char" || base == "float")) {
    return fail("`long` cannot apply to " + quoted(base));
  }
  if (shortCount != 0 && (base == "char" || base == "float" || base == "double")) {
    return fail("`short` cannot apply to " + quoted(base));
  }

  // Any of the three groups may be absent, and the base is `int` when it is:
  // `unsigned` is `unsigned int`, `long` is `long int`.
  if (base.empty() || base == "int") {
    const std::uint16_t bits = longCount == 2    ? std::uint16_t{64}
                               : longCount == 1  ? target.longBits
                               : shortCount != 0 ? target.shortBits
                                                 : target.intBits;
    return ok(sawUnsigned ? types.unsignedInt(bits) : types.signedInt(bits));
  }
  if (base == "char") {
    if (sawUnsigned) {
      return ok(types.unsignedInt(target.charBits));
    }
    if (sawSigned) {
      return ok(types.signedInt(target.charBits));
    }
    // `.mx` `char`: distinct, and unsigned (README, decided).
    return ok(kTypeChar);
  }
  if (base == "float") {
    if (sawUnsigned || sawSigned) {
      return fail(std::string(sawUnsigned ? "`unsigned`" : "`signed`") +
                  " cannot apply to `float`");
    }
    return ok(types.floatOf(32));
  }
  // base == "double"
  if (sawUnsigned || sawSigned) {
    return fail(std::string(sawUnsigned ? "`unsigned`" : "`signed`") + " cannot apply to `double`");
  }
  if (longCount == 1) {
    // `long double` is the target's extended format: 80 bits on System V, and
    // what MSVC calls `double` on Windows.
    return ok(types.floatOf(target.longDoubleBits));
  }
  return ok(types.floatOf(64));
}

} // namespace minc::sema
