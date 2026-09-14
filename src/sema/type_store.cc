// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "sema/type_store.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "support/limits.h"

namespace minc::sema {
namespace {

// FNV-1a over the fields that make two types the same. Only ever a filter --
// `TypeStore::equal` confirms every candidate, because a collision that was
// believed would hand two different types the same identity.
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

std::uint64_t mix(std::uint64_t hash, std::uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) {
    hash ^= (value >> (i * 8U)) & 0xFFU;
    hash *= kFnvPrime;
  }
  return hash;
}

} // namespace

TypeStore::TypeStore(TargetInfo target, std::size_t maxTypes)
    : target_(std::move(target)), maxTypes_(maxTypes) {
  // Registration order *is* the constant list in `sema/type.h`. Appending here
  // without adding a constant (or the reverse) is caught by the test that
  // asserts `count() == kFirstInternedType` and by every test that names a
  // built-in id.
  types_.reserve(kFirstInternedType);
  // Fields are assigned rather than aggregate-initialized so that adding a field
  // to `Type` later cannot silently leave one of the built-ins half-built (and
  // so the compiler does not warn about the fields this list intentionally
  // leaves at their defaults).
  const auto add = [this](TypeKind kind, bool isSigned, std::uint16_t bits) {
    Type type;
    type.kind = kind;
    type.isSigned = isSigned;
    type.bits = bits;
    const TypeId id{static_cast<std::uint32_t>(types_.size())};
    index_.emplace(hashOf(type), id);
    types_.push_back(type);
  };

  add(TypeKind::Error, false, 0);
  add(TypeKind::Void, false, 0);
  add(TypeKind::Bool, false, 0);
  add(TypeKind::Char, false, target_.charBits);
  add(TypeKind::IntLiteral, false, 0);
  add(TypeKind::FloatLiteral, false, 0);
  add(TypeKind::Str, false, 0);

  for (const std::uint16_t bits : {std::uint16_t{8}, std::uint16_t{16}, std::uint16_t{32},
                                   std::uint16_t{64}, std::uint16_t{128}}) {
    add(TypeKind::Int, true, bits);
  }
  for (const std::uint16_t bits : {std::uint16_t{8}, std::uint16_t{16}, std::uint16_t{32},
                                   std::uint16_t{64}, std::uint16_t{128}}) {
    add(TypeKind::Int, false, bits);
  }
  for (const std::uint16_t bits : {std::uint16_t{32}, std::uint16_t{64}, std::uint16_t{80}}) {
    add(TypeKind::Float, false, bits);
  }
}

std::uint64_t TypeStore::hashOf(const Type& type) const {
  std::uint64_t hash = kFnvOffset;
  hash = mix(hash, static_cast<std::uint64_t>(type.kind));
  hash = mix(hash, type.isSigned ? 1U : 0U);
  hash = mix(hash, type.bits);
  hash = mix(hash, type.pointee.index);
  hash = mix(hash, type.returnType.index);
  hash = mix(hash, type.paramCount);
  // The parameter list is part of the structure; without it every `fn f(a)` and
  // `fn f(b)` would be one type, which is the bug that makes a call site
  // type-check against the wrong signature.
  for (std::uint32_t i = 0; i < type.paramCount; ++i) {
    hash = mix(hash, params_[type.firstParam + i].index);
  }
  return hash;
}

bool TypeStore::equal(const Type& a, const Type& b) const {
  if (a.kind != b.kind || a.isSigned != b.isSigned || a.bits != b.bits || a.pointee != b.pointee ||
      a.returnType != b.returnType || a.name != b.name || a.paramCount != b.paramCount) {
    return false;
  }
  for (std::uint32_t i = 0; i < a.paramCount; ++i) {
    if (params_[a.firstParam + i] != params_[b.firstParam + i]) {
      return false;
    }
  }
  return true;
}

TypeId TypeStore::intern(const Type& type) {
  const std::uint64_t hash = hashOf(type);
  const auto range = index_.equal_range(hash);
  for (auto it = range.first; it != range.second; ++it) {
    if (equal(types_[it->second.index], type)) {
      return it->second;
    }
  }
  // The budget is checked *before* the insert, so hitting it is a diagnostic the
  // caller can report rather than an allocation that already happened.
  if (types_.size() >= maxTypes_) {
    return kInvalidType;
  }
  const TypeId id{static_cast<std::uint32_t>(types_.size())};
  types_.push_back(type);
  index_.emplace(hash, id);
  return id;
}

