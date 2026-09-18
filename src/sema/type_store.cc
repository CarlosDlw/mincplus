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
    // The derived fields, computed the same way `internSequence` computes them
    // for every other type: a built-in that skipped this would answer a size of 0
    // and an alignment of 1 -- which is a plausible-looking pair of numbers, and
    // therefore worse than a wrong one, because `[4]i32` would then be an array
    // of nothing and nothing would say so.
    type.nodes = nodesOf({}, {});
    const Layout layout = layoutOf(type, {});
    type.size = layout.size;
    type.align = layout.align;
    type.unknownSize = layout.unknownSize;
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
  // The type's own row, and a row is never written again once its type has been
  // appended -- which is what makes the span safe to hold across an interning
  // (`type_store.h`, on `sequences_`).
  //
  // The bounds answer is for a `Type` that was never interned, which the store's
  // own callers cannot produce; the guard is here so that reaching it is an empty
  // row and not a read past the end, the same answer `known` gives for an id this
  // store does not know.
  if (type.sequence >= sequences_.size()) {
    return {};
  }
  return std::span<const TypeId>(sequences_[type.sequence]);
}

std::uint32_t TypeStore::childNodes(TypeId id) const {
  return known(id) ? get(id).nodes : 1;
}

// The layout of the type `type` is becoming, computed from the layouts of its
// children -- every one of which is already in the store, so this is O(children)
// and never a walk of the structure. This is the same rule `sizeOf`/`alignOf`/
// `hasUnknownSize` state, moved to where a type is built so that the answers can
// be *read* instead of recomputed (`type.h`, on `Type::size`).
//
// The rules, in the order they have to be asked:
//
//  - a **binder** has no width yet, and neither does an aggregate containing one:
//    that is `unknownSize`, which is what makes `[4]T` wait for its argument
//    instead of being arithmetic on a zero (`arrays.md` decision 21).
//  - a **function**, `void`, `!` and the poison have no object representation:
//    size 0 and alignment 0, which is the answer `sizeOf`/`alignOf` have always
//    given and a different statement from "not yet".
//  - an **array** multiplies its element's complete size, and a **slice** is a
//    pointer and a length -- neither arithmetic can overflow where it is built,
//    because `arrayOf` refused a product that would not fit.
//  - a **product** is its members in order, each at its own alignment, padded to
//    the strictest member's (`tuples.md`, decision 4).
TypeStore::Layout TypeStore::layoutOf(const Type& type, std::span<const TypeId> parts) const {
  Layout layout;
  const auto childSize = [this](TypeId id) -> std::size_t { return known(id) ? get(id).size : 0; };
  const auto childAlign = [this](TypeId id) -> std::uint32_t {
    return known(id) ? get(id).align : 0;
  };
  const auto childUnknown = [this](TypeId id) { return known(id) && get(id).unknownSize; };
  switch (type.kind) {
  case TypeKind::Bool:
  case TypeKind::Char:
    layout.size = 1;
    layout.align = 1;
    return layout;
  case TypeKind::Int:
    layout.size = type.bits / 8U;
    // A width is not an alignment on every target: i386's ABI aligns a 64-bit
    // integer to four bytes (`i64:32:64`), and this number is what every emitted
    // `align N` is built from.
    layout.align = type.bits == 64 ? static_cast<std::uint16_t>(target_.int64AlignBits / 8U)
                                   : static_cast<std::uint16_t>(type.bits / 8U);
    return layout;
  case TypeKind::Float: {
    // The ABI's slot for the width: the value's bytes rounded up to the
    // alignment the target states for it. `f80` is the one place the two are not
    // the same (`type_store.cc`'s old `sizeOf` said why: ten bytes of value in a
    // sixteen-byte slot on System V and in a four-byte slot on i386).
    switch (type.bits) {
    case 64:
      layout.align = static_cast<std::uint16_t>(target_.float64AlignBits / 8U);
      break;
    case 80:
      layout.align = static_cast<std::uint16_t>(target_.float80AlignBits / 8U);
      break;
    default:
      layout.align = static_cast<std::uint16_t>(type.bits / 8U);
      break;
    }
    const std::size_t bytes = type.bits / 8U;
    const std::size_t align = layout.align == 0 ? 0 : layout.align;
    layout.size = align == 0 ? bytes : ((bytes + align - 1U) / align) * align;
    return layout;
  }
  case TypeKind::Str:
  case TypeKind::Pointer:
    // `*void` included: a pointer to `void` is a pointer, and its size is the
    // pointer's. It is `void` that has no object representation.
    layout.size = target_.pointerBits / 8U;
    layout.align = static_cast<std::uint16_t>(target_.pointerBits / 8U);
    return layout;
  case TypeKind::Slice:
    // Two words and no data: the descriptor is a pointer and a length, and
    // `sizeof(s)` answers for the *view* (`slices.md` decision 5). Aligned like a
    // pointer and not like its element.
    layout.size = static_cast<std::size_t>(target_.pointerBits / 8U) * 2U;
    layout.align = static_cast<std::uint16_t>(target_.pointerBits / 8U);
    return layout;
  case TypeKind::Array:
    layout.align = static_cast<std::uint16_t>(childAlign(type.pointee));
    layout.unknownSize = childUnknown(type.pointee);
    layout.size =
        layout.unknownSize ? 0 : static_cast<std::size_t>(type.count) * childSize(type.pointee);
    return layout;
  case TypeKind::Tuple: {
    std::uint32_t align = 1;
    bool unknown = false;
    std::size_t size = 0;
    for (const TypeId member : parts) {
      if (childUnknown(member)) {
        unknown = true;
        break;
      }
      const std::uint32_t memberAlign = static_cast<std::uint32_t>(childAlign(member));
      if (memberAlign == 0) {
        // A member with no alignment is a member with no object, which `tupleOf`
        // refuses; the layout of a type that cannot exist is not a number, and
        // the zero `unknown` path below leaves the answer at 0 as well.
        unknown = true;
        break;
      }
      align = std::max(align, memberAlign);
      // The padding before this member, and then the member itself.
      size = ((size + memberAlign - 1U) / memberAlign) * memberAlign;
      size += childSize(member);
    }
    layout.unknownSize = unknown;
    if (!unknown) {
      // The whole object is padded to its strictest member's alignment.
      layout.align = static_cast<std::uint16_t>(align);
      layout.size = ((size + align - 1U) / align) * align;
    }
    return layout;
  }
  case TypeKind::Param:
    // A binder stands for a type argument, and an argument *is* an object -- what
    // it does not have, until it is substituted, is a width. An alignment of 1 is
    // what keeps a product of binders from claiming anything (`alignOf`'s rule).
    layout.align = 1;
    layout.unknownSize = true;
    return layout;
  case TypeKind::Error:
  case TypeKind::Void:
  case TypeKind::Never:
  case TypeKind::Function:
  case TypeKind::IntLiteral:
  case TypeKind::FloatLiteral:
    // No object representation: size 0 and alignment 0. A deferred literal is
    // here for a different reason -- nothing decides its width yet, so nothing
    // may store one -- and `isObject` is the predicate that says so.
    layout.align = 0;
    return layout;
  }
  return layout;
}

