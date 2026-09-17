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
// What is deliberately not here is *constraints* (§ 6). A binder with no constraint
// is `Any`: it may be stored, copied, passed, returned and addressed, and every
// other operation is refused with the sentence that names the fix
// (`refuseParameter`). Constraints add classes to that one table; they change
// nothing below it.
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
  std::uint32_t count = 0;
  if (!params.valid()) {
    return 0;
  }
  for (const ast::AstId binder : childrenOf(params, ast::NodeKind::Name)) {
    const std::string_view written = spelling(binder);
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
    // The spelling is kept beside the row for the two sentences that have to name
    // a binder, and it is *not* recoverable from the type afterwards: a `Param`'s
    // identity is `(owner, binder)` and an unsound binder has no `Param` at all.
    if (!written.empty()) {
      std::vector<std::string_view>& names = binderSpellings_[owner];
      if (names.size() <= count) {
        names.resize(count + 1);
      }
      names[count] = written;
    }
    const TypeId parameter = sound ? types_.param(owner, count, written) : kInvalidType;
    // `binders == 0` on the row on purpose: a binder is not a generic name of its
    // own, so `T<i32>` is a use of a name that takes no arguments and the reader
    // says so. `kNoAliasRow` because a binder declares no name for a type -- it
    // *is* the type.
    out.push_back(TypeName{written, parameter, kNoAliasRow});
    ++count;
  }
  return count;
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

bool Checker::refuseParameter(ast::AstId at, TypeId type, std::string_view why) {
  if (!types_.isParam(type)) {
    return false;
  }
  error(at, SemaErrorCode::GenericOperation,
        std::string(why) + ": `" + std::string(types_.get(type).paramSpelling) +
            "` is a type parameter, and what may be done with one is what its constraint allows "
            "(`T: Num`). Constraints are the next stage; today a binder may be stored, copied, "
            "passed, returned and addressed");
  return true;
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
    info.templateParams.push_back(
        types_.param(decl.owner, binder, binderSpelling(decl.owner, binder)));
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

  // (2) The arguments, each checked **once**, against the template's row. That is
  // what lets a binder be solved from an argument's type, and it is also what keeps
  // a literal in its own class: a literal in a binder position is not decided by the
  // binder (decision 11), it contributes its default.
  std::vector<TypeId> actuals(args.size(), kInvalidType);
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (i >= params.size()) {
      // Past the declared parameters: the variadic promotion and nothing else.
      (void)checkVariadicArgument(expr, static_cast<std::uint8_t>(i + 1), args[i]);
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
  const auto found = binderSpellings_.find(owner);
  if (found == binderSpellings_.end() || binder >= found->second.size()) {
    return "T";
  }
  return found->second[binder];
}

} // namespace minc::sema
