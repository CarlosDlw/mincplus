// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The access record: what the optimizer may assume at a place reached through a
// pointer, and the vocabulary it is written in.
//
// `memory.md` states one obligation per access and one *closed list* of
// assumptions the compiler may hand the optimizer. This file is the second half
// of that: the checker writes the answer down at the node that denotes the
// access, and the lowering reads it there. Re-deriving it later would mean a
// second copy of the aliasing rule, which is exactly the class of thing the
// model exists to make impossible.
//
// The rules, in one place:
//
//   * an access is recorded at the node the lowering will be standing on --
//     `*p`, and `p[i]` -- and a node that denotes no access has no entry;
//   * `provenanceOf` is a *proof*, not a guess, and the two answers are the two
//     ends of it. It recognises the address of an object this unit named, moved
//     only by arithmetic since (`&x`, `&x + n`, `*(&x + n)`), and it answers
//     `Foreign` for everything else: a parameter, a value read from memory, a
//     value returned by a call. It is syntactic, so it is sound and incomplete
//     by construction, and the direction it errs in is the only safe one --
//     `Foreign` is the answer that assumes nothing.
#include "checker.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace minc::sema {
namespace {

// The access kinds, in one table: an enumerator added without a row is a compile
// error here, and a row without an enumerator is caught by `allAccessKinds()`
// being derived from the table and a test comparing the two sizes.
// NOLINTBEGIN(readability-identifier-naming): table name follows the project's
// convention for the other stages' tables.
constexpr std::array<AccessKindInfo, 1> kAccessKindInfos{{
    {AccessKind::Ordinary, "ordinary"},
}};

constexpr std::array<ProvenanceKindInfo, 2> kProvenanceKindInfos{{
    {ProvenanceKind::Object, "object"},
    {ProvenanceKind::Foreign, "foreign"},
}};

// The three extents, in the order a reader asks about them: nothing, a count in
// a type, a length in a descriptor. The names are the words the lowering and the
// dumps use, and the third one exists because a slice's bounds check compares
// against a value -- the first draft had only the first two, which left `s[i]`
// unchecked while `a[i]` on a four-element array was checked (`checks.md`).
// The first row is spelled `none` and **not** `unknown`: `toString` answers
// `"unknown"` for a kind that has no row, so a row named after the fallback would
// make "the table has a problem" and "this access has no extent" the same string.
constexpr std::array<ExtentKindInfo, 3> kExtentKindInfos{{
    {ExtentKind::Unknown, "none"},
    {ExtentKind::Count, "count"},
    {ExtentKind::Length, "length"},
}};
// NOLINTEND(readability-identifier-naming)

template <std::size_t... Indexes>
[[nodiscard]] constexpr auto accessKindsFromTable(std::index_sequence<Indexes...>) {
  return std::array<AccessKind, sizeof...(Indexes)>{kAccessKindInfos[Indexes].kind...};
}

template <std::size_t... Indexes>
[[nodiscard]] constexpr auto provenanceKindsFromTable(std::index_sequence<Indexes...>) {
  return std::array<ProvenanceKind, sizeof...(Indexes)>{kProvenanceKindInfos[Indexes].kind...};
}

template <std::size_t... Indexes>
[[nodiscard]] constexpr auto extentKindsFromTable(std::index_sequence<Indexes...>) {
  return std::array<ExtentKind, sizeof...(Indexes)>{kExtentKindInfos[Indexes].kind...};
}

constexpr auto kAllAccessKinds =
    accessKindsFromTable(std::make_index_sequence<kAccessKindInfos.size()>{});
constexpr auto kAllProvenanceKinds =
    provenanceKindsFromTable(std::make_index_sequence<kProvenanceKindInfos.size()>{});
constexpr auto kAllExtentKinds =
    extentKindsFromTable(std::make_index_sequence<kExtentKindInfos.size()>{});

} // namespace

std::span<const AccessKindInfo> accessKindInfos() {
  return kAccessKindInfos;
}

std::span<const AccessKind> allAccessKinds() {
  return kAllAccessKinds;
}

std::string_view toString(AccessKind kind) {
  for (const AccessKindInfo& info : kAccessKindInfos) {
    if (info.kind == kind) {
      return info.name;
    }
  }
  return "unknown";
}

std::span<const ProvenanceKindInfo> provenanceKindInfos() {
  return kProvenanceKindInfos;
}

std::span<const ProvenanceKind> allProvenanceKinds() {
  return kAllProvenanceKinds;
}

std::string_view toString(ProvenanceKind kind) {
  for (const ProvenanceKindInfo& info : kProvenanceKindInfos) {
    if (info.kind == kind) {
      return info.name;
    }
  }
  return "unknown";
}

std::span<const ExtentKindInfo> extentKindInfos() {
  return kExtentKindInfos;
}

std::span<const ExtentKind> allExtentKinds() {
  return kAllExtentKinds;
}

std::string_view toString(ExtentKind kind) {
  for (const ExtentKindInfo& info : kExtentKindInfos) {
    if (info.kind == kind) {
      return info.name;
    }
  }
  return "unknown";
}

// --- the proof ---------------------------------------------------------------

