// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Generics: the binder rows, the instantiation, the inference and the two names
// an instance has.
//
// The record is `docs/architectures/generics.md`, and three of its decisions are
// what this file *is*:
//
//   * A generic declaration is a **template**. Its signature is read with its
//     binders in scope, so it holds `Param`s, and no stage below this one ever
//     sees the template itself. A use is an *instantiation* -- the declaration plus
//     an argument list -- and the substituted signature is a type the store
//     already had, so every consumer below reads an instance exactly as it reads a
//     hand-written function.
//   * The arguments are decided by **unification** from the arguments' types and
//     from the expected type: first-order, structural, with no generalization
//     (`generics.md`, § 2). A literal never decides a binder -- it contributes its
//     *default* type, which is the rule `let x = 5;` already follows.
//   * Discovery is a **worklist**, because the arguments inside a generic body are
//     written in terms of that body's own binders: `fn T id<T>(x: T) { return
//     id::<T>(x); }` has one call site and one instance per argument the *outer*
//     body is instantiated with (§ 5). The list is bounded and the bound is a
//     diagnostic rather than an out-of-memory.
//
// The **constraints** (§ 6) are the other half, and they are two rules over one
// table: a binder's class decides what its *body* may do with it (checked here,
// once, abstractly) and which *type arguments* may fill it (checked when an
// instance is made). A binder with no written constraint is `Any`, which grants no
// operation and admits every type -- so `<T>` means exactly what it meant before
// classes existed, and every refusal that used to say "constraints are the next
// stage" now says which word to write.
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "checker.h"
#include "sema/sema_error.h"
#include "support/typenames/type_name.h"

