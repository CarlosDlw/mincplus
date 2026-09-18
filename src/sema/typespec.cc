// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "sema/typespec.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "support/limits.h"

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
// exactly what the feature checklist promises: the combined `uint`, and the
// compiler's
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

// The one failure that is neither a malformed type nor an unknown word: an argument
// outside its binder's class (`TypeSpecResult::constraintViolation`). Its own maker
// because it is its own *code* at the caller, and because the message it carries is
// about a declaration and a use rather than about this position.
[[nodiscard]] TypeSpecResult constraintFailure(std::string message) {
  TypeSpecResult result = fail(std::move(message));
  result.constraintViolation = true;
  return result;
}

[[nodiscard]] TypeSpecResult ok(TypeId type) {
  TypeSpecResult result;
  result.type = type;
  result.ok = true;
  return result;
}

// The run was understood and names a type that is *already reported broken*: a
// name this unit declared whose own expansion failed. `ok` because nothing is
// wrong with the spelling, no type because there is none to hand over, and the
// flag because the caller's silence is the point -- the declaration is the fault.
[[nodiscard]] TypeSpecResult brokenName() {
  TypeSpecResult result;
  result.type = kInvalidType;
  result.ok = true;
  result.brokenName = true;
  return result;
}

[[nodiscard]] std::string quoted(std::string_view word) {
  return "`" + std::string(word) + "`";
}

} // namespace

const TypeName* findTypeName(std::span<const TypeName> names, std::string_view word) {
  // **Backwards, because the table is a stack.** A block may declare a name the
  // file has already spent, and the row that answers a use inside that block is
  // the one the block published -- the latest. A block drops the rows it pushed
  // when it ends, so the same lookup is right outside it too, and the direction of
  // this loop is the whole of the shadowing rule (`type_alias.md`, decision 5).
  for (auto row = names.rbegin(); row != names.rend(); ++row) {
    if (row->spelling == word) {
      return &*row;
    }
  }
  return nullptr;
}

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

namespace {

void collectWords(std::span<const TypePart> parts, std::vector<std::string_view>& out) {
  for (const TypePart& part : parts) {
    if (part.isStar || part.isArray) {
      continue; // a constructor is not a name
    }
    if (part.isTuple) {
      // A member is a type position, so its words are words of this run: the
      // dependency walk has to see them, and in the order they were written.
      for (const std::vector<TypePart>& member : part.members) {
        collectWords(member, out);
      }
      continue;
    }
    if (!part.word.empty()) {
      out.push_back(part.word);
    }
    // The arguments of a use are names this run mentions: `type A = Pair<B, i32>;`
    // depends on `B` exactly as `type A = *B;` does, and a walk that skipped them
    // would decide `A` before `B` and then fail to read it.
    for (const std::vector<TypePart>& argument : part.args) {
      collectWords(argument, out);
    }
  }
}

} // namespace

std::vector<std::string_view> typeRunWords(std::span<const TypePart> parts) {
  std::vector<std::string_view> out;
  collectWords(parts, out);
  return out;
}

bool typeNameOnTarget(std::string_view name, const TargetInfo& target) {
  const Primitive* primitive = findPrimitive(name);
  // Every word that is not a primitive is a type on every target: the C
  // specifiers are read by rule, and `isize`/`usize` and the `size_t` family take
  // their width *from* the target rather than being refused by it. The one entry
  // the target decides is the format (`f80`), and the C spellings that reach it
  // (`long double`) are the portable answer and not the word being asked about.
  return primitive == nullptr || primitive->bits != 80 || target.hasFloat80();
}