// The child a type holds *outside* its member sequence: a pointee for the three
// constructors that have one, and a return type for a function. Every other kind
// is made of its members and nothing else.
//
// The span points into the `Type` being built, which outlives the call that
// builds it -- the one place a `Type` is read while it is *not* in the store yet,
// and the reason this is a free function here rather than a member of `Type`.
[[nodiscard]] static std::span<const TypeId> singleChild(const Type& type) {
  switch (type.kind) {
  case TypeKind::Pointer:
  case TypeKind::Array:
  case TypeKind::Slice:
    return std::span<const TypeId>(&type.pointee, 1);
  case TypeKind::Function:
    return std::span<const TypeId>(&type.returnType, 1);
  default:
    return {};
  }
}

std::uint32_t TypeStore::nodesOf(std::span<const TypeId> parts,
                                 std::span<const TypeId> children) const {
  // Saturated at one past the bound: the arithmetic is on a structure a caller is
  // asking about, and the answer is only ever compared against the bound.
  constexpr std::uint32_t kOver = support::kMaxTypeNodes + 1;
  std::uint32_t nodes = 1;
  for (const TypeId child : children) {
    nodes += childNodes(child);
    if (nodes > support::kMaxTypeNodes) {
      return kOver;
    }
  }
  for (const TypeId part : parts) {
    nodes += childNodes(part);
    if (nodes > support::kMaxTypeNodes) {
      return kOver;
    }
  }
  return nodes;
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
  // A type parameter's identity is `(unit, owner, binder)` and not its structure,
  // so all three are mixed: two declarations that each call their binder `T` must
  // not land in one bucket, and `spelling` is deliberately left out because it is a
  // third fact about the type and not part of what makes two of them the same.
  //
  // `unit` is mixed for the reason the other two are, and it is the one that was
  // missing: an id is an index into *one* file's tree, so `(owner, binder)` names a
  // binder per file and the store is per compilation.
  hash = mix(hash, type.unit);
  hash = mix(hash, type.owner);
  hash = mix(hash, type.binder);
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
         a.name == b.name && a.variadic == b.variadic && a.paramCount == b.paramCount &&
         a.owner == b.owner && a.binder == b.binder && a.unit == b.unit;
}