ProvenanceKind Checker::provenanceOf(ast::AstId expr) const {
  if (!expr.valid() || inError(expr)) {
    return ProvenanceKind::Foreign;
  }
  switch (kindOf(expr)) {
  case ast::NodeKind::ParenExpr: {
    // Parentheses do not change what an expression denotes.
    const std::vector<ast::AstId> operands = operandsOf(expr);
    return operands.empty() ? ProvenanceKind::Foreign : provenanceOf(operands.front());
  }
  case ast::NodeKind::PrefixExpr: {
    // The one producer of a provenance this stage can name: `&x` is the address
    // of an object the unit declared. `*p` is the opposite case -- the value came
    // out of memory, and memory can hold anything.
    const ast::AstId op = tokenOf(expr);
    if (op.valid() && tagOf(kindOf(op)) == kTokAmp) {
      return ProvenanceKind::Object;
    }
    return ProvenanceKind::Foreign;
  }
  case ast::NodeKind::SliceExpr: {
    // Slicing does not invent provenance: a view of an object the unit named is
    // still an access inside that object, and a view of anything else is unknown
    // (`slices.md` decision 20).
    //
    // The three bases are three different questions. An **array** is not a
    // pointer value at all -- there is nothing for `provenanceOf` to answer -- so
    // it is the subscript's own question, one level down. A **pointer** carries
    // whatever the expression that produced it carried, so `&table[0]` stays
    // `object` and `p` from a parameter stays `foreign`. A **slice** is a value,
    // and the pointer word inside it is not something this pass follows -- which
    // is exactly what "unknown otherwise" means, and it is the conservative
    // answer.
    const ast::SliceParts parts = file_.slicePartsOf(expr);
    if (!parts.hasBase()) {
      return ProvenanceKind::Foreign;
    }
    const TypeId baseType = out_.typed.typeOf(parts.base);
    if (types_.isArray(baseType)) {
      return placeProvenanceOf(parts.base);
    }
    if (types_.isPointer(baseType)) {
      return provenanceOf(parts.base);
    }
    return ProvenanceKind::Foreign;
  }
  case ast::NodeKind::BinaryExpr: {
    const std::vector<ast::AstId> operands = operandsOf(expr);
    if (operands.size() < 2) {
      return ProvenanceKind::Foreign;
    }
    // Pointer arithmetic preserves provenance exactly, in both directions --
    // `p + n` is `n + p` -- and only the pointer operand's answer matters. A
    // pointer the compiler cannot name does not become nameable by adding to it.
    for (const ast::AstId operand : operands) {
      if (types_.isPointer(out_.typed.typeOf(operand))) {
        return provenanceOf(operand);
      }
    }
    return ProvenanceKind::Foreign;
  }
  default:
    // A parameter's value, a value loaded from memory, a call's result. All of
    // them are unknown, and unknown is the answer that assumes nothing.
    return ProvenanceKind::Foreign;
  }
}

// The provenance of an access *inside a place* -- an array subscript (`a[i]`), a
// view of one (`a[1..2]`) and a product member (`t.0`) all ask this, and it is a
// different question from `provenanceOf`'s, answered differently on purpose.
//
// `provenanceOf(a)` asks what allocation a *pointer value* came from, and a path
// naming an array is not a pointer value at all -- there is nothing to ask. What
// `a[i]` needs is the allocation the *place* is inside, and the answer follows
// from what the object is:
//
//   * a binding of this unit's own (`let t: [4]i32;`, a file-scope table) is an
//     object the unit named, so the access is inside it and the extent is real;
//   * a **parameter** is not: the pointer came in from the caller, and the
//     compiler cannot see the object it names (`memory.md`, *Provenance*). The
//     caller copied the array, so the access is in bounds -- but "in bounds" is a
//     fact about the call, not something this unit can prove;
//   * `(*p)[i]` -- a pointer to an array -- is behind a pointer value, so it is
//     `Foreign` for the same reason `p[i]` is, and only the extent survives,
//     because the count is in the type.
ProvenanceKind Checker::placeProvenanceOf(ast::AstId base) const {
  const std::optional<resolve::DefId> def = defOfPlace(base);
  if (!def.has_value() || def->index >= defs_.defs.size()) {
    return ProvenanceKind::Foreign;
  }
  // The declaration's kind is the answer, and it is one comparison rather than a
  // second walk of the tree: a parameter's storage belongs to the caller, and
  // everything else the unit named is its own object.
  return defs_.defs[def->index].kind == resolve::DefKind::Parameter ? ProvenanceKind::Foreign
                                                                    : ProvenanceKind::Object;
}

void Checker::recordAccess(ast::AstId place, TypeId type, ProvenanceKind provenance,
                           std::uint64_t extent, ExtentKind extentKind) {
  if (!place.valid() || !type.valid()) {
    return;
  }
  // A refused access describes a program that was already reported, and the tree
  // is never lowered. Recording it would put an obligation in the artifact for a
  // node whose type is the poison -- and the poison has no size, so the entry
  // could not be materialised.
  if (types_.isError(type) || types_.isVoid(type)) {
    return;
  }
  out_.typed.addAccess(
      AccessObligation{place, type, AccessKind::Ordinary, provenance, extent, extentKind});
}

} // namespace minc::sema