namespace {

// The constructors of a run, applied **inside out**: the one nearest the words is
// the innermost, so the list is walked in reverse and `**i32` is a pointer to a
// pointer while `[2][3]i32` is two arrays of three.
//
// Shared by both bases a run can have -- a sequence of words and a product --
// because `*i32` and `*(i32, bool)` are the same construction, and the array's
// four refusals must not exist twice.
TypeSpecResult applyConstructors(const TypeSpecResult& base,
                                 const std::vector<const TypePart*>& constructors, TypeStore& types,
                                 std::optional<std::uint64_t> inferredCount) {
  TypeId result = base.type;
  for (auto it = constructors.rbegin(); it != constructors.rend(); ++it) {
    const TypePart& part = **it;
    if (part.isStar) {
      result = types.pointerTo(result);
      if (!result.valid()) {
        // The type budget, reported by the caller through the same check the base
        // reader relies on: `ok` with an invalid id means "understood, but the
        // store refused to intern it", and the caller owns that diagnostic.
        return ok(kInvalidType);
      }
      continue;
    }
    // The array, and its four refusals. Each is named, because "invalid array
    // type" is not something a reader can repair -- and each is refused *here*,
    // in the stage that has the count and the element in hand, so no later stage
    // ever meets an array whose size is not a number (`arrays.md` decisions 4, 5,
    // 17, 20).
    //
    // The count, from the one of the three places it can come from: the source,
    // or the initializer for a `_`. Checked in the order of "what was written",
    // so a `[]` is never blamed on a missing initializer and a `[_]` never reads
    // as a slice.
    std::uint64_t count = part.count;
    if (part.countInferred) {
      if (!inferredCount.has_value()) {
        return fail("`_` is the count an initializer takes from its own elements: "
                    "`[_]i32{1, 2, 3}`. Here there is no initializer, so the count has to be "
                    "written, as in `[3]i32`");
      }
      if (&part != constructors.front()) {
        // Only the outermost, and the reason is structural: with an inner `_`
        // each row of `[2][_]i32{...}` could count its own elements, and rows of
        // different lengths are not a type.
        return fail("only the outermost count of an initializer can be `_`: an inner `_` "
                    "would let every row have its own length, and the rows of an array are "
                    "one array");
      }
      count = *inferredCount;
    } else if (!part.hasCount) {
      // `[]T`, the slice: a view, so none of the count rules apply and the one
      // element rule does. Refused here rather than by the store so the sentence
      // can name the element -- the same reason the array's four refusals live in
      // this loop (`slices.md`).
      if (!types.isObject(result)) {
        return fail("`" + types.spelling(result) +
                    "` cannot be a slice element: a view has to be a view of something "
                    "that can be stored, whose size is a number");
      }
      result = types.sliceOf(result);
      if (!result.valid()) {
        return ok(kInvalidType);
      }
      continue;
    }
    if (part.countOverflow) {
      return fail("the count of an array type has to be a number that fits in 64 bits");
    }
    if (count == 0) {
      return fail("the count of an array type is at least 1: an object of no elements has no "
                  "address and no size");
    }
    if (!types.isObject(result)) {
      return fail("`" + types.spelling(result) +
                  "` cannot be an array element: an element has to be a type that can be "
                  "stored");
    }
    // An element with no width *yet* -- a type parameter, or a product holding
    // one -- skips the arithmetic: `[4]T` is a perfectly good array *type*, and
    // the number the refusal is about is the argument's, which is not known until
    // the declaration is instantiated. `substitute` builds the result through
    // `arrayOf`, so the arithmetic is applied to the type that reaches a module
    // (`generics.md`, decision 8).
    if (!types.hasUnknownSize(result) && !types.arraySize(result, count).has_value()) {
      return fail("an array of " + std::to_string(count) + " `" + types.spelling(result) +
                  "` is larger than this target can address");
    }
    result = types.arrayOf(result, count);
    if (!result.valid()) {
      return ok(kInvalidType);
    }
  }
  return ok(result);
}

} // namespace