bool TypeStore::equalParts(const Type& type, std::span<const TypeId> parts) const {
  if (type.paramCount != parts.size()) {
    return false;
  }
  for (std::uint32_t i = 0; i < type.paramCount; ++i) {
    if (sequences_[type.sequence][i] != parts[i]) {
      return false;
    }
  }
  return true;
}

TypeId TypeStore::intern(const Type& type) {
  return internSequence(type, partsOf(type));
}

TypeId TypeStore::internSequence(const Type& type, std::span<const TypeId> parts) {
  // The structural bound, and the only place it is enforced for the kinds that
  // reach the store one at a time -- `*T`, `[N]T`, `[]T` and every scalar. The two
  // kinds with a *member list* ask it in their own builders, before they lay the
  // structure out (`tupleOf`, `function`), because laying out a structure whose
  // members were reused doubles per level and the point of the bound is to be
  // reached before that arithmetic rather than after it.
  //
  // It is *not* compared for identity: how large a structure is follows from its
  // children, and hashing or comparing it would make two identical types with
  // different histories unequal (`hashOf`, which mixes what a type *is*).
  const std::uint32_t nodes = nodesOf(parts, singleChild(type));
  if (nodes > support::kMaxTypeNodes) {
    return kInvalidType;
  }
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
  // The sequence is appended only now, and the row it takes is the one the type
  // records: a caller that repeated an existing type never reached this line, so
  // a signature written twice does not grow the table (`function`'s rule, and now
  // the tuple's as well).
  //
  // `parts` may be a span over a **row** -- `intern` is called with `partsOf` of a
  // type the store already holds, which is what substituting a tuple does -- and
  // appending to a `deque` leaves that span pointing where it did.
  const TypeId id{static_cast<std::uint32_t>(types_.size())};
  Type stored = type;
  stored.sequence = static_cast<std::uint32_t>(sequences_.size());
  stored.nodes = nodes;
  // The layout is computed here, once, from the children's -- which is the whole
  // point of storing it: the same question is asked again by every use of this
  // type, and this store is a DAG, so asking it by walking would cost the *tree*
  // the type spells out every single time (`type.h`, on `Type::size`).
  const Layout layout = layoutOf(type, parts);
  stored.size = layout.size;
  stored.align = layout.align;
  stored.unknownSize = layout.unknownSize;
  sequences_.emplace_back(parts.begin(), parts.end());
  types_.push_back(stored);
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
  if (!isObject(element)) {
    return kInvalidType;
  }
  // An element with no size *yet* -- a type parameter, or a product holding one
  // -- skips the arithmetic instead of failing it: `[4]T` says nothing false
  // about the store, and the count and the element are checked again on the
  // substituted type, which is the type that reaches a module. Asking this
  // through `arraySize` would refuse the template with a sentence about a size,
  // which is the one thing that is not yet known.
  if (!hasUnknownSize(element) && !arraySize(element, count).has_value()) {
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

TypeId TypeStore::param(support::FileId unit, std::uint32_t owner, std::uint32_t binder,
                        std::string_view spelling, support::ConstraintClass klass) {
  paramSpellings_.emplace_back(spelling);
  Type type;
  type.kind = TypeKind::Param;
  type.unit = unit;
  type.owner = owner;
  type.binder = binder;
  type.paramSpelling = paramSpellings_.back();
  type.binderClass = klass;
  // Interned like every other type, and the class is not compared for identity
  // (`equalFields` mixes the triple above and not this) -- so a binder that is read
  // twice comes back as one type, which is what "the identity is
  // `(unit, owner, binder)`" has to mean. The class is a function of that triple
  // -- a binder list is read once, from one piece of source -- so the second read
  // writes the class the first one did.
  //
  // The triple is not a detail. A `Param` used to be keyed by `(owner, binder)`, on
  // the assumption that a store's scope is one unit; a store is one per
  // *compilation*, and a tree is numbered from zero per file, so the first binder
  // declared at node 5 of one input answered for the binder declared at node 5 of
  // another -- with the wrong class, silently (`type.h`, `unit`).
  return intern(type);
}

support::ConstraintClass TypeStore::binderClass(TypeId id) const {
  if (!isParam(id)) {
    return support::ConstraintClass::Any;
  }
  return get(id).binderClass;
}

bool TypeStore::satisfies(support::ConstraintClass klass, TypeId type) const {
  if (!type.valid() || isError(type) || isParam(type)) {
    return true;
  }
  switch (klass) {
  case support::ConstraintClass::Any:
    return true;
  case support::ConstraintClass::Eq:
    return isScalar(type);
  case support::ConstraintClass::Ordered:
  case support::ConstraintClass::Number:
    // One predicate for two classes, and that is the design and not a shortcut: a
    // class is members *and* grants, and these two admit the same types while
    // promising different operations (`support/constraint`'s header). Writing them
    // as two branches would invite them to drift into two different sets of types,
    // which is the one thing that is *not* free to change.
    return isArithmetic(type);
  case support::ConstraintClass::Integer:
    return isInteger(type);
  case support::ConstraintClass::Float:
    return isFloat(type);
  case support::ConstraintClass::Pointer:
    return isPointer(type);
  }
  return true;
}

bool TypeStore::hasConstant(TypeId id, support::TypeConstant constant) const {
  if (!known(id) || isError(id)) {
    return false;
  }
  const Type& shape = get(id);
  // Two groups, and the split is the vocabulary's: the four every number has, and
  // the three a float alone has. `char` is an integer here (it *is* an unsigned
  // byte, by decision), so it takes the first group with the rest of them --
  // `char::MAX` is 255 and not a question.
  const bool anyNumber =
      constant == support::TypeConstant::Zero || constant == support::TypeConstant::One ||
      constant == support::TypeConstant::Min || constant == support::TypeConstant::Max;
  const bool floatOnly = constant == support::TypeConstant::Epsilon ||
                         constant == support::TypeConstant::Infinity ||
                         constant == support::TypeConstant::Nan;
  switch (shape.kind) {
  case TypeKind::Int:
  case TypeKind::Char:
    return anyNumber;
  case TypeKind::Float:
    return anyNumber || floatOnly;
  default:
    // A binder is not judged here on purpose: an abstract type has a constant
    // when its class grants one, and the checker asks that question first.
    return false;
  }
}

TypeId TypeStore::substitute(TypeId subject, std::span<const TypeId> args, support::FileId unit,
                             std::uint32_t owner) {
  if (!known(subject)) {
    return subject;
  }
  // By value, and not a reference: building the result interns, and interning can
  // grow the very vector this type lives in.
  const Type type = get(subject);
  switch (type.kind) {
  case TypeKind::Param:
    // The one place a substitution ends. A binder of *another* declaration -- in
    // this unit or any other -- keeps its identity, which is what makes a generic
    // body that mentions an enclosing binder still be about that binder
    // (decision 6). The unit is compared before the node id, and it is not
    // bookkeeping: both are per-file coordinates, and a definition of this
    // declaration living in another file would otherwise be substituted by this
    // one's arguments (`type.h`, `unit`).
    if (type.unit != unit || type.owner != owner || type.binder >= args.size()) {
      return subject;
    }
    return args[type.binder];
  case TypeKind::Pointer: {
    const TypeId pointee = substitute(type.pointee, args, unit, owner);
    return pointee.valid() ? pointerTo(pointee) : kInvalidType;
  }
  case TypeKind::Array: {
    // Rebuilt through `arrayOf`, so the count and the element are checked *again*
    // against the substituted type: `[4]T` with `T := i32` is an array the store
    // is allowed to have, and `[4]T` with `T := (i32, bool)` is refused here, by
    // the same arithmetic that would have refused it if it had been written.
    const TypeId element = substitute(type.pointee, args, unit, owner);
    return element.valid() ? arrayOf(element, type.count) : kInvalidType;
  }
  case TypeKind::Slice: {
    const TypeId element = substitute(type.pointee, args, unit, owner);
    return element.valid() ? sliceOf(element) : kInvalidType;
  }
  case TypeKind::Tuple: {
    // The members are copied out before the walk: every `substitute` below can
    // intern, and interning appends to the same member array this span points
    // into.
    const std::vector<TypeId> original(membersOf(subject).begin(), membersOf(subject).end());
    std::vector<TypeId> replaced;
    replaced.reserve(original.size());
    for (const TypeId member : original) {
      const TypeId one = substitute(member, args, unit, owner);
      if (!one.valid()) {
        return kInvalidType;
      }
      replaced.push_back(one);
    }
    return tupleOf(replaced);
  }
  case TypeKind::Function: {
    const std::vector<TypeId> original(paramsOf(subject).begin(), paramsOf(subject).end());
    std::vector<TypeId> replaced;
    replaced.reserve(original.size());
    for (const TypeId param : original) {
      const TypeId one = substitute(param, args, unit, owner);
      if (!one.valid()) {
        return kInvalidType;
      }
      replaced.push_back(one);
    }
    const TypeId returns = substitute(type.returnType, args, unit, owner);
    if (!returns.valid()) {
      return kInvalidType;
    }
    return function(returns, replaced, type.variadic);
  }
  default:
    // Everything else is a type with no part that can be a binder.
    return subject;
  }
}

TypeId TypeStore::function(TypeId returnType, std::span<const TypeId> params, bool variadic) {
  // The structural bound **before** anything reads the structure: a signature is
  // the second shape that can be doubled per level by reusing a type, and the
  // refusal has to be cheaper than the work it prevents (`limits.h`).
  if (nodesOf(params, std::span<const TypeId>(&returnType, 1)) > support::kMaxTypeNodes) {
    return kInvalidType;
  }
  Type type;
  type.kind = TypeKind::Function;
  type.returnType = returnType;
  // The count is what the comparison below reads for the candidates already in
  // the table; the row the parameters take is assigned in `internSequence`, after
  // the search -- so a repeated signature does not grow the table, which is why
  // this cannot be a plain `intern`.
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
  bool unknown = false;
  for (const TypeId member : members) {
    if (!isObject(member)) {
      return kInvalidType;
    }
    unknown = unknown || hasUnknownSize(member);
  }
  // The structural bound, before the layout arithmetic below: `tupleSize` walks
  // the members' own structures, so a product that reuses a large type twice --
  // `(P, P)` where `P` was built the same way -- would pay for the whole
  // structure at every level. Asked here, the cost is one step per member.
  if (nodesOf(members) > support::kMaxTypeNodes) {
    return kInvalidType;
  }
  // The layout arithmetic, for the same reason the array does it: a product whose
  // size does not fit `size_t` is refused where it is built, so `sizeOf` below is
  // a number and not a question. It also means the size and every member offset
  // are computed once per type and never overflow anywhere else.
  //
  // A member with no size yet is the exception, and it is the same one the array
  // makes: `(T, K)` is the *template* of `Pair<T, K>`, and its layout is a
  // question for the substituted product -- which `substitute` builds through
  // this function, so the check is applied to the type that gets lowered.
  if (!unknown && !tupleSize(members).has_value()) {
    return kInvalidType;
  }
  Type type;
  type.kind = TypeKind::Tuple;
  type.paramCount = static_cast<std::uint32_t>(members.size());
  return internSequence(type, members);
}

std::optional<std::size_t> TypeStore::tupleSize(std::span<const TypeId> members) const {
  std::size_t align = 1;
  std::size_t size = 0;
  for (const TypeId member : members) {
    if (hasUnknownSize(member)) {
      // Not a size that is not a number: a size that does not exist yet. The
      // sentence for it belongs to the position that asked (`sizeof` of a
      // binder, an array of one), and the answer here is the honest "no".
      return std::nullopt;
    }
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
bool TypeStore::isParam(TypeId id) const {
  return known(id) && get(id).kind == TypeKind::Param;
}
bool TypeStore::isParamOf(TypeId id, support::FileId unit, std::uint32_t owner) const {
  return isParam(id) && get(id).unit == unit && get(id).owner == owner;
}
bool TypeStore::hasUnknownSize(TypeId id) const {
  // Stored with the layout, and the rule is `layoutOf`'s: a binder has no width
  // yet, an aggregate built out of one has none either, and a pointer or a view
  // has one whatever it points at (`slices.md` decision 5). Asked per position
  // that would otherwise do arithmetic on a width -- an array's element product,
  // a product's layout -- so it is answered by a read.
  return known(id) && get(id).unknownSize;
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
  if (kind == TypeKind::Param) {
    // A `T` is an object in this predicate's sense -- it *is* a type argument, and
    // an argument is an object -- which is what lets a generic body bind, pass
    // and return one (`generics.md`). It is deliberately not `isScalar`: "one
    // word" is not a thing a binder promises.
    return true;
  }
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
  case TypeKind::Param:
    // The binder's own name, which is the third fact stored beside the identity
    // and the only one a diagnostic has any use for (`generics.md`).
    return std::string(type.paramSpelling);
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
  // **Read, never walked.** The size is computed once, when the type is built,
  // and `layoutOf` is where the rule lives (`type.h`, on `Type::size`): this
  // store is a DAG, so a type can spell out a structure many times its own size,
  // and the size of a type is asked again by every use of it.
  //
  // 0 for `void`, a function, a deferred literal and the poison -- they have no
  // object representation -- and 0 for an aggregate whose width is not decided
  // yet, which is the case `hasUnknownSize` tells apart from the first.
  return known(id) ? get(id).size : 0;
}

std::size_t TypeStore::alignOf(TypeId id) const {
  // Stored beside the size, and 0 wherever there is no object representation:
  // `Bool`/`Char` 1, an integer its target alignment, a float its ABI slot
  // (`f80` included), a pointer the pointer width, an array and a product their
  // strictest member's. The rule is `layoutOf`'s.
  return known(id) ? get(id).align : 0;
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