namespace minc::sema {
namespace {

[[nodiscard]] std::string quoted(std::string_view word) {
  return "`" + std::string(word) + "`";
}

// `1`/`2`/`n` and the word that follows it, for the arity sentences.
[[nodiscard]] std::string plural(std::size_t count, std::string_view one, std::string_view many) {
  return std::to_string(count) + " " + std::string(count == 1 ? one : many);
}

} // namespace

// --- the binder rows ----------------------------------------------------------

std::uint32_t Checker::pushBinderRows(ast::AstId params, std::uint32_t owner,
                                      std::vector<TypeName>& out) {
  if (!params.valid()) {
    return 0;
  }
  // The declaration's binders are read **once**, and every later entry -- the body
  // pass after the signature pass, which is the pair this exists for -- is answered
  // from the table rather than from the tree. Two consequences, and both are the
  // reason: a constraint fault is one sentence and not two, and the body pass cannot
  // disagree with the signature pass about what `T` is.
  std::vector<BinderRow>& rows = binders_[owner];
  if (rows.empty()) {
    readBinderRows(params, owner, rows);
  }
  out.reserve(out.size() + rows.size());
  for (const BinderRow& row : rows) {
    // `kNoAliasRow` because a binder declares no name for a type -- it *is* the
    // type. No `rows`: this entry is the binder's own name inside the declaration's
    // target, and the list a *use* checks against is the `fn`/`type` name's own row
    // (`TypeName::rows`), which is published with the declaration.
    TypeName entry{row.spelling, row.param, kNoAliasRow};
    out.push_back(entry);
  }
  return static_cast<std::uint32_t>(rows.size());
}

// The one read of a binder list: the names, their constraints, and the `Param`s
// they intern to.
void Checker::readBinderRows(ast::AstId params, std::uint32_t owner, std::vector<BinderRow>& out) {
  // The *n*-th `Name` and the `Constraint` that follows it are one binder
  // (`parse/syntax_kind.h`), so the list is walked in order and the constraint is
  // attached to the last name seen. A `Constraint` with no name before it cannot be
  // produced by the parser, and is ignored here rather than guessed at.
  std::vector<support::ConstraintClass> classes;
  std::size_t last = std::string_view::npos;
  for (const ast::AstId child : file_.childrenOf(params)) {
    const ast::NodeKind kind = kindOf(child);
    if (kind == ast::NodeKind::Name) {
      classes.push_back(support::ConstraintClass::Any);
      last = classes.size() - 1;
      continue;
    }
    if (kind == ast::NodeKind::Constraint && last != std::string_view::npos) {
      classes[last] = readConstraintClass(child);
      last = std::string_view::npos;
    }
  }

  // The binder's index is what the `Param` is made of (`owner`, index), so it is the
  // width of a `Param` from the first line rather than a `size_t` narrowed at the
  // call -- a declared binder list that could not be counted is not a case this
  // stage has to report.
  std::uint32_t count = 0;
  for (const ast::AstId binder : childrenOf(params, ast::NodeKind::Name)) {
    const std::string_view written = spelling(binder);
    const support::ConstraintClass klass =
        count < classes.size() ? classes[count] : support::ConstraintClass::Any;
    // A binder is a *name*, and a sound name is one `resolve` accepted. Three
    // things make it unsound, and all three are already reported there: a word of
    // the language (`fn i32 f<i32>()`, which would otherwise hide `i32` from the
    // reader), a repeated binding (`f<T, T>`, whose second def is not its own
    // canonical one), and a name the parser could not read at all.
    //
    // The question is asked of the *same* table the resolver asked
    // (`support::isTypeNameWord`) and of its own map for the rest, and this stage
    // stays silent about it: it contributes a row whose type is invalid, and every
    // consumer of the table already reads that as "understood, no type; the fault
    // was reported". One mistake, one diagnostic.
    bool sound = !written.empty() && !support::isTypeNameWord(written);
    if (sound) {
      if (const std::optional<resolve::DefId> def = defAtName(binder)) {
        const resolve::Def* const row = defFor(*def);
        sound = row != nullptr && row->canonical == *def;
      }
    }
    // The spelling is kept beside the row for the sentences that have to name a
    // binder, and it is *not* recoverable from the type afterwards: a `Param`'s
    // identity is `(owner, binder)` and an unsound binder has no `Param` at all.
    // The class travels the same way and for the same reason (`BinderRow`).
    BinderRow row;
    row.spelling = written;
    row.klass = klass;
    row.param = sound ? types_.param(owner, count, written, klass) : kInvalidType;
    out.push_back(row);
    ++count;
  }
}

// The class a `Constraint` names.
//
// A word that is not a class is refused here, and the refusal is *not* silent: an
// unrecognized constraint left as `Any` would be a declaration that asked for a
// guarantee and got none, which is the one outcome worse than an error
// (`generics.md`, § 6). The sentence lists the classes, because a reader who wrote
// a plausible word (`number`, `numeric`, `Ordered`) has no other way to learn the
// set.
//
support::ConstraintClass Checker::readConstraintClass(ast::AstId constraint) {
  const ast::AstId nameNode = childOf(constraint, ast::NodeKind::Name);
  if (!nameNode.valid()) {
    // The parser already reported the missing name; a `:` with nothing after it is
    // that sentence and not this one.
    return support::ConstraintClass::Any;
  }
  const std::string_view word = spelling(nameNode);
  if (word.empty()) {
    return support::ConstraintClass::Any;
  }
  if (const std::optional<support::ConstraintClass> klass =
          support::constraintClassFromName(word)) {
    return *klass;
  }

  std::string classes;
  for (const std::string_view name : support::constraintClassNames()) {
    if (!classes.empty()) {
      classes += name == support::constraintClassNames().back() ? " and " : ", ";
    }
    classes += name;
  }
  error(nameNode, SemaErrorCode::ConstraintNotAClass,
        quoted(word) +
            " is not a class a binder can be constrained to: a binder is "
            "constrained to one of the classes the language knows, because a class is what "
            "decides which operations the body may perform. The classes are " +
            classes);
  return support::ConstraintClass::Any;
}

// --- the questions ------------------------------------------------------------

bool Checker::mentionsParam(TypeId type, std::uint32_t owner) const {
  if (!types_.known(type)) {
    return false;
  }
  if (types_.isParamOf(type, owner)) {
    return true;
  }
  const Type& shape = types_.get(type);
  switch (shape.kind) {
  case TypeKind::Pointer:
  case TypeKind::Array:
  case TypeKind::Slice:
    return mentionsParam(shape.pointee, owner);
  case TypeKind::Tuple: {
    for (const TypeId member : types_.membersOf(type)) {
      if (mentionsParam(member, owner)) {
        return true;
      }
    }
    return false;
  }
  case TypeKind::Function: {
    if (mentionsParam(shape.returnType, owner)) {
      return true;
    }
    for (const TypeId param : types_.paramsOf(type)) {
      if (mentionsParam(param, owner)) {
        return true;
      }
    }
    return false;
  }
  default:
    return false;
  }
}

bool Checker::refuseOperation(ast::AstId at, TypeId type, support::Operation op,
                              std::string_view opText) {
  if (!types_.isParam(type)) {
    // Not a binder: the ordinary operation rules judge it, and this has nothing to
    // say. Every caller reaches here with a type it has already checked for being an
    // error, so the only two cases are a binder and a concrete type.
    return false;
  }
  const support::ConstraintClass klass = types_.binderClass(type);
  if (support::constraintGrants(klass, op)) {
    return false;
  }

  const std::string_view binder = types_.get(type).paramSpelling;

  // A **pointer** binder is the one class whose refusal must not name another class,
  // and the reason is that the operation is not missing a class -- it is missing a
  // *type*. `*p`, `p[i]` and `p + i` are the pointee's, and an abstract pointer does
  // not name it, so "widen the class to `Number`" would be advice about a different
  // kind of value entirely (`support/constraint`'s header).
  if (klass == support::ConstraintClass::Pointer) {
    error(at, SemaErrorCode::GenericOperation,
          std::string(opText) + " on a binder: `" + std::string(binder) +
              "` is a pointer, and the only operations expressible on one whose pointee nobody "
              "named are the comparisons. This one needs the pointee -- write the pointed-at type "
              "(`fn i32 f(p: *i32)`), or write the comparison");
    return true;
  }

  const support::ConstraintClass needed = support::constraintForOperation(op);
  if (needed == support::ConstraintClass::Any) {
    // No class grants it, which happens for exactly the positions that require a
    // `bool`: `!`, `&&`, `||`, and the condition of `if`/`while`/`for`. The fix is
    // not a class -- a class with one member is the type -- so the sentence names
    // the type.
    error(at, SemaErrorCode::GenericOperation,
          std::string(opText) + " on a binder: `" + std::string(binder) +
              "` is a type parameter and no class allows this, because it is defined for `bool` "
              "and nothing else. A value that must be a `bool` is written `bool`, not a binder");
    return true;
  }

  // Two sentences, and which one is right is decided by whether the binder has a
  // class at all: a binder with none is one word away from being fine, and a binder
  // with the wrong one needs the *larger* class named -- `Number` does not grant
  // `%` and `Integer` does, and `Ordered` does not grant `+` and `Number` does.
  // `constraintForOperation` is what picks the larger one, so the two sentences
  // cannot disagree about which class an operation needs.
  const std::string klassName(support::constraintClassName(klass));
  const std::string neededName(support::constraintClassName(needed));
  if (klass == support::ConstraintClass::Any) {
    error(at, SemaErrorCode::GenericOperation,
          std::string(opText) + " on a binder needs a constraint: `" + std::string(binder) +
              "` is a type parameter, and what may be done with one is what its class allows. "
              "Constrain it, as in `fn " +
              std::string(binder) + " f<" + std::string(binder) + ": " + neededName +
              ">(...)`, and `" + neededName + "` is the weakest class that admits " +
              std::string(opText));
    return true;
  }
  error(at, SemaErrorCode::GenericOperation,
        std::string(opText) + " on a binder: `" + std::string(binder) + "` is constrained to `" +
            klassName + "`, which does not allow it. Widen the class to `" + neededName + "`");
  return true;
}

void Checker::refuseLiteralInBinder(ast::AstId at, TypeId binder, TypeId literal) {
  const support::ConstraintClass klass = classOfBinder(binder);
  const support::LiteralClass admitted = support::constraintLiteralClass(klass);
  const bool isFloat = types_.get(literal).kind == TypeKind::FloatLiteral;
  const std::string binderName(types_.get(binder).paramSpelling);

  // Four shapes of refusal and one sentence each, and the four exist because the fix
  // is different in each: a binder with no class needs a class named, a class whose
  // members are not all of one kind of number needs a *narrower* class, and a class
  // of the other kind needs the literal rewritten. The `Any` case is first because a
  // class that admits nothing admits no literal either -- `None` is its value too,
  // and only one of the two can be said about it.
  std::string why;
  if (klass == support::ConstraintClass::Any) {
    why = "`" + binderName +
          "` has no constraint, so nothing says what a literal means for it: a class whose "
          "members are all integers (`<" +
          binderName + ": Integer>`) takes `1`, and one whose members are all floats (`<" +
          binderName +
          ": Float>`) takes `1.0`. Write the literal in the type you mean, or constrain the "
          "binder";
  } else if (admitted == support::LiteralClass::None) {
    why = "`" + binderName + "` is constrained to `" +
          std::string(support::constraintClassName(klass)) +
          "`, and that class does not say which kind of number a literal is: `1` would mean "
          "`1i32` for one instantiation and `1.0` for another, and the body is checked "
          "**once**. Write the value in the type you mean, or narrow the binder to `Integer` "
          "(every member an integer) or `Float` (every member a float)";
  } else {
    // The admitted kind is the other one, so the literal is the thing to rewrite --
    // and the class it should be narrowed to is the one that admits what was written.
    const bool classWantsInteger = admitted == support::LiteralClass::Integer;
    why = "`" + binderName + "` is constrained to `" +
          std::string(support::constraintClassName(klass)) + "`, whose members are all " +
          (classWantsInteger ? "integers" : "floats") + ", so a " +
          (isFloat ? "float literal has" : "integer literal has") + " no type to take. Write " +
          (classWantsInteger ? "an integer" : "a float") + " literal, or narrow the binder to `" +
          std::string(support::constraintClassName(classWantsInteger
                                                       ? support::ConstraintClass::Float
                                                       : support::ConstraintClass::Integer)) +
          "`";
  }
  error(at, SemaErrorCode::GenericOperation,
        std::string(isFloat ? "this float" : "this integer") + " literal cannot be stored in `" +
            binderName + "`: " + why);
}

TypeId Checker::binderParam(std::uint32_t owner, std::uint32_t binder) const {
  const std::span<const BinderRow> rows = binderRowsOf(owner);
  return binder < rows.size() ? rows[binder].param : kInvalidType;
}

// --- the two names of an instance ---------------------------------------------

void Checker::mangleInto(const TypeStore& types, TypeId type, std::string& out) {
  if (!types.known(type)) {
    out += 'X';
    return;
  }
  const Type& shape = types.get(type);
  switch (shape.kind) {
  case TypeKind::Int:
  case TypeKind::Float:
  case TypeKind::Bool:
  case TypeKind::Char:
  case TypeKind::Str:
    // A primitive, by the spelling the reader wrote and a diagnostic prints. The
    // names are prefix-free (`i8` is not a prefix of `i16`, `u8` is not one of
    // `u128`, `i32` is not one of anything), which is what lets a run of them read
    // back greedily and unambiguously without a separator (`generics.md`, § 7).
    // `isize`/`usize` need no case of their own: they *are* the pointer-sized int,
    // so `identity::<usize>` and `identity::<i64>` are one instance on a 64-bit
    // target -- correctly, because they are one type.
    out += types.spelling(type);
    return;
  case TypeKind::Pointer:
    out += 'P';
    mangleInto(types, shape.pointee, out);
    return;
  case TypeKind::Slice:
    out += 'S';
    mangleInto(types, shape.pointee, out);
    return;
  case TypeKind::Array:
    out += 'A';
    out += std::to_string(shape.count);
    out += '_';
    mangleInto(types, shape.pointee, out);
    return;
  case TypeKind::Tuple: {
    const std::span<const TypeId> members = types.membersOf(type);
    out += 'T';
    out += std::to_string(members.size());
    out += '_';
    for (const TypeId member : members) {
      mangleInto(types, member, out);
    }
    return;
  }
  case TypeKind::Function: {
    out += 'F';
    mangleInto(types, shape.returnType, out);
    const std::span<const TypeId> params = types.paramsOf(type);
    out += std::to_string(params.size());
    out += '_';
    for (const TypeId param : params) {
      mangleInto(types, param, out);
    }
    return;
  }
  default:
    // Not an object: a `Param`, `void`, `!`, the poison, a deferred literal. Every
    // one of them is refused before an instance exists, so this is a letter a
    // program cannot reach rather than a silent collision.
    out += 'X';
    return;
  }
}

std::string Checker::mangle(std::string_view name, std::span<const TypeId> args,
                            const TypeStore& types) {
  // `__M<length>_<name><arguments>`. The leader is reserved by the language -- a
  // declaration whose name begins with `__` is refused -- and that rule is what
  // makes the form unreachable from source without depending on a character the
  // source cannot produce. The length prefix delimits the name without a separator:
  // a digit cannot begin a primitive, so `<length>_<name>` and the argument run
  // after it cannot be confused for each other.
  std::string out = "__M";
  out += std::to_string(name.size());
  out += '_';
  out += name;
  for (const TypeId arg : args) {
    mangleInto(types, arg, out);
  }
  return out;
}

std::string Checker::instanceName(std::string_view name, std::span<const TypeId> args,
                                  const TypeStore& types) {
  // `identity<i32>`: the spelling a diagnostic and a `DW_AT_name` print, and the
  // one `gdb` matches when a reader breaks on the bare name (`generics.md`, § 8).
  // One formatter for both, so the two cannot drift.
  std::string out(name);
  out += '<';
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (i != 0) {
      out += ", ";
    }
    out += types.spelling(args[i]);
  }
  out += '>';
  return out;
}