TypeSpecResult readType(std::span<const TypePart> parts, TypeStore& types,
                        std::span<const TypeName> names,
                        std::optional<std::uint64_t> inferredCount) {
  // `!` first, because it is the one accepted spelling that is not a run of
  // words under some stars: it is a whole type on its own, and anything beside
  // it is a spelling with no meaning to give.
  //
  // The two messages are different on purpose. `!` as the whole run is a type;
  // `*!`, `!i32` and `! !` are attempts to combine it, and the fix is to stop
  // combining -- usually by naming the type the expression would have had, or
  // by calling the function for its effect and not for its value.
  if (parts.size() == 1 && parts.front().isBang) {
    return ok(kTypeNever);
  }
  for (const TypePart& part : parts) {
    if (part.isBang) {
      return fail("`!` is a type on its own: it cannot be combined with a type name or a `*`");
    }
  }

  // The constructors, then the words. Both constructors are prefixes over
  // everything to their right, so one rule reads both orders: `*[4]i32` is a
  // pointer to an array, `[4]*i32` is an array of pointers, and neither needs a
  // grammar of its own (`arrays.md`, *The surface*).
  std::vector<const TypePart*> constructors;
  constructors.reserve(parts.size());
  std::vector<std::string_view> words;
  words.reserve(parts.size());
  const TypePart* tuplePart = nullptr;
  // The base a use of a generic name produced, already substituted. Set in the
  // loop above and applied with the constructors below, exactly like a product's
  // base -- the two are the only bases a run can have.
  TypeId usedBase = kInvalidType;
  bool sawWord = false;
  for (const TypePart& part : parts) {
    if (part.isStar || part.isArray) {
      if (sawWord) {
        // A constructor *after* the words: `i32*` and `i32[4]` are the two
        // shapes, and both are one mistake with one fix. Refused here rather than
        // left to produce "`[` is not a type" about a token the parser already
        // accepted as part of the type, because the reader's intent is not in
        // doubt -- only the side the constructor belongs on.
        return fail(part.isArray
                        ? "an array type is written `[N]T`, with the `[N]` before the element type"
                        : "a pointer type is written `*T`, with the `*` before the type it "
                          "points to");
      }
      constructors.push_back(&part);
      continue;
    }
    if (part.isTuple) {
      // The nesting bound, reported where the group was built: a type that nests
      // deeper than the parser's own limit is a refusal, not a recursion this
      // reader performs on a tree that came from somewhere else.
      if (part.tooDeep) {
        return fail("this type nests deeper than the compiler follows: name the inner type "
                    "with a `type` declaration, or flatten the product a level");
      }
      if (tuplePart != nullptr) {
        // `(i32, i32)(bool, bool)`: two products in one run have no meaning to
        // give -- there is no operation that would join them.
        return fail("a type position holds one type: two products cannot sit next to each "
                    "other");
      }
      if (sawWord) {
        // A product beside a type name or a primitive, in either order. A tuple is
        // a whole type: the fix is to name what the two were meant to be.
        return fail("`(T, U)` is a whole type: it cannot be combined with a type name in the "
                    "same type position");
      }
      tuplePart = &part;
      sawWord = true;
      continue;
    }
    if (tuplePart != nullptr) {
      // The words came *after* the product (`(i32, bool) i32`).
      return fail("`(T, U)` is a whole type: it cannot be combined with a type name in the "
                  "same type position");
    }
    if (part.hasArgs) {
      // `Pair<i32, bool>`: a **use** of a generic name (`generics.md`).
      //
      // It is a base of the run and not a prefix over it, for the same reason a
      // product is: the substitution produces a whole type, so `*Pair<i32, bool>`
      // is a pointer to one and there is no half-built state for a constructor to
      // wrap.
      if (sawWord || usedBase.valid()) {
        return fail("a type with arguments is one type: it cannot be combined with another "
                    "type name in the same type position");
      }
      if (part.word.empty()) {
        // Unreachable from the grammar -- a list of arguments is always written
        // against a name -- and spelled anyway so a tree from somewhere else costs
        // a sentence and not a wrong lookup of the empty name.
        return fail("a list of type arguments belongs to a type name: `Pair<i32, bool>`");
      }
      // The arguments are type positions of their own, read by this same reader,
      // so `Pair<i32, Vec<u8>>` nests without a rule of its own.
      std::vector<TypeId> arguments;
      arguments.reserve(part.args.size());
      for (const std::vector<TypePart>& argument : part.args) {
        TypeSpecResult read = readType(argument, types, names, std::nullopt);
        if (!read.ok) {
          // The inner sentence is the whole answer: it names the word that was
          // wrong, and re-stating it here would be two messages for one mistake.
          return read;
        }
        if (!read.type.valid()) {
          return brokenName();
        }
        arguments.push_back(read.type);
      }
      const TypeName* row = findTypeName(names, part.word);
      if (row == nullptr) {
        return fail("expected a type name where `" + std::string(part.word) +
                        "` is: a list of type arguments belongs to a type this unit declared",
                    part.word);
      }
      if (row->binders == 0) {
        // The name is a type and takes nothing. Named rather than left to "this
        // is not a type", because the reader wrote a name the unit *has* -- the
        // mistake is the list, and the fix is to delete it.
        return fail("`" + std::string(part.word) +
                    "` takes no type arguments: it is a name for one type, and the `type` that "
                    "declares it wrote no binders");
      }
      if (arguments.size() != row->binders) {
        return fail("`" + std::string(part.word) + "` takes " + std::to_string(row->binders) +
                    " type argument" + (row->binders == 1 ? "" : "s") + ", and " +
                    std::to_string(arguments.size()) + " were written");
      }
      if (!row->type.valid()) {
        // A name whose own expansion failed; reported where it failed.
        return brokenName();
      }
      // **The class check, at the use**, and this is the whole of what a constraint
      // on a generic `type` means: the declaration wrote which types may fill its
      // holes, and this is where a hole gets filled. Without it the class would be
      // decoration on an alias -- `Vec<bool>` for a `type Vec<T: Number>` -- and a
      // declaration that promises something no stage checks is exactly what this
      // compiler refuses to ship (`generics.md`, § 6).
      //
      // Before the substitution, because a list that cannot be filled needs no
      // target: the same order the `fn` half uses, and the same argument for it.
      for (std::size_t i = 0; i < arguments.size() && i < row->rows.size(); ++i) {
        if (types.satisfies(row->rows[i].klass, arguments[i])) {
          continue;
        }
        return constraintFailure("`" + std::string(types.spelling(arguments[i])) +
                                 "` cannot be the `" + std::string(row->rows[i].spelling) +
                                 "` of `" + std::string(part.word) +
                                 "`: the declaration says that binder is `" +
                                 std::string(support::constraintClassName(row->rows[i].klass)) +
                                 "`, and a type argument has to be one of the types that "
                                 "class admits");
      }
      usedBase = types.substitute(row->type, arguments, row->unit, row->owner);
      if (!usedBase.valid()) {
        // The substitution itself was refused. It is *not* the store's budget,
        // which reports itself: this is the target's own rules meeting the
        // arguments -- `[4]T` with `T := void`, or a count whose product does not
        // fit the address space. A sentence, because the reader wrote something
        // and nothing else will explain it (`generics.md`, decision 8).
        return fail("`" + std::string(part.word) +
                    "` with these arguments has no type: the arguments are substituted into "
                    "the declaration's target, and the result is refused -- an argument that "
                    "cannot be stored, or an object this target cannot address");
      }
      sawWord = true;
      continue;
    }
    sawWord = true;
    words.push_back(part.word);
  }
  if (usedBase.valid()) {
    return applyConstructors(ok(usedBase), constructors, types, inferredCount);
  }
  if (words.empty() && !constructors.empty() && tuplePart == nullptr) {
    // A constructor with nothing under it (`*`, `[4]`). The base reader would
    // answer "expected a type name", which is true and does not say which one:
    // the fix is to write the type the constructor is building around.
    return fail(constructors.back()->isArray
                    ? "expected the element type of the array: an array is written `[N]T`"
                    : "expected the type the pointer points to: a pointer is written `*T`");
  }

  // The product, built member by member: each member's run is read by this same
  // reader, with the same `names`, so an alias inside a product resolves exactly
  // as it does anywhere else and `type Pair = (Meters, bool);` needs no rule of
  // its own (`tuples.md`, decision 14).
  //
  // The two refusals are the store's, asked here because this is the stage that
  // can name the member: a product of one member *is* that member, and a member
  // that cannot be stored (an array of no elements cannot reach here -- it is
  // refused where its count is read) has no size to contribute.
  if (tuplePart != nullptr) {
    // The two arities a product cannot have, and they are two sentences because
    // they are two mistakes with two fixes: `()` is the empty group (a parameter
    // list, or the type with no value), and `(T,)` -- or `(T)` in a type position,
    // which is the same shape -- is a product of one member, which *is* that
    // member (`tuples.md`, decision 2).
    if (tuplePart->members.empty()) {
      return fail("a product has at least two members, `(T, U)`; for the type with no value "
                  "write `void`");
    }
    if (tuplePart->members.size() == 1) {
      return fail("a product of one member is that member: write the member's type, not "
                  "`(T,)`");
    }
    std::vector<TypeId> members;
    members.reserve(tuplePart->members.size());
    for (const std::vector<TypePart>& member : tuplePart->members) {
      TypeSpecResult read = readType(member, types, names, std::nullopt);
      if (!read.ok) {
        return read;
      }
      if (!read.type.valid()) {
        // `ok` with no type: either the store refused the member (the budget, the
        // caller's diagnostic) or the member names an expansion that already
        // failed (reported where it failed, and the flag has to travel).
        return read.brokenName ? read : ok(kInvalidType);
      }
      if (!types.isObject(read.type)) {
        return fail("`" + types.spelling(read.type) +
                    "` cannot be a member of a product: a member is stored, so it has to be "
                    "a type that can be stored");
      }
      members.push_back(read.type);
    }
    const TypeId product = types.tupleOf(members);
    if (!product.valid()) {
      // The four reasons `tupleOf` answers no: an arity under two (impossible
      // here, the reader refused it above), a member that is not an object (asked
      // member by member above), a product larger than the target can address,
      // and -- the one this reader is the only one able to say -- a product made
      // of too many types.
      //
      // The two are told apart by asking the store the structural question first,
      // and that is why the question is public: a product that reuses a large type
      // twice per level is a *different* mistake from one that is merely too big
      // for the target, and a reader sent looking for the wrong cause is what two
      // sentences prevent (`type_store.h`, on `nodesOf`).
      if (types.nodesOf(members) > support::kMaxTypeNodes) {
        return fail("this product is made of more than " + std::to_string(support::kMaxTypeNodes) +
                    " types: a product that holds a large one twice per level doubles per "
                    "level, so the structure is refused rather than laid out");
      }
      return fail("a product of " + std::to_string(members.size()) +
                  " members is larger than this target can address");
    }
    TypeSpecResult productResult;
    productResult.type = product;
    productResult.ok = true;
    // The constructors *around* the product go through the same rule as around a
    // run of words: `*[3](i32, bool)` is an array of products, and a `[_]` that
    // is outermost here really is the initializer's count. A `_` *inside* a
    // member was read as a member's own run, where it has no initializer to
    // count -- which is the sentence the member's read already produced.
    return applyConstructors(productResult, constructors, types, inferredCount);
  }

  // Not `const`: the failure path returns it, and a const local would force a
  // copy where the move is the point (`performance-no-automatic-move`).
  TypeSpecResult base = readTypeSpec(words, types, names);
  if (!base.ok) {
    return base;
  }
  if (!base.type.valid()) {
    // `ok` with no type, and the two reasons are different sentences the caller
    // owns: the store refusing the run is the type budget, while a name whose
    // expansion already failed was reported where it failed. **The flag travels
    // through here**, and this line is why: answering a fresh `ok(kInvalidType)`
    // would turn every use of a broken name into "too many distinct types", which
    // is a lie about a program that has three.
    return base.brokenName ? base : ok(kInvalidType);
  }
  return applyConstructors(base, constructors, types, inferredCount);
}

