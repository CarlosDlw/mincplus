// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "sema/type_store.h"

#include <cstddef>
#include <cstdint>
#include <limits>
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
    index_.emplace(hashOf(type, {}), id);
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

  // The bottom type, last: the ids above are constants every dump and test
  // names, so a new one is appended and never inserted (`type.h`).
  add(TypeKind::Never, false, 0);
}

std::span<const TypeId> TypeStore::partsOf(const Type& type) const {
  if (type.paramCount == 0) {
    return {};
  }
  return std::span<const TypeId>(params_.data() + type.firstParam, type.paramCount);
}

std::uint64_t TypeStore::hashOf(const Type& type, std::span<const TypeId> parts) const {
  std::uint64_t hash = kFnvOffset;
  hash = mix(hash, static_cast<std::uint64_t>(type.kind));
  hash = mix(hash, type.isSigned ? 1U : 0U);
  hash = mix(hash, type.bits);
  hash = mix(hash, type.pointee.index);
  // The count is part of the identity, so it is part of the hash. A count left
  // out here would put `[4]i32` and `[8]i32` in one bucket and the comparison
  // would have to sort them out -- which is fine, but a store whose *hash* says
  // two types are the same is a store one comparison bug away from handing out
  // the wrong `TypeId` (`arrays.md` decision 19).
  hash = mix(hash, type.count);
  hash = mix(hash, type.returnType.index);
  hash = mix(hash, type.paramCount);
  // `variadic` and `name` are part of what a type *is*, and they are mixed here
  // even though a tuple leaves both at their defaults: one hash function for
  // every kind means no kind can be hashed by a rule the others do not share --
  // which is exactly how a `f(i32)` and a `f(i32, ...)` end up in two buckets for
  // one call site (`sema/type.h`).
  hash = mix(hash, type.variadic ? 1U : 0U);
  hash = mix(hash, type.name);
  // The member sequence is part of the structure; without it every `fn f(a)` and
  // `fn f(b)` would be one type, and so would `(i32, bool)` and `(bool, i32)`.
  for (const TypeId part : parts) {
    hash = mix(hash, part.index);
  }
  return hash;
}

bool TypeStore::equalFields(const Type& a, const Type& b) const {
  return a.kind == b.kind && a.isSigned == b.isSigned && a.bits == b.bits &&
         a.pointee == b.pointee && a.count == b.count && a.returnType == b.returnType &&
         a.name == b.name && a.variadic == b.variadic && a.paramCount == b.paramCount;
}

bool TypeStore::equalParts(const Type& type, std::span<const TypeId> parts) const {
  if (type.paramCount != parts.size()) {
    return false;
  }
  for (std::uint32_t i = 0; i < type.paramCount; ++i) {
    if (params_[type.firstParam + i] != parts[i]) {
      return false;
    }
  }
  return true;
}

TypeId TypeStore::intern(const Type& type) {
  return internSequence(type, partsOf(type));
}