// --- unification ---------------------------------------------------------------

bool Checker::solve(std::uint32_t owner, TypeId pattern, TypeId actual,
                    std::vector<TypeId>& solution) {
  // A pattern with none of this declaration's binders asks nothing: the ordinary
  // assignability check judges that argument, and comparing the two here would
  // refuse a literal that is perfectly assignable -- `identity(5)` in an `f64`
  // position is `i32` and then an error about *that*, which is the sentence the
  // reader needs.
  if (!types_.known(pattern) || !mentionsParam(pattern, owner)) {
    return true;
  }
  if (types_.isParamOf(pattern, owner)) {
    const std::uint32_t binder = types_.get(pattern).binder;
    if (binder >= solution.size()) {
      return true; // unreachable: the table is the declaration's binder list
    }
    if (solution[binder].valid()) {
      return solution[binder] == actual;
    }
    // The **occurs check**, and it is about an infinite type rather than a
    // mistake: binding `T` to a type that *contains* `T` has no finite... except
    // in this language it does, because an instance is a pair and not a type:
    // `fn T id<T>(x: T) { return id(x); }` solves `T := T` and terminates, since
    // the instance it asks for is the one being expanded.
    //
    // So the check is about the strict case only -- `T := *T`, which grows without
    // end -- and the identity equation is allowed. What the *budget* then bounds is
    // the family a body generates, not a cycle.
    //
    // Both comparisons are by identity, and identity is `(owner, binder)`, so a
    // binder of an *enclosing* declaration is a different type and never trips
    // either: that is exactly how a generic calls a generic.
    if (actual != pattern && mentionsParam(actual, owner)) {
      return false;
    }
    solution[binder] = actual;
    return true;
  }
  if (!types_.known(actual)) {
    return true;
  }
  const Type& shape = types_.get(pattern);
  const Type& given = types_.get(actual);
  switch (shape.kind) {
  case TypeKind::Pointer:
    return given.kind == TypeKind::Pointer && solve(owner, shape.pointee, given.pointee, solution);
  case TypeKind::Array:
    // The count is part of an array's identity, so a `[4]T` pattern meets a
    // `[4]u8` actual and nothing else: `[8]T` is a different type, and an argument
    // of one is not an argument of the other.
    return given.kind == TypeKind::Array && shape.count == given.count &&
           solve(owner, shape.pointee, given.pointee, solution);
  case TypeKind::Slice:
    return given.kind == TypeKind::Slice && solve(owner, shape.pointee, given.pointee, solution);
  case TypeKind::Tuple: {
    if (given.kind != TypeKind::Tuple) {
      return false;
    }
    const std::span<const TypeId> patternMembers = types_.membersOf(pattern);
    const std::span<const TypeId> givenMembers = types_.membersOf(actual);
    if (patternMembers.size() != givenMembers.size()) {
      return false;
    }
    for (std::size_t i = 0; i < patternMembers.size(); ++i) {
      if (!solve(owner, patternMembers[i], givenMembers[i], solution)) {
        return false;
      }
    }
    return true;
  }
  case TypeKind::Function: {
    if (given.kind != TypeKind::Function || shape.variadic != given.variadic) {
      return false;
    }
    if (!solve(owner, shape.returnType, given.returnType, solution)) {
      return false;
    }
    const std::span<const TypeId> patternParams = types_.paramsOf(pattern);
    const std::span<const TypeId> givenParams = types_.paramsOf(actual);
    if (patternParams.size() != givenParams.size()) {
      return false;
    }
    for (std::size_t i = 0; i < patternParams.size(); ++i) {
      if (!solve(owner, patternParams[i], givenParams[i], solution)) {
        return false;
      }
    }
    return true;
  }
  default:
    // A binder under a constructor the argument does not have: `*T` against an
    // `i32`. Not a solution, and not a crash.
    return false;
  }
}

