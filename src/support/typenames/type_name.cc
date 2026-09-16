// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "support/typenames/type_name.h"

#include <array>
#include <string_view>

namespace minc::support {
namespace {

// The list, and the whole of it. Order is by kind -- the language's own names,
// then the C specifier words, then the shorthands -- so a diff reads as a row
// added where it belongs.
//
// `char` and `void` appear once, and `long` twice in `sema`'s reader (a `long`
// is a length that may repeat); a *set* is what this is, so a word is listed
// once and the reader's own grammar decides how it may repeat.
constexpr std::array<std::string_view, 31> kTypeNames{{
    "i8",     "i16",       "i32",  "i64",   "i128",   "u8",    "u16",      "u32",
    "u64",    "u128",      "f32",  "f64",   "f80",    "isize", "usize",    "ssize_t",
    "size_t", "ptrdiff_t", "bool", "char",  "str",    "void",  "signed",   "unsigned",
    "short",  "long",      "int",  "float", "double", "uint",  "__int128",
}};

} // namespace

std::span<const std::string_view> typeNameWords() {
  return kTypeNames;
}

bool isTypeNameWord(std::string_view word) {
  for (const std::string_view name : kTypeNames) {
    if (name == word) {
      return true;
    }
  }
  return false;
}

} // namespace minc::support