TypeId TypeStore::signedInt(std::uint16_t bits) {
  Type type;
  type.kind = TypeKind::Int;
  type.isSigned = true;
  type.bits = bits;
  return intern(type);
}
TypeId TypeStore::unsignedInt(std::uint16_t bits) {
  Type type;
  type.kind = TypeKind::Int;
  type.isSigned = false;
  type.bits = bits;
  return intern(type);
}
TypeId TypeStore::floatOf(std::uint16_t bits) {
  Type type;
  type.kind = TypeKind::Float;
  type.bits = bits;
  return intern(type);
}

TypeId TypeStore::pointerTo(TypeId pointee) {
  Type type;
  type.kind = TypeKind::Pointer;
  type.pointee = pointee;
  return intern(type);
}

TypeId TypeStore::function(TypeId returnType, std::span<const TypeId> params, bool variadic) {
  Type type;
  type.kind = TypeKind::Function;
  type.returnType = returnType;
  type.firstParam = static_cast<std::uint32_t>(params_.size());
  type.paramCount = static_cast<std::uint32_t>(params.size());
  type.variadic = variadic;
  // Look for an existing one *before* copying the parameters, so a repeated
  // signature does not grow the parameter arena.
  //
  // `variadic` is in the hash and in the comparison below because it is part of
  // what a function type *is*: interning `f(i32)` and `f(i32, ...)` to one id
  // would make the lowering emit one LLVM signature for two different calls.
  const std::uint64_t hash = [&] {
    std::uint64_t h = kFnvOffset;
    h = mix(h, static_cast<std::uint64_t>(type.kind));
    h = mix(h, type.returnType.index);
    h = mix(h, type.paramCount);
    h = mix(h, type.variadic ? 1u : 0u);
    for (const TypeId param : params) {
      h = mix(h, param.index);
    }
    return h;
  }();
  const auto range = index_.equal_range(hash);
  for (auto it = range.first; it != range.second; ++it) {
    const Type& candidate = types_[it->second.index];
    if (candidate.kind != TypeKind::Function || candidate.returnType != returnType ||
        candidate.paramCount != params.size() || candidate.variadic != variadic) {
      continue;
    }
    bool same = true;
    for (std::uint32_t i = 0; i < candidate.paramCount && same; ++i) {
      same = params_[candidate.firstParam + i] == params[i];
    }
    if (same) {
      return it->second;
    }
  }
  if (types_.size() >= maxTypes_) {
    return kInvalidType;
  }
  params_.insert(params_.end(), params.begin(), params.end());
  const TypeId id{static_cast<std::uint32_t>(types_.size())};
  types_.push_back(type);
  index_.emplace(hash, id);
  return id;
}

std::span<const TypeId> TypeStore::paramsOf(TypeId id) const {
  if (!known(id)) {
    return {};
  }
  const Type& type = get(id);
  if (type.kind != TypeKind::Function) {
    return {};
  }
  return std::span<const TypeId>(params_.data() + type.firstParam, type.paramCount);
}

bool TypeStore::isVariadic(TypeId id) const {
  return known(id) && get(id).kind == TypeKind::Function && get(id).variadic;
}

bool TypeStore::isInteger(TypeId id) const {
  if (!known(id)) {
    return false;
  }
  const TypeKind kind = get(id).kind;
  return kind == TypeKind::Int || kind == TypeKind::Char;
}
bool TypeStore::isFloat(TypeId id) const {
  return known(id) && get(id).kind == TypeKind::Float;
}
bool TypeStore::isSmallInteger(TypeId id) const {
  if (!known(id)) {
    return false;
  }
  const Type& type = get(id);
  if (type.kind == TypeKind::Char || type.kind == TypeKind::Bool) {
    return true;
  }
  return type.kind == TypeKind::Int && type.bits < 32;
}
bool TypeStore::isDeferred(TypeId id) const {
  if (!known(id)) {
    return false;
  }
  const TypeKind kind = get(id).kind;
  return kind == TypeKind::IntLiteral || kind == TypeKind::FloatLiteral;
}
bool TypeStore::isArithmetic(TypeId id) const {
  if (!known(id)) {
    return false;
  }
  const TypeKind kind = get(id).kind;
  return kind == TypeKind::Int || kind == TypeKind::Float || kind == TypeKind::Char ||
         kind == TypeKind::IntLiteral || kind == TypeKind::FloatLiteral;
}
bool TypeStore::isScalar(TypeId id) const {
  if (!known(id)) {
    return false;
  }
  const TypeKind kind = get(id).kind;
  return isArithmetic(id) || kind == TypeKind::Bool || kind == TypeKind::Str ||
         kind == TypeKind::Pointer;
}
bool TypeStore::isPointer(TypeId id) const {
  return known(id) && get(id).kind == TypeKind::Pointer;
}
bool TypeStore::isVoidPointer(TypeId id) const {
  if (!known(id)) {
    return false;
  }
  const Type& type = get(id);
  return type.kind == TypeKind::Pointer && type.pointee == kTypeVoid;
}
TypeId TypeStore::pointeeOf(TypeId id) const {
  if (!isPointer(id)) {
    return kInvalidType;
  }
  return get(id).pointee;
}
bool TypeStore::isVoid(TypeId id) const {
  return known(id) && get(id).kind == TypeKind::Void;
}
bool TypeStore::isError(TypeId id) const {
  return known(id) && get(id).kind == TypeKind::Error;
}