// --- the instances -------------------------------------------------------------

std::uint32_t Checker::internInstance(std::uint32_t function, std::span<const TypeId> args,
                                      ast::AstId at) {
  // The key is the declaration's index and the arguments' ids -- the pair, which is
  // what makes a repeated instance one instance, and what makes a recursive generic
  // terminate: a repeat is never expanded twice.
  std::string key = std::to_string(function);
  for (const TypeId arg : args) {
    key += ',';
    key += std::to_string(arg.index);
  }
  if (const auto found = instanceByKey_.find(key); found != instanceByKey_.end()) {
    return found->second;
  }
  // The budget first. A declaration that grows its own arguments (`g::<*T>`) asks
  // for one instance per step, and a compiler that follows it exhausts memory
  // instead of reporting. Reported **once**, like every other limit here, because
  // the walk would otherwise produce one sentence per step it took.
  if (instanceByKey_.size() >= options_.maxInstances) {
    if (!instanceLimitReported_) {
      instanceLimitReported_ = true;
      error(at, SemaErrorCode::GenericInstanceLimit,
            "this unit needs more than " + std::to_string(options_.maxInstances) +
                " generic instances; a declaration that instantiates itself with a larger type "
                "every time has no last instance, so the count is where the compiler stops "
                "instead of running out of memory");
    }
    return kNoInstance;
  }
  const FunctionInfo& decl = out_.typed.functionTable[function];

  // The **constraint check**, and it belongs here rather than at the call: an
  // instance is made from `(declaration, arguments)` whichever path asked for it --
  // a concrete call, or the worklist expanding a site inside another generic -- so
  // one check here covers both, and the body it is about was already checked once
  // against the class. That is the trade the record makes (`generics.md`, § 6): the
  // *one* body check plus this is what lets the lowering substitute and emit with
  // no re-checking, which is the half C++ cannot have.
  //
  // Before the substitution, because a list that cannot be filled needs no signature.
  const std::span<const BinderRow> binders = binderRowsOf(decl.owner);
  bool admissible = true;
  for (std::size_t i = 0; i < args.size() && i < binders.size(); ++i) {
    if (types_.satisfies(binders[i].klass, args[i])) {
      continue;
    }
    admissible = false;
    // One sentence per way of being wrong here, not one per expansion. A call
    // written inside a generic body is expanded once per instance of the enclosing
    // declaration, and two expansions asking for the same inadmissible argument are
    // one fact about the source -- the node and the argument list, which is the key.
    std::string seen = std::to_string(at.index) + ':' + key;
    if (unsatisfiedReported_.insert(std::move(seen)).second) {
      error(at, SemaErrorCode::ConstraintUnsatisfied,
            "`" + types_.spelling(args[i]) + "` does not satisfy the constraint on `" +
                std::string(binderSpelling(decl.owner, static_cast<std::uint32_t>(i))) +
                "`: this declaration says that binder is `" +
                std::string(support::constraintClassName(binders[i].klass)) +
                "`, and a type argument has to be one of the types that class admits -- "
                "otherwise the body, which is checked once against the class, would mean "
                "something the instance cannot do");
    }
  }
  if (!admissible) {
    return kNoInstance;
  }

  // **The substitution**, and the whole of what an instance is: a signature the
  // store already knew how to build. A refusal here is the declaration's own rules
  // meeting the arguments -- `[4]T` with `T := void`, a count that does not fit the
  // address space -- and the sentence says that the argument list is what produced
  // it.
  const TypeId signature = types_.substitute(decl.functionType, args, decl.owner);
  if (!signature.valid()) {
    error(at, SemaErrorCode::GenericTypeArgs,
          "this type argument list has no instance: the arguments are substituted into the "
          "declaration's signature and the result is refused -- an argument that cannot be "
          "stored, or an object this target cannot address");
    return kNoInstance;
  }
  const std::string_view written =
      decl.name == support::kInvalidSym ? std::string_view("?") : symbols_.lookup(decl.name);
  InstantiationInfo info;
  info.function = function;
  info.args.assign(args.begin(), args.end());
  // The declaration's own `Param`s, so the two lists line up: this is what lets
  // the debug info name each template parameter with the word the reader wrote
  // (`binderSpelling`) and type it with the argument (`generics.md`, § 8).
  info.templateParams.reserve(decl.binders);
  for (std::uint32_t binder = 0; binder < decl.binders; ++binder) {
    // The `Param` the declaration's own read interned, and not a fresh one built
    // from the spelling: rebuilding it would be a second way to name one binder, and
    // the class the declaration wrote is exactly the fact a rebuild would drop
    // (`BinderRow`).
    info.templateParams.push_back(binderParam(decl.owner, binder));
  }
  info.functionType = signature;
  info.name = instanceName(written, args, types_);
  info.symbol = mangle(written, args, types_);
  const std::uint32_t index = static_cast<std::uint32_t>(out_.typed.instanceTable.size());
  out_.typed.addInstance(info);
  instanceByKey_.emplace(std::move(key), index);
  return index;
}