TypeSpecResult readTypeSpec(std::span<const std::string_view> words, TypeStore& types,
                            std::span<const TypeName> names) {
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

  // **The unit's own names, and only in the one shape a name has.** A type name is
  // a *single* word -- the constructors around it were stripped by `readType`, and
  // no C spelling has ever been a name for something else -- so this is a lookup
  // and not a third reader. It sits before the C state machine below because that
  // machine is the fallback for everything left, and a name the unit declared is
  // not an unknown word.
  //
  // The refusal to *declare* one of the language's type words is what keeps the
  // two tables from ever holding the same spelling -- `resolve` reports it from
  // `support::isTypeNameWord`, the same table this asks -- so the order of these
  // two questions cannot matter, which is the property that makes adding a name
  // here safe rather than a tie to be arbitrated. A name in the table whose
  // expansion failed answers `brokenName`: understood, no type, caller silent.
  if (words.size() == 1) {
    if (const TypeName* const named = findTypeName(names, words.front()); named != nullptr) {
      if (named->binders != 0) {
        // A **generic** name written with no arguments. Its template is a type
        // with `Param`s in it, which is not a type any position may hold, so this
        // is refused here -- beside the name -- rather than left to fail later
        // against the first member with a sentence about `T` that the reader never
        // wrote (`generics.md`, decision 3).
        return fail("`" + std::string(named->spelling) + "` is generic: it takes " +
                    std::to_string(named->binders) + " type argument" +
                    (named->binders == 1 ? "" : "s") +
                    ", and a generic name is only a type once they are written (`" +
                    std::string(named->spelling) + "<...>`)");
      }
      return named->type.valid() ? ok(named->type) : brokenName();
    }
  }

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
        // `f80` is a **format** and not a width: it is x87's, and a target whose
        // machines have no x87 -- AArch64, RISC-V -- has no such type. Refused
        // here, at the spelling, and not in the lowering: what this stage accepts
        // is a promise that the program compiles (`mincc check --target
        // aarch64-unknown-linux-gnu` used to accept `let x: f80` and `mincc build`
        // refused it, which is the gap this sentence closes). `long double` is the
        // portable spelling and resolves to whatever extended format the target
        // states; on the targets that have x87 the two are the same type.
        //
        // No `unknownWord`, deliberately: `f80` is a word this reader knows, and
        // the suggestion path would answer a reader who wrote a type the target
        // has no ABI for with "did you mean `i8`?". The sentence above is the
        // whole fix, and the diagnostic is `MalformedType` -- a well-formed run the
        // target cannot be given.
        if (primitive->bits == 80 && !target.hasFloat80()) {
          return fail("`f80` is the x87 80-bit format, which `" + std::string(target.name()) +
                      "` has no ABI for; use `f64`, or `long double` for this target's "
                      "extended format");
        }
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
    } else if (findTypeName(names, word) != nullptr) {
      // A name cannot be combined with a specifier either, and it is the same
      // mistake as `unsigned i32` -- `unsigned` applies to a C base type, and a
      // name for a type is a *complete* type (`type_alias.md`).
      for (const std::string_view other : words) {
        if (other != word) {
          return fail(quoted(word) + " is a name for a type and cannot be combined with " +
                          quoted(other),
                      word);
        }
      }
      return fail(quoted(word) + " is a name for a type and cannot be combined");
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