std::string TypeStore::spelling(TypeId id) const {
  if (!known(id)) {
    // Not a type, and not a lie: `spelling` is called on the result of a lookup
    // that has already been reported, and the message it appears in should say
    // "unknown" rather than crash.
    return "<invalid>";
  }
  const Type& type = get(id);
  switch (type.kind) {
  case TypeKind::Error:
    return "<error>";
  case TypeKind::Void:
    return "void";
  case TypeKind::Bool:
    return "bool";
  case TypeKind::Char:
    return "char";
  case TypeKind::Str:
    return "str";
  case TypeKind::IntLiteral:
    return "<integer literal>";
  case TypeKind::FloatLiteral:
    return "<float literal>";
  case TypeKind::Int:
    // The `.mx` primitive name, because the type is the primitive and `int` is
    // one of several spellings that reached it.
    return (type.isSigned ? "i" : "u") + std::to_string(type.bits);
  case TypeKind::Float:
    return "f" + std::to_string(type.bits);
  case TypeKind::Pointer:
    // **Prefix**, which is the one spelling the grammar accepts. The canonical
    // name of a type is what a reader can write back, and `i32*` is refused by
    // name -- see `parser.md`'s type-position rule and `memory.md`, *The surface*.
    return "*" + spelling(type.pointee);
  case TypeKind::Array:
    return spelling(type.pointee) + "[]";
  case TypeKind::Function: {
    std::string text = "fn " + spelling(type.returnType) + "(";
    const std::span<const TypeId> params = paramsOf(id);
    for (std::size_t i = 0; i < params.size(); ++i) {
      if (i != 0) {
        text += ", ";
      }
      text += spelling(params[i]);
    }
    if (type.variadic) {
      text += params.empty() ? "..." : ", ...";
    }
    text += ")";
    return text;
  }
  }
  return "?";
}

std::size_t TypeStore::sizeOf(TypeId id) const {
  if (!known(id)) {
    return 0;
  }
  const Type& type = get(id);
  switch (type.kind) {
  case TypeKind::Bool:
  case TypeKind::Char:
    return 1;
  case TypeKind::Int:
    return type.bits / 8U;
  case TypeKind::Float:
    // `f80` is stored in 16 bytes on System V: the 10 bytes of value rounded up
    // to the ABI's 16-byte slot. Anything narrower is its own width.
    return type.bits <= 64 ? type.bits / 8U : 16;
  case TypeKind::Str:
  case TypeKind::Pointer:
    // `*void` included: a pointer to `void` is a pointer, and its size is the
    // pointer's. It is `void` that has no object representation, not a pointer
    // to one.
    return target_.pointerBits / 8U;
  case TypeKind::Error:
  case TypeKind::Void:
  case TypeKind::Function:
  case TypeKind::IntLiteral:
  case TypeKind::FloatLiteral:
  case TypeKind::Array:
    return 0;
  }
  return 0;
}

std::size_t TypeStore::alignOf(TypeId id) const {
  if (!known(id)) {
    return 0;
  }
  const Type& type = get(id);
  switch (type.kind) {
  case TypeKind::Bool:
  case TypeKind::Char:
    return 1;
  case TypeKind::Int:
    return type.bits / 8U;
  case TypeKind::Float:
    return sizeOf(id);
  case TypeKind::Str:
  case TypeKind::Pointer:
    return target_.pointerBits / 8U;
  default:
    return 0;
  }
}

TypeId TypeStore::defaultOf(TypeId id) {
  if (!known(id)) {
    return kInvalidType;
  }
  // The kind is copied rather than the whole type being held by reference:
  // answering this question interns the default, and interning can grow the
  // store that the reference points into.
  const TypeKind kind = get(id).kind;
  if (kind == TypeKind::IntLiteral) {
    // C's rule for an unsuffixed decimal constant is the first of
    // `int`/`long`/`long long` that represents it. Since a deferred literal is
    // range-checked against whatever type it ends up with, the *default* only
    // matters when nothing constrains it, and `i32` is the answer a reader
    // expects from `let x = 1;`.
    return signedInt(32);
  }
  if (kind == TypeKind::FloatLiteral) {
    return floatOf(64);
  }
  return id;
}

} // namespace minc::sema
