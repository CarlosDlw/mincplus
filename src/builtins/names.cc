// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The words, and the one rule that says which names the compiler keeps.
//
// Two jobs live here because both are "a row said as text": a diagnostic, a page
// and a `--verbose` dump all have to print a row, and none of them may invent its
// own abbreviation for one. Each `toString` therefore has a total switch with no
// `default:` arm, so an enumerator added to the header fails to compile *here*
// until somebody decides what it is called in a sentence.
#include "builtins/builtin.h"

#include <cstddef>
#include <string>

namespace minc::builtins {
namespace {

// The prefix, in one place. It is a *rule* and not a list: the table decides what
// exists, and this decides what a program may not take. Two lists would be two
// answers to "is this name the compiler's", and a name that one of them thought
// was free is a name a program can use to mean something else.
constexpr std::string_view kReservedPrefix = "__builtin_";

} // namespace

std::string_view toString(SpellingClass value) {
  switch (value) {
  case SpellingClass::Reserved:
    return "reserved";
  case SpellingClass::Prelude:
    return "prelude";
  }
  return "?";
}

std::string_view toString(Status value) {
  switch (value) {
  case Status::Stable:
    return "stable";
  case Status::Internal:
    return "internal";
  }
  return "?";
}

std::string_view toString(BuiltinType value) {
  switch (value) {
  case BuiltinType::Void:
    return "void";
  case BuiltinType::Bool:
    return "bool";
  case BuiltinType::Char:
    return "char";
  case BuiltinType::Str:
    return "str";
  case BuiltinType::I8:
    return "i8";
  case BuiltinType::I16:
    return "i16";
  case BuiltinType::I32:
    return "i32";
  case BuiltinType::I64:
    return "i64";
  case BuiltinType::I128:
    return "i128";
  case BuiltinType::Isize:
    return "isize";
  case BuiltinType::U8:
    return "u8";
  case BuiltinType::U16:
    return "u16";
  case BuiltinType::U32:
    return "u32";
  case BuiltinType::U64:
    return "u64";
  case BuiltinType::U128:
    return "u128";
  case BuiltinType::Usize:
    return "usize";
  case BuiltinType::F32:
    return "f32";
  case BuiltinType::F64:
    return "f64";
  case BuiltinType::VoidPtr:
    return "*void";
  case BuiltinType::Never:
    return "!";
  case BuiltinType::AnyInteger:
    return "any-int";
  case BuiltinType::MatchArg:
    return "same";
  case BuiltinType::MatchArgPtr:
    return "*same";
  }
  return "?";
}

std::string_view toString(Effect value) {
  switch (value) {
  case Effect::None:
    return "none";
  case Effect::Diverges:
    return "diverges";
  }
  return "?";
}

std::string_view toString(TailOperand value) {
  switch (value) {
  case TailOperand::None:
    return "none";
  case TailOperand::I1False:
    return "i1 false";
  case TailOperand::I1True:
    return "i1 true";
  }
  return "?";
}

std::string_view toString(MatchedWidths value) {
  switch (value) {
  case MatchedWidths::Any:
    return "any";
  case MatchedWidths::EvenBytes:
    return "even byte counts";
  }
  return "?";
}

bool isReservedPrefix(std::string_view spelling) {
  // `starts_with` and not equality with the prefix: the prefix itself is the
  // compiler's too, so `__builtin_` as a declaration is refused as well. Nothing
  // is hidden by that -- the name has no meaning to a user either way.
  return spelling.starts_with(kReservedPrefix);
}

std::string signatureText(const BuiltinInfo& row) {
  std::string text = "(";
  for (std::size_t i = 0; i < row.signature.params.size(); ++i) {
    if (i != 0) {
      text += ", ";
    }
    text += toString(row.signature.params[i]);
  }
  text += ") -> ";
  text += toString(row.signature.result);
  return text;
}

} // namespace minc::builtins
