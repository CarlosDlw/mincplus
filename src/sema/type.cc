// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "sema/type.h"

#include <string_view>

namespace minc::sema {

std::string_view toString(TypeKind kind) {
  switch (kind) {
  case TypeKind::Error:
    return "error";
  case TypeKind::Void:
    return "void";
  case TypeKind::Bool:
    return "bool";
  case TypeKind::Char:
    return "char";
  case TypeKind::Int:
    return "int";
  case TypeKind::Float:
    return "float";
  case TypeKind::Str:
    return "str";
  case TypeKind::Function:
    return "function";
  case TypeKind::IntLiteral:
    return "integer-literal";
  case TypeKind::FloatLiteral:
    return "float-literal";
  case TypeKind::Pointer:
    return "pointer";
  case TypeKind::Array:
    return "array";
  }
  return "unknown";
}

} // namespace minc::sema