TypeId TypeStore::internSequence(const Type& type, std::span<const TypeId> parts) {
  const std::uint64_t hash = hashOf(type, parts);
  const auto range = index_.equal_range(hash);
  for (auto it = range.first; it != range.second; ++it) {
    const Type& candidate = types_[it->second.index];
    if (equalFields(candidate, type) && equalParts(candidate, parts)) {
      return it->second;
    }
  }
  // The budget is checked *before* the insert, so hitting it is a diagnostic the
  // caller can report rather than an allocation that already happened.
  if (types_.size() >= maxTypes_) {
    return kInvalidType;
  }
  // The sequence is appended only now, and `type.firstParam` is the position it
  // will have: a caller that repeated an existing type never reached this line, so
  // a signature written twice does not grow the arena (`function`'s rule, and now
  // the tuple's as well).
  const TypeId id{static_cast<std::uint32_t>(types_.size())};
  params_.insert(params_.end(), parts.begin(), parts.end());
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

std::optional<std::size_t> TypeStore::arraySize(TypeId element, std::uint64_t count) const {
  if (count == 0 || !isObject(element)) {
    return std::nullopt;
  }
  const std::size_t size = sizeOf(element);
  // An object type with no bytes is not an element: `void`, `!`, a function and
  // the poison have no object representation, and an array of one would be an
  // array whose every element has no storage.
  if (size == 0) {
    return std::nullopt;
  }
  if (count > std::numeric_limits<std::size_t>::max() / size) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(count) * size;
}

TypeId TypeStore::arrayOf(TypeId element, std::uint64_t count) {
  // Both rules are asked *before* the intern, so an ill-formed array type never
  // enters the store: a type the rest of the pipeline can only misunderstand is
  // worse than a refusal here, and the caller has the count's spelling and its
  // node to point a sentence at.
  if (!arraySize(element, count).has_value()) {
    return kInvalidType;
  }
  Type type;
  type.kind = TypeKind::Array;
  type.pointee = element;
  type.count = count;
  return intern(type);
}

TypeId TypeStore::sliceOf(TypeId element) {
  // The one rule the array shares, asked for the same reason: an element has to
  // have storage. A slice of a deferred literal is a view of a width nobody has
  // decided, and a slice of `void` is a view of nothing.
  if (!isObject(element)) {
    return kInvalidType;
  }
  Type type;
  type.kind = TypeKind::Slice;
  type.pointee = element;
  // `count` stays 0: the count of a slice is not in the type, and 0 is not a
  // count the store ever gives a type that has one.
  return intern(type);
}

TypeId TypeStore::function(TypeId returnType, std::span<const TypeId> params, bool variadic) {
  Type type;
  type.kind = TypeKind::Function;
  type.returnType = returnType;
  // The position the parameters *would* take, and the one the comparison below
  // reads for the candidates that are already in the arena. The append happens in
  // `internSequence`, after the search -- so a repeated signature does not grow
  // the arena, which is why this cannot be a plain `intern`.
  type.firstParam = static_cast<std::uint32_t>(params_.size());
  type.paramCount = static_cast<std::uint32_t>(params.size());
  // `variadic` is part of the fields the comparison asks about because it is part
  // of what a function type *is*: interning `f(i32)` and `f(i32, ...)` to one id
  // would make the lowering emit one LLVM signature for two different calls.
  type.variadic = variadic;
  return internSequence(type, params);
}

TypeId TypeStore::tupleOf(std::span<const TypeId> members) {
  // The three refusals, all before the intern, so an ill-formed product never
  // enters the store: a type the rest of the pipeline can only misunderstand is
  // worse than a refusal here, and the caller has the members' nodes to point a
  // sentence at (`tuples.md`, decision 1).
  if (members.size() < 2) {
    return kInvalidType;
  }
  for (const TypeId member : members) {
    if (!isObject(member)) {
      return kInvalidType;
    }
  }
  // The layout arithmetic, for the same reason the array does it: a product whose
  // size does not fit `size_t` is refused where it is built, so `sizeOf` below is
  // a number and not a question. It also means the size and every member offset
  // are computed once per type and never overflow anywhere else.
  if (!tupleSize(members).has_value()) {
    return kInvalidType;
  }
  Type type;
  type.kind = TypeKind::Tuple;
  type.firstParam = static_cast<std::uint32_t>(params_.size());
  type.paramCount = static_cast<std::uint32_t>(members.size());
  return internSequence(type, members);
}

std::optional<std::size_t> TypeStore::tupleSize(std::span<const TypeId> members) const {
  std::size_t align = 1;
  std::size_t size = 0;
  for (const TypeId member : members) {
    const std::size_t memberAlign = alignOf(member);
    const std::size_t memberSize = sizeOf(member);
    if (memberAlign == 0 || memberSize == 0) {
      return std::nullopt;
    }
    // The padding before this member: the size so far rounded up to this
    // member's alignment.
    if (size > std::numeric_limits<std::size_t>::max() - (memberAlign - 1U)) {
      return std::nullopt;
    }
    size = ((size + memberAlign - 1U) / memberAlign) * memberAlign;
    if (memberSize > std::numeric_limits<std::size_t>::max() - size) {
      return std::nullopt;
    }
    size += memberSize;
    align = std::max(align, memberAlign);
  }
  // The whole object, padded to its own alignment -- the C rule, measured
  // (`tuples.md`, decision 4): `{char, int}` is eight bytes and `{int, char}` is
  // eight, while `{char, char}` is two.
  if (size > std::numeric_limits<std::size_t>::max() - (align - 1U)) {
    return std::nullopt;
  }
  return ((size + align - 1U) / align) * align;
}

std::size_t TypeStore::memberOffset(TypeId id, std::uint32_t index) const {
  if (!isTuple(id) || index >= get(id).paramCount) {
    return 0;
  }
  const std::span<const TypeId> members = membersOf(id);
  std::size_t size = 0;
  for (std::uint32_t i = 0; i < members.size(); ++i) {
    // A member has an object -- `tupleOf` refuses anything else -- so its alignment
    // is at least one. The floor is what makes that a fact the arithmetic below can
    // carry rather than a fact a reader (or an analysis) has to hold in mind: every
    // tuple was laid out by `tupleSize` before it was interned, and that function
    // refused any whose layout could not be computed.
    const std::size_t memberAlign = std::max<std::size_t>(alignOf(members[i]), 1U);
    if (i == index) {
      return ((size + memberAlign - 1U) / memberAlign) * memberAlign;
    }
    size = ((size + memberAlign - 1U) / memberAlign) * memberAlign + sizeOf(members[i]);
  }
  return 0;
}

std::span<const TypeId> TypeStore::membersOf(TypeId id) const {
  if (!isTuple(id)) {
    return {};
  }
  return partsOf(get(id));
}

std::span<const TypeId> TypeStore::paramsOf(TypeId id) const {
  if (!known(id) || get(id).kind != TypeKind::Function) {
    return {};
  }
  return partsOf(get(id));
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
bool TypeStore::isArray(TypeId id) const {
  return known(id) && get(id).kind == TypeKind::Array;
}
bool TypeStore::isSlice(TypeId id) const {
  return known(id) && get(id).kind == TypeKind::Slice;
}
bool TypeStore::isTuple(TypeId id) const {
  return known(id) && get(id).kind == TypeKind::Tuple;
}
bool TypeStore::isAggregate(TypeId id) const {
  // Three kinds and not one, because `isArray` is asked wherever the *storage* is
  // the subject (`sizeof` of the object, an element count, a copy) and a view is
  // not storage. What they share is that a load, a store or a copy moves them as
  // one object (`slices.md`), and a product is storage with no single element
  // type: `(i32, bool)` copies as one object and has no `[i]`.
  return isArray(id) || isSlice(id) || isTuple(id);
}
bool TypeStore::isObject(TypeId id) const {
  if (!known(id)) {
    return false;
  }
  const TypeKind kind = get(id).kind;
  if (kind == TypeKind::Array || kind == TypeKind::Slice) {
    // A view is an object in the only sense this predicate asks: it has a
    // representation, so it can be a binding, a parameter, an element of an
    // array, or the source of a copy. What it cannot be is `isArray`.
    return true;
  }
  if (kind == TypeKind::Tuple) {
    // Every member was checked when the type was built (`tupleOf`), so a tuple
    // that is in the store has a size -- which is the whole of what this
    // predicate asks. What it is *not* is scalar: it is more than one word, so
    // every consumer that needs a register-sized value has to ask a different
    // question.
    return true;
  }
  // `isScalar` minus the deferred literals: they are scalar-*shaped* and have no
  // width yet, so nothing may store one and nothing may be an array of one.
  return isScalar(id) && !isDeferred(id);
}
TypeId TypeStore::elementOf(TypeId id) const {
  if (!isArray(id) && !isSlice(id)) {
    return kInvalidType;
  }
  return get(id).pointee;
}
std::uint64_t TypeStore::countOf(TypeId id) const {
  if (!isArray(id)) {
    return 0;
  }
  return get(id).count;
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
bool TypeStore::isNever(TypeId id) const {
  return known(id) && get(id).kind == TypeKind::Never;
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
  case TypeKind::Never:
    return "!";
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
    // The count is *written*, because the canonical spelling is what a reader
    // can type back: `[4]i32` is the type, and `i32[]` was never a spelling this
    // language had. The number is the folded value, so `[0x10]i32` prints as
    // `[16]i32` -- one type, one name (`arrays.md` decision 19).
    return "[" + std::to_string(type.count) + "]" + spelling(type.pointee);
  case TypeKind::Slice:
    // `[]T`, with nothing between the brackets: the canonical spelling of the
    // view is what `slices.md` reserved and what a reader types back. There is no
    // length to print, which is the whole difference from the line above.
    return "[]" + spelling(type.pointee);
  case TypeKind::Tuple: {
    // `(i32, bool)`, in the order written: the canonical spelling is what a
    // reader can type back, and the order is part of the type -- which is why the
    // members are printed and not summarized as a count (`tuples.md`, decision 1).
    std::string text = "(";
    const std::span<const TypeId> members = membersOf(id);
    for (std::size_t i = 0; i < members.size(); ++i) {
      if (i != 0) {
        text += ", ";
      }
      text += spelling(members[i]);
    }
    text += ")";
    return text;
  }
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
  case TypeKind::Float: {
    // The ABI's slot for the width: the value's bytes rounded up to the
    // alignment the target states for it. That is the same number as the width
    // for `f32`, `f64` and `f128` on every target here, and it is the one place
    // the two questions are not the same: `f80` is ten bytes of value in a
    // sixteen-byte slot on System V and in a **four**-byte slot on i386, where
    // the object is twelve bytes (`f80:32`). Answering 16 there would make the
    // object disagree with the type LLVM emits (`x86_fp80`), and every byte
    // count taken from this function -- `sizeof`, a debug record, a future
    // `memcpy` length -- would be four bytes too long.
    const std::size_t bytes = type.bits / 8U;
    const std::size_t align = alignOf(id);
    // An alignment of zero means "this kind has no object", which a float never
    // is -- the guard is here so the arithmetic cannot divide by it, and so a
    // reader does not have to hold two kinds in mind to see that it cannot.
    return align == 0 ? bytes : ((bytes + align - 1U) / align) * align;
  }
  case TypeKind::Str:
  case TypeKind::Pointer:
    // `*void` included: a pointer to `void` is a pointer, and its size is the
    // pointer's. It is `void` that has no object representation, not a pointer
    // to one.
    return target_.pointerBits / 8U;
  case TypeKind::Array:
    // The element's **complete** size, padding included, times the count -- the
    // rule that makes `p + 1` land on the next element and `sizeof(TABLE)` agree
    // with the allocator (`arrays.md` decision 6). It cannot overflow: `arrayOf`
    // refused a product that would not fit, so this multiply is the same number
    // the store already computed.
    return static_cast<std::size_t>(type.count) * sizeOf(type.pointee);
  case TypeKind::Slice:
    // Two words and no data: the descriptor is a pointer and a length, and
    // `sizeof(s)` answers for the *view* (`slices.md` decision 5). The multiply
    // cannot overflow: the pointer width is 16, 32 or 64 bits, so the product is
    // at most 16 bytes.
    return static_cast<std::size_t>(target_.pointerBits / 8U) * 2U;
  case TypeKind::Tuple:
    // The one layout rule, asked of the same function the builder used to refuse
    // a product that does not fit: a tuple that is *in* the store has a size, so
    // the `value_or` is unreachable and says so rather than pretending.
    return tupleSize(membersOf(id)).value_or(0);
  case TypeKind::Error:
  case TypeKind::Void:
  case TypeKind::Never:
  case TypeKind::Function:
  case TypeKind::IntLiteral:
  case TypeKind::FloatLiteral:
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
    // A width is not an alignment on every target: i386's ABI aligns a 64-bit
    // integer to four bytes (`i64:32:64`), and the number this function answers
    // is the one every emitted `align N` is built from -- so it has to be the
    // target's, not the width's.
    return type.bits == 64 ? target_.int64AlignBits / 8U : type.bits / 8U;
  case TypeKind::Float:
    switch (type.bits) {
    case 64:
      return target_.float64AlignBits / 8U;
    case 80:
      return target_.float80AlignBits / 8U;
    default:
      // 32 and 128 are aligned to their own size on every target this compiler
      // names (`f32:32` by default, `f128:128` by default and stated on i386).
      return type.bits / 8U;
    }
  case TypeKind::Str:
  case TypeKind::Pointer:
    return target_.pointerBits / 8U;
  case TypeKind::Array:
    // The element's alignment, not the object's size: `[3]i8` is aligned like an
    // `i8`, and aligning it like a machine word would make every `[3]i8` field of
    // a future `struct` three bytes of padding wider than the C one.
    return alignOf(type.pointee);
  case TypeKind::Slice:
    // The descriptor is a pointer and a length, so it is aligned like a pointer
    // -- not like its element. `[]u8` is eight-byte aligned on a 64-bit target,
    // and a `struct` that holds one gets the padding a C compiler would give it.
    return target_.pointerBits / 8U;
  case TypeKind::Tuple: {
    // The largest member alignment, and never zero: every member is an object
    // (`tupleOf`), and an object has an alignment. A product is aligned like its
    // strictest member, which is what makes `(i8, i64)` a sixteen-byte object
    // starting at an eight-byte boundary.
    std::size_t align = 1;
    for (const TypeId member : membersOf(id)) {
      align = std::max(align, alignOf(member));
    }
    return align;
  }
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
