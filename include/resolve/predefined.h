// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The names the language binds before any source is read.
//
// This is Go's *universe block* in miniature, and it exists here for one reason:
// three stages have to agree about these names and none of them may own the
// answer alone. `resolve` binds them (it is the stage that owns scopes), `sema`
// gives them their types, and `ir` gives them their constants -- and before this
// table existed each of the three carried its own copy of the list, matched by
// string spelling. Three lists in three modules, held together by nothing the
// compiler can check, is exactly the shape that makes one name's rule drift from
// another's.
//
// So the enumeration is the *identity* and the spelling is data. A stage reads
// `Def::predefined` -- one field, one enum -- and switches on it, which means
// adding a name is one row here plus one arm in each switch, and the compiler
// points at every arm that has not been written yet.
//
// The lexer deliberately does not make these keywords: `true` is a value, not
// grammar, so `let true = 1;` shadows it like any other name and `-Wshadow` says
// so. What is *here* is only the fact that the name exists at all, before the
// unit is read.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace minc::resolve {

// Which predefined name a `Def` is. `None` means "an ordinary declaration", so
// the field that carries this needs no second boolean: `Predefined::None` *is*
// the answer to "is this a name the language bound?".
//
// The enumerators are ordered as the language lists them -- the two boolean
// values, then the null pointer -- and not alphabetically, because the order is
// the order a reader of the table wants and the table is the definition.
enum class Predefined : std::uint8_t {
  None = 0,
  False,
  True,
  Null,
};

// One row: what the name is spelled, and what it is.
//
// The spelling points into static storage and lives as long as the program, so
// binding one is an `intern` call and never an allocation.
struct PredefinedName {
  std::string_view spelling;
  Predefined name;
};

// The whole set. Small on purpose: every row is a name the user cannot use for
// anything else, so a row is a promise the language keeps forever.
inline constexpr std::array<PredefinedName, 3> kPredefinedNames{{
    {"false", Predefined::False},
    {"true", Predefined::True},
    {"null", Predefined::Null},
}};

[[nodiscard]] constexpr bool isPredefined(Predefined value) {
  return value != Predefined::None;
}

// The predefined name this spelling is, or `None`. For the edges -- a header, a
// `#if` -- where the name arrives as text and not as a `Def`. Inside the
// pipeline the answer is `Def::predefined` and this function is not consulted;
// matching on spelling is what the table exists to put in one place.
[[nodiscard]] constexpr Predefined predefinedFromSpelling(std::string_view spelling) {
  for (const PredefinedName& row : kPredefinedNames) {
    if (row.spelling == spelling) {
      return row.name;
    }
  }
  return Predefined::None;
}

// The name, for a message or a dump. `"none"` for `Predefined::None`.
[[nodiscard]] std::string_view toString(Predefined value);

} // namespace minc::resolve