void Checker::runInstantiations() {
  // The seeds are the instances the walk already created -- exactly the calls
  // written outside any generic body -- and they are expanded in the order they were
  // found. The loop is over an index and not an iterator because the body appends to
  // the table it is walking, and it terminates because a pair is never expanded
  // twice (`internInstance`).
  for (std::size_t index = 0; index < out_.typed.instanceTable.size(); ++index) {
    const InstantiationInfo instance = *out_.typed.instance(static_cast<std::uint32_t>(index));
    const FunctionInfo& decl = out_.typed.functionTable[instance.function];
    for (const GenericSite& site : sites_) {
      if (site.body != instance.function) {
        continue;
      }
      // The site's arguments are written in terms of the *enclosing* declaration's
      // binders, and this instance is a substitution for exactly those: applying it
      // is what turns `id::<T>` into `id::<u8>` when the enclosing function was
      // instantiated at `u8`.
      std::vector<TypeId> args;
      args.reserve(site.args.size());
      bool concrete = true;
      for (const TypeId arg : site.args) {
        const TypeId substituted = types_.substitute(arg, instance.args, decl.owner);
        if (!substituted.valid()) {
          concrete = false;
          break;
        }
        args.push_back(substituted);
      }
      if (!concrete) {
        continue; // refused and reported where the substitution was asked for
      }
      const std::uint32_t target = internInstance(site.target, args, site.call);
      if (target == kNoInstance) {
        continue;
      }
      out_.typed.addCallTarget(CallTarget{static_cast<std::uint32_t>(index), site.call, target});
    }
  }
}

// --- the call ------------------------------------------------------------------

bool Checker::readTypeArguments(ast::AstId list, std::vector<TypeId>& out) {
  bool ok = true;
  // One `Type` node per argument, read by the same reader every other type position
  // uses -- with the binders in scope, which is what makes `id::<T>(x)` inside `id`
  // one rule and not a special case.
  for (const ast::AstId argument : childrenOf(list, ast::NodeKind::Type)) {
    const TypeId type = resolveTypeNode(argument);
    if (!type.valid()) {
      ok = false;
    }
    out.push_back(type);
  }
  return ok;
}

TypeId Checker::checkGenericCall(ast::AstId expr, ast::AstId callee, std::uint32_t target,
                                 TypeId expected, ExprInfo& info) {
  const FunctionInfo& decl = out_.typed.functionTable[target];
  const std::uint32_t owner = decl.owner;
  const std::string_view written =
      decl.name == support::kInvalidSym ? std::string_view("?") : symbols_.lookup(decl.name);
  const std::span<const TypeId> params = types_.paramsOf(decl.functionType);

  // The call's arguments, found **by kind** and not by position: a call that wrote
  // `::<...>` has three children -- the callee, the list of type arguments, and the
  // argument list of values -- and the value list is not the second.
  const ast::AstId argList = childOf(expr, ast::NodeKind::ArgList);
  const std::vector<ast::AstId> args =
      argList.valid() ? operandsOf(argList) : std::vector<ast::AstId>{};
  const ast::AstId writtenList = childOf(expr, ast::NodeKind::TypeArgList);

  // The count the *signature* decides, before anything is solved: a call with the
  // wrong number of arguments is that mistake and no other. A wrong count does not
  // stop the arguments being checked -- a wrong count must not hide a wrong
  // argument -- but it does stop the instance from being created.
  const bool variadic = types_.isVariadic(decl.functionType);
  const bool countOk = variadic ? args.size() >= params.size() : args.size() == params.size();
  if (!countOk) {
    error(expr, SemaErrorCode::ArgumentCount,
          argumentCountText(params.size(), args.size(), variadic));
  }

  // (1) The binder list, when the call wrote one. Everything after this either has a
  // full solution or has already said why it does not.
  std::vector<TypeId> solution(decl.binders, kInvalidType);
  bool solved = true;
  if (writtenList.valid()) {
    std::vector<TypeId> writtenArgs;
    solved = readTypeArguments(writtenList, writtenArgs);
    if (solved && writtenArgs.size() != decl.binders) {
      error(writtenList, SemaErrorCode::GenericTypeArgs,
            quoted(written) + " takes " + plural(decl.binders, "type argument", "type arguments") +
                ", and " + std::to_string(writtenArgs.size()) + " were written");
      solved = false;
    }
    if (solved) {
      solution = std::move(writtenArgs);
    }
  }

  // (2) The arguments, each checked **once**. What this step is *for* is the type of
  // each argument, because that is what a binder is solved from -- and the row it is
  // checked against is the template's, which means a binder.
  //
  // A binder as the *context* is deliberately withheld: a literal in a binder's
  // position contributes its own class's default and never the binder (decision 10),
  // so `identity(5)` is `identity<i32>` and not `identity<typeof 5>`. Handing the
  // binder down would let `decideAt` adopt it -- which is right where a value is
  // *stored* in one (`let x: T = 1;`) and wrong here, because here the literal is an
  // argument whose type is the equation, and a binder cannot be a term of it.
  // A **concrete** row is still handed down: `fn T f(n: u8, x: T)` has one, and the
  // width it gives a literal is the width that literal has.
  std::vector<TypeId> actuals(args.size(), kInvalidType);
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (i >= params.size()) {
      // Past the declared parameters: the variadic promotion and nothing else.
      (void)checkVariadicArgument(expr, static_cast<std::uint8_t>(i + 1), args[i]);
      continue;
    }
    if (types_.isParam(params[i])) {
      (void)checkExpr(args[i], kInvalidType);
      // No conversion is recorded here: the pair that matters is the argument and the
      // **instance's** parameter type, and that is not known until the instance is
      // (`coerce.cc`). Step (5) records it.
      actuals[i] = decideAt(args[i], kInvalidType);
      continue;
    }
    actuals[i] = checkOperand(expr, static_cast<std::uint8_t>(i + 1), args[i], params[i]);
  }

  // (3) The solution from what the arguments did not decide, and then from what the
  // context wants the *value* to be. Arguments first, because a binder the program
  // wrote a value for is decided by that value; the expected type only fills what is
  // left (`generics.md`, § 2).
  if (solved && !writtenList.valid()) {
    for (std::size_t i = 0; i < actuals.size() && i < params.size(); ++i) {
      const TypeId actual = defaultValue(actuals[i]);
      if (!solve(owner, params[i], actual, solution)) {
        error(expr, SemaErrorCode::GenericTypeArgs,
              "the arguments do not name one instance of " + quoted(written) + ": `" +
                  types_.spelling(params[i]) + "` cannot take `" + types_.spelling(actual) + "`");
        solved = false;
        break;
      }
    }
  }
  if (solved && !writtenList.valid() && expected.valid()) {
    (void)solve(owner, types_.get(decl.functionType).returnType, expected, solution);
  }

  // (4) What is still unknown, and what is not an object. Both are refusals of the
  // *instantiation*, and both name what to write.
  if (solved) {
    std::string unsolved;
    for (std::uint32_t binder = 0; binder < decl.binders; ++binder) {
      if (!solution[binder].valid()) {
        if (!unsolved.empty()) {
          unsolved += ", ";
        }
        unsolved += "`" + std::string(binderSpelling(owner, binder)) + "`";
        continue;
      }
      if (!types_.isObject(solution[binder])) {
        error(expr, SemaErrorCode::GenericTypeArgs,
              "`" + types_.spelling(solution[binder]) +
                  "` cannot be a type argument: an argument is substituted into the declaration "
                  "and the result is stored, so it has to be a type that can be stored");
        solved = false;
      }
    }
    if (solved && !unsolved.empty()) {
      error(expr, SemaErrorCode::GenericNotInferable,
            "nothing decides " + unsolved +
                ": the arguments and the context are both silent about the binder, so write the "
                "instantiation, as in `" +
                std::string(written) + "::<i32>(...)`");
      solved = false;
    }
  }
  if (!solved || !countOk) {
    return kTypeError;
  }

  // (5) The instance's signature, and the arguments against **its** rows. The
  // template's parameter type was the context an argument was checked in; the
  // instance's is what it is converted to, and the conversion is recorded here
  // because only now are both sides known (`coerce.cc`).
  const TypeId signature = types_.substitute(decl.functionType, solution, owner);
  if (!signature.valid()) {
    error(expr, SemaErrorCode::GenericTypeArgs,
          "this type argument list has no instance: the arguments are substituted into the "
          "declaration's signature and the result is refused");
    return kTypeError;
  }
  // The callee's own type, and it is the **instance's** signature: the call's
  // callee is the function this instantiation is, which is what makes
  // `lowerCall`'s "the callee is a value" path read the instance's parameters and
  // return type and not the template's. It is set here because this path never
  // checked the callee as an expression -- it could not: a template has no type to
  // give.
  if (callee.valid()) {
    setType(callee, signature);
  }
  const std::span<const TypeId> instanceParams = types_.paramsOf(signature);
  for (std::size_t i = 0; i < actuals.size() && i < instanceParams.size(); ++i) {
    checkAssignable(actuals[i], instanceParams[i], args[i], SemaErrorCode::InvalidAssignment,
                    " as this argument", args[i]);
    recordConversion(expr, static_cast<std::uint8_t>(i + 1), args[i], defaultValue(actuals[i]),
                     instanceParams[i]);
  }

  // (6) Two answers for the same call, and which one it is depends on where it was
  // written. Outside a generic body the instance is concrete and is created here.
  // Inside one, the arguments are written in terms of the enclosing binders, so the
  // *site* is recorded and the worklist expands it once per instance of the
  // enclosing declaration (§ 5). A call inside a generic body is recorded either
  // way, even when its arguments happen to be concrete: the body is lowered once per
  // instance, so the call has to answer per instance.
  if (currentGenericBinders_ != 0) {
    sites_.push_back(GenericSite{expr, currentBody_, target,
                                 std::vector<TypeId>(solution.begin(), solution.end())});
  } else {
    const std::uint32_t instance = internInstance(target, solution, expr);
    if (instance == kNoInstance) {
      return kTypeError;
    }
    out_.typed.addCallTarget(CallTarget{kNoInstance, expr, instance});
  }
  (void)callee;
  (void)info;
  return types_.get(signature).returnType;
}

std::string_view Checker::binderSpelling(std::uint32_t owner, std::uint32_t binder) const {
  const std::span<const BinderRow> rows = binderRowsOf(owner);
  if (binder >= rows.size() || rows[binder].spelling.empty()) {
    // The fallback is a binder the parser could not read a name for, and it is a
    // *spelling* rather than a fault: the sentence that would use it has already
    // been reported by the stage that found the missing name.
    return "T";
  }
  return rows[binder].spelling;
}

} // namespace minc::sema
