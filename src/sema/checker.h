// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The checker, internal to `src/sema`.
//
// One object per unit, one pass, everything about it in one place. It is split
// across `check_stmt.cc` (declarations, statements, types) and
// `check_expr.cc` (expressions) but it is *one* class: an expression cannot be
// typed without the enclosing function's return type, a statement cannot be
// checked without the same environment, and splitting the state would mean
// passing the same fifteen things down every call.
//
// The checker owns three pieces of derived state for the duration of a unit:
//
//   * `defTypes_` -- every declaration's type, filled by the signature pass
//     before any body is checked, which is what makes a call to a function
//     written *later* in the file work;
//   * `defConstValues_` -- what a `const` binding is worth, so folding a `const`
//     expression does not need the initializer again;
//   * `depth_` -- the AST-depth guard, so a pathological tree is a diagnostic
//     and not a stack overflow;
//   * `globals_` -- the file-scope bindings and the value each one has, which
//     `checkGlobals` decides once, in dependency order, before any body is
//     checked (`global.cc`).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/ast.h"
#include "builtins/builtin.h"
#include "parse/syntax_kind.h"
#include "resolve/def_index.h"
#include "resolve/map.h"
#include "sema/sema.h"
#include "sema/sema_error.h"
#include "sema/type_store.h"
#include "sema/typed_ast.h"
#include "sema/typespec.h"
#include "support/consteval/suffix.h"
#include "support/constraint/constraint.h"
#include "support/intern/interner.h"

// The operator tokens the checker names, spelled once. Internal to this module,
// so it is included the way a sibling source file is: by name.
#include "tokens.h"

namespace minc::sema {

class Checker {
public:
  Checker(const ast::LoweredFile& file, const resolve::DefMap& defs,
          const support::Interner& symbols, TypeStore& types, SemaOptions options);

  [[nodiscard]] SemaOutput run();

private:
  // --- tree access -----------------------------------------------------------

  [[nodiscard]] bool inError(ast::AstId id) const {
    return file_.inErrorRegion(id);
  }
  [[nodiscard]] ast::AstId childOf(ast::AstId id, ast::NodeKind kind) const {
    return file_.childOfKind(id, kind);
  }
  [[nodiscard]] std::vector<ast::AstId> childrenOf(ast::AstId id, ast::NodeKind kind) const {
    return file_.childrenOfKind(id, kind);
  }
  // The children that are not tokens, in source order: for most nodes this is
  // exactly the operands, which is what keeps the checker from re-implementing
  // the grammar.
  [[nodiscard]] std::vector<ast::AstId> operandsOf(ast::AstId id) const;
  // The single token child, for the nodes whose operator or literal is one:
  // `BinaryExpr`, `PrefixExpr`, `PostfixExpr`, `AssignExpr`, `LiteralExpr`.
  // `kInvalidAst` when there is none.
  [[nodiscard]] ast::AstId tokenOf(ast::AstId id) const;
  [[nodiscard]] ast::NodeKind kindOf(ast::AstId id) const {
    return file_.at(id).kind;
  }
  [[nodiscard]] std::string_view spelling(ast::AstId id) const {
    return file_.spellingOf(id);
  }
  [[nodiscard]] support::Span origin(ast::AstId id) const {
    return file_.at(id).origin;
  }

  // --- results --------------------------------------------------------------

  void setType(ast::AstId id, TypeId type);
  void setExpr(ast::AstId id, const ExprInfo& info);

  // --- diagnostics ----------------------------------------------------------
  //
  // One place that appends, so ordering (source order, because the walk is)
  // and the error budget are decisions of the checker and not of a call site.
  void error(ast::AstId at, SemaErrorCode code, std::string message);
  void errorAt(support::Span span, SemaErrorCode code, std::string message);
  void warning(ast::AstId at, SemaErrorCode code, std::string message);
  void attachNote(SemaErrorCode code, support::Span span, std::string note);

  // --- declarations and statements ------------------------------------------

  void runSignatures();
  void checkFunction(const FunctionInfo& info);
  void checkBody(ast::AstId block, TypeId returnType);
  void checkStatement(ast::AstId stmt, TypeId returnType);
  void checkBlock(ast::AstId block, TypeId returnType);

  // -- control flow -----------------------------------------------------------

  // The one place a condition is decided, for `if`, `while`, `for` and `?:`
  // alike: a condition is a `bool`, and an arithmetic value is not silently one.
  // `what` names the construct, so the sentence says which condition it is about.
  void checkCondition(ast::AstId condition, std::string_view what);
  void checkIf(ast::AstId stmt, TypeId returnType);
  void checkWhile(ast::AstId stmt, TypeId returnType);
  void checkFor(ast::AstId stmt, TypeId returnType);

  // Can control reach the end of this statement? Exact rather than conservative
  // wherever the language allows an exact answer: a `return`; a block whose last
  // reachable statement terminates; an `if` whose *both* arms terminate; a loop
  // whose condition is a constant `true` and whose body contains no `break` that
  // could leave it; and a region the parser already reported (where saying
  // "control falls off the end" would be a second sentence about one mistake).
  [[nodiscard]] bool terminates(ast::AstId stmt) const;
  // Does this expression never produce a value? True exactly when its type is
  // `!`, which is the whole flow rule for the bottom type: no table, no callee
  // lookup, no second meaning of a word beside the signature (`never.md`).
  [[nodiscard]] bool diverges(ast::AstId expr) const;
  // The `return` this subtree can execute, or an invalid id when it cannot
  // execute one. *Reachable* is the load-bearing word: a `return` after a
  // statement that never completes is one a caller never sees, and reporting it
  // would make `while true {} return;` an error for a promise it does not break.
  // The scan stops wherever `terminates` says control cannot continue, which is
  // what keeps the two answers in step. Returning the node rather than a bool is
  // what lets the diagnostic point at the statement.
  [[nodiscard]] ast::AstId reachableReturn(ast::AstId node) const;
  // The value of a `let`/`const`, or an invalid id when it has none.
  [[nodiscard]] ast::AstId initializerOf(ast::AstId stmt) const;
  // `let (a, b) = t;` (`tuples.md`, decision 6). The pattern's names become real
  // bindings, each typed by the member it takes, and the count of names against
  // the count of members is checked here -- the one place both numbers exist.
  void checkDestructuring(ast::AstId stmt, ast::AstId pattern, bool isConst);
  // Is this loop guaranteed to leave only through a `return`? True for a
  // constant-true condition with no `break` aimed at *this* loop -- a `break`
  // inside a nested loop belongs to that loop and does not count.
  [[nodiscard]] bool loopsForever(ast::AstId stmt) const;
  [[nodiscard]] bool hasBreakForThisLoop(ast::AstId node) const;
  // The folded value of a condition, when the checker folded one. `nullopt` for
  // a condition it could not resolve to a constant.
  [[nodiscard]] std::optional<bool> constantCondition(ast::AstId condition) const;
  // `break`/`continue` are the only statements whose *validity* depends on where
  // they were written, so the loop nesting is tracked as the walk descends.
  std::uint32_t loopDepth_ = 0;

  // --- definite assignment ---------------------------------------------------
  //
  // A binding with no initializer holds no value, and this is what proves it is
  // never read that way. The analysis is a *separate pass* over the body and not
  // a thread through the typing walk, for two reasons: an assignment is an
  // expression, so the state has to move left to right *inside* an expression,
  // and a name being stored into is not a name being read, which is a
  // distinction the typing walk does not make when it types the target.
  //
  // The rules are the ones every production language with this check uses (JLS
  // 16, the C# and Swift specs): the state is a set of assigned definitions, a
  // conditional merges by *intersection* because only one side runs, a loop is
  // analyzed with the state from before it because it can run zero times, and a
  // loop whose condition is constantly true is left only by `break`, so its exit
  // state is the intersection of the states at its own breaks. No fixpoint and
  // no CFG: the shape of the language is enough, which is what keeps the answer
  // exact in step with what the checker already knows about reachability.
  //
  // One slot per definition in the unit; whether a slot is meaningful is
  // decided by the definition's kind (`Variable` and nothing else can be
  // unassigned).
  using AssignmentSet = std::vector<bool>;
  using BreakStates = std::vector<AssignmentSet>;

  void checkDefiniteAssignment(ast::AstId body);
  void flowStatement(ast::AstId stmt);
  void flowExpression(ast::AstId expr);
  // The target of a plain assignment: a store, not a load. Reads inside it (a
  // pointer being dereferenced, later) still count, which is why it is not
  // simply skipped.
  void flowStoreTarget(ast::AstId place);
  void markAssigned(ast::AstId place);
  void reportIfUnassigned(resolve::DefId def, ast::AstId at);
  // The state after a loop leaves, from the state at its entry and the states at
  // the `break`s aimed at it.
  [[nodiscard]] AssignmentSet loopExit(const AssignmentSet& entry, const BreakStates& breaks,
                                       ast::AstId condition);
  AssignmentSet assigned_;
  // Which definitions have already been reported. One mistake, one
  // diagnostic: a binding nobody assigned and read five times is one thing to
  // fix, and the five reads are one sentence rather than five.
  AssignmentSet reportedUnassigned_;
  // One per nested loop, innermost last: a `break` is recorded in the list of
  // the loop it belongs to, which a stack decides without a second scan.
  std::vector<BreakStates> breakStack_;

  // --- access, recorded ------------------------------------------------------
  //
  // `memory.md`'s access rule, written down where the tree can answer it. The
  // lowering reads the record instead of re-deriving it, on the same principle
  // the conversion record follows: a rule recomputed a stage later is a second
  // copy of the rule, and the two copies are what disagree.

  // The obligation belonging to a place that reaches memory through a pointer or
  // through an array. `place` is the `*p`, the `p[i]` or the `a[i]` itself, so
  // the binding between the record and the node the lowering stands on is the
  // identity and not a search.
  //
  // `extent` and `extentKind` are the half of the obligation the checked build's
  // bounds guard reads: `Count` with the type's number for a subscript of a named
  // array (`arrays.md` decision 26), `Length` for a subscript of a slice, whose
  // number is a word in the descriptor, and `Unknown` for a `*p` or a subscript
  // through a pointer, which have no number to give (`checks.md`).
  void recordAccess(ast::AstId place, TypeId type, ProvenanceKind provenance,
                    std::uint64_t extent = 0, ExtentKind extentKind = ExtentKind::Unknown);
  // The provenance of an access that sits *inside a place* -- `a[i]`, `a[1..2]`,
  // `t.0` -- which is not `provenanceOf`'s question: `provenanceOf` asks what
  // allocation a *pointer value* came from, and a path naming an array or a
  // product is not a pointer value. This asks what allocation the *place* `a[i]`
  // is inside, which is the object `a` names -- unless `a` is a parameter, whose
  // storage came in from the caller (`arrays.md` decision 26).
  [[nodiscard]] ProvenanceKind placeProvenanceOf(ast::AstId base) const;

  // --- the two questions about a place ----------------------------------------
  //
  // *Which declaration does this place belong to* and *which object does this
  // store give a value* are different questions, and an array is where they stop
  // being the same question (`arrays.md` decisions 23, 24).

  // The declaration a **store** assigns. Sees through parentheses and stops at a
  // name: storing into an element of an array does not assign the array.
  [[nodiscard]] std::optional<resolve::DefId> defOfStoreTarget(ast::AstId expr) const;
  // The declaration a **place** belongs to, for the rules that are about
  // permission. `*p` is not a place of `p`'s and neither is `p[i]` -- but
  // `a[i]` *is* a place of `a`'s when `a` is an array, because the element lives
  // inside the object the binding names.
  //
  // Both are declared here rather than in one function with a flag because the
  // call sites differ by question and not by convenience: `checkModifiable` and
  // `checkAddressOf` want the wide answer (a `const` table's element is not
  // writable), `markAssigned` wants the narrow one (an element store assigns
  // nothing).
  [[nodiscard]] std::optional<resolve::DefId> defOfPlace(ast::AstId expr) const;

  // Where the pointer an expression produces comes from, as far as this stage can
  // **prove**. `Object` only for the address of an object this unit named, moved
  // by arithmetic since; `Foreign` for everything the compiler cannot name.
  //
  // Sound and incomplete on purpose: the incomplete half costs an assumption the
  // optimizer could have made, and the wrong half would cost a correct program
  // its meaning.
  [[nodiscard]] ProvenanceKind provenanceOf(ast::AstId expr) const;

  // --- generics ---------------------------------------------------------------
  //
  // A generic declaration is a **template**: its signature is read with its
  // binders in scope, so it holds `Param`s, and nothing below this stage ever sees
  // the template itself. A *use* is an **instantiation** -- the declaration plus
  // an argument list -- and three things have to be decided for one: the type
  // arguments, the signature after substitution, and which instance a call
  // reaches (`generics.md`).

  // The table a type position is read with: the unit's names and then the
  // binders in scope, in that order -- binders last, because the table is a stack
  // and a binder is the name that was declared closest (`type_alias.md`, decision
  // 5). Rebuilt when either half changes, which is why nothing reads `aliasNames_`
  // or `binderRows_` directly for a read: two tables and one lookup is how the
  // two come to disagree about which name answered.
  [[nodiscard]] std::span<const TypeName> names() const {
    return names_;
  }
  void refreshNames() {
    names_.assign(aliasNames_.begin(), aliasNames_.end());
    names_.insert(names_.end(), binderRows_.begin(), binderRows_.end());
  }
  // A name for a type, published by the alias pass. One writer for the table and
  // the buffer beside it, so the two cannot drift.
  void addTypeName(const TypeName& row) {
    aliasNames_.push_back(row);
    refreshNames();
  }
  // The binders a signature or a body is read under. Entered and left by the two
  // functions that read them -- a generic declaration's signature and its body --
  // so "what is in scope" is a property of the walk's position and not of a flag a
  // caller has to remember to clear.
  void enterBinders(const std::vector<TypeName>& rows) {
    binderRows_ = rows;
    refreshNames();
  }
  void leaveBinders() {
    binderRows_.clear();
    refreshNames();
  }

  // The rows one binder list contributes to a type reader's table, appended in
  // binder order; the return value is how many were written.
  //
  // **Read once per declaration.** The signature pass and the body pass both enter
  // the binder list, and the second entry has to see what the first one saw -- a
  // second read of the tree could report a second sentence and could disagree. So
  // the declaration's binders live in `binders_` from the first read, and every
  // later call is answered from the table. That is also why a constraint's fault is
  // reported here exactly once.
  //
  // Total and silent about a binder's *name*, deliberately: that is `resolve`'s
  // question and it has already answered -- a reserved word and a repeated binder
  // are both its errors -- so a binder that is not a sound name contributes a row
  // whose type is invalid, which every consumer of the table already reads as
  // *understood, no type; the fault is reported elsewhere* (`readTypeSpec`'s
  // `brokenName`). A row is pushed for every binder written, so the count is the
  // written arity and not the number that happened to survive.
  std::uint32_t pushBinderRows(ast::AstId params, std::uint32_t owner, std::vector<TypeName>& out);

  // One table, one writer, and four readers that must not disagree -- a sentence
  // that names a binder, an operation that asks what its class grants, a
  // `DW_TAG_template_type_parameter` that needs the `Param` itself, and the type
  // reader's check that an argument is inside its binder's class. The row itself is
  // `sema::BinderRow` (`typespec.h`), because that last reader is handed it through
  // `TypeName::rows`; rebuilding a `Param` from `(owner, binder)` at any of them
  // would be a second way to name one binder, and the class is exactly the fact that
  // would be lost by it.
  //
  // The declaration's binders, or an empty span when it wrote none.
  [[nodiscard]] std::span<const BinderRow> binderRowsOf(std::uint32_t owner) const {
    const auto found = binders_.find(owner);
    return found == binders_.end() ? std::span<const BinderRow>()
                                   : std::span<const BinderRow>(found->second);
  }

  // The one read of a binder list: the names, the constraint each one wrote, and the
  // `Param`s those intern to. Called by `pushBinderRows` exactly once per
  // declaration, which is what keeps a fault to one sentence.
  void readBinderRows(ast::AstId params, std::uint32_t owner, std::vector<BinderRow>& out);

  // The class a `Constraint` names, and the one sentence for a word that is not a
  // class. `Any` is the answer for a fault -- the declaration is then
  // unconstrained rather than promising something no stage enforces, and the
  // sentence has already said so.
  [[nodiscard]] support::ConstraintClass readConstraintClass(ast::AstId constraint);

  // The class a type's binder was declared with, or `Any` when the type is not a
  // binder. One question, asked where an operation is about to be allowed and where
  // a type argument is about to be accepted.
  [[nodiscard]] support::ConstraintClass classOfBinder(TypeId type) const {
    return types_.binderClass(type);
  }

  // Does this type belong to the class? The predicate per class is the *operation
  // rule's own* (`support/constraint`'s header names one per class), so a class
  // cannot accept a type that the operation it grants would refuse.
  [[nodiscard]] bool satisfies(support::ConstraintClass klass, TypeId type) const;

  // A call whose callee is a generic declaration: the written argument list, or
  // inference from the arguments and the expected type, then the substituted
  // signature and the instance the call reaches.
  [[nodiscard]] TypeId checkGenericCall(ast::AstId expr, ast::AstId callee, std::uint32_t target,
                                        TypeId expected, ExprInfo& info);

  // The `TypeArgList` written behind `::`, read by the same reader every other
  // type position uses -- with the binders of the enclosing body in scope, so
  // `id::<T>(x)` inside `id` is one rule and not a special case. False when an
  // argument was refused, which is reported where it was read.
  [[nodiscard]] bool readTypeArguments(ast::AstId list, std::vector<TypeId>& out);

  // First-order unification, one equation: `pattern` is a type of the callee's
  // declaration (so it may hold that declaration's `Param`s) and `actual` is what
  // the program supplied. A pattern holding none of `owner`'s parameters gives no
  // equation at all -- the ordinary assignability check judges that argument, and
  // a comparison here would refuse a literal that is perfectly assignable.
  [[nodiscard]] bool solve(std::uint32_t owner, TypeId pattern, TypeId actual,
                           std::vector<TypeId>& solution);

  // Does this type mention a `Param` of `owner`? The question the worklist asks
  // of an argument list to decide whether an instantiation is concrete.
  [[nodiscard]] bool mentionsParam(TypeId type, std::uint32_t owner) const;

  // The instance for (declaration, arguments): the one already found, or a new
  // one -- appended to `out_.typed` as well, because the table is published. A
  // repeated pair is not re-created, which is what makes a recursive generic
  // terminate.
  std::uint32_t internInstance(std::uint32_t function, std::span<const TypeId> args, ast::AstId at);

  // The worklist, run once after every body has been checked: it expands each
  // instance's abstract sites until nothing new appears, and it is the reason the
  // set of instances cannot be enumerated by walking the program once.
  void runInstantiations();

  // The two spellings of an instance: `identity<i32>` (what a diagnostic and a
  // `DW_AT_name` print) and `__M8_identityi32` (what the linker sees, § 7).
  [[nodiscard]] static std::string instanceName(std::string_view name, std::span<const TypeId> args,
                                                const TypeStore& types);
  [[nodiscard]] static std::string mangle(std::string_view name, std::span<const TypeId> args,
                                          const TypeStore& types);
  static void mangleInto(const TypeStore& types, TypeId type, std::string& out);

  // An **operation** on a binder that the binder's class does not grant -- which is
  // the whole of the body-side rule (`generics.md`, § 6).
  //
  // False when the operation is allowed, so a caller reads it as a guard rather
  // than a test: `if (refuseOperation(at, type, op, "`+`")) return kTypeError;`.
  // True when `type` is a binder the class does not admit and the sentence was
  // reported; the caller then answers with a poison instead of the operation's
  // result, so one mistake stays one diagnostic.
  //
  // The sentence names the class to write -- the *least powerful* one that grants
  // the operation (`constraintForOperation`), because that is the smallest promise
  // that makes the body legal. For the three `bool`-only operators and the `bool`
  // positions, no class grants it on purpose and the sentence names the type
  // instead (`support/constraint`'s header says why).
  [[nodiscard]] bool refuseOperation(ast::AstId at, TypeId type, support::Operation op,
                                     std::string_view opText);

  // The **literal rule**'s sentence (`generics.md`, decision 11), and the one place
  // it is written: a deferred literal whose kind the binder's class does not admit is
  // refused here, naming the class, the literal's kind, and the two classes that
  // would take it.
  //
  // The decision itself is `support::literalAdmittedBy`, asked by all three sites
  // that can meet this case; this is only what a reader repairs from, so a change to
  // the rule cannot leave one of the three saying something the others do not.
  void refuseLiteralInBinder(ast::AstId at, TypeId binder, TypeId literal);

  // The **code** a type reader's failure reports under. The reader knows what went
  // wrong with the type and not which bucket a reader of the diagnostic looks for,
  // so the mapping is here, once, instead of as the same expression at three call
  // sites: an unknown word is the spelling's business (and earns a suggestion), an
  // argument outside its binder's class is the declaration's promise meeting the
  // use, and everything else is the type position itself.
  [[nodiscard]] static SemaErrorCode codeOf(const TypeSpecResult& spec);

  // The `Param` of the `binder`-th binder of `owner`, already interned by the
  // declaration's own read. `kInvalidType` for an unsound binder -- one `resolve`
  // refused -- and for an index past the list.
  [[nodiscard]] TypeId binderParam(std::uint32_t owner, std::uint32_t binder) const;

  // What the `binder`-th binder of `owner` was called. Kept beside the rows
  // because a `Param`'s identity is `(owner, binder)` and an unsound binder has no
  // `Param` at all -- and a sentence that names a binder writes the word the reader
  // wrote, never an index.
  [[nodiscard]] std::string_view binderSpelling(std::uint32_t owner, std::uint32_t binder) const;

  // --- types -----------------------------------------------------------------

  // --- type names -------------------------------------------------------------

  // `type Name = T;`, in dependency order, before anything reads a type: a
  // signature may be written with a name declared below it (`type_alias.md`,
  // decision 5). The pass publishes the alias table, builds `aliasNames_` for the
  // reader, and reports a cycle with the path that closed it.
  void runAliases();
  // The *other* half of the rule, and the small one: a `type` among the statements
  // of a block, where the name is visible from the declaration to the end of the
  // block. Checked as the block is walked, in order, and published on the same
  // stack the file scope uses -- so an inner name hides an outer one of the same
  // spelling for exactly as long as its block is being checked.
  void checkBlockAlias(ast::AstId decl);
  // The file-scope `type` declarations of the unit, in source order, with the def
  // each was declared as -- so the pass can ask "is this word one of mine?"
  // without re-deriving it from the tree per word.
  void collectAliases();
  //
  // The parts of a `Type` node, in source order: its `*` tokens and its words.
  [[nodiscard]] std::vector<TypePart> typeParts(ast::AstId typeNode) const;
  // One run of a type position, starting at `i`, stopping at a `,`, a `)` or the
  // end. A product's members are runs, and this is where that recursion lives
  // (`tuples.md`, decision 15): the type layer of the language is a run of parts
  // with two constructors, and a product is a base whose members are runs.
  void typePartsInto(std::span<const ast::AstId> children, std::size_t& i, std::uint32_t depth,
                     std::vector<TypePart>& parts) const;
  [[nodiscard]] TypeId resolveTypeNode(ast::AstId typeNode);
  // The type a binding has when nothing constrained it: `i32` / `f64` for a
  // deferred literal, the type itself otherwise.
  [[nodiscard]] TypeId defaultValue(TypeId type);
  // A deferred literal takes the type its context gave it, with a range check.
  // Anything else comes back unchanged; the assignment conversion is separate.
  [[nodiscard]] TypeId adaptTo(TypeId type, TypeId expected, ast::AstId at, const ExprInfo& info);
  // The assignment conversion, reported with the code of the context that asked.
  //
  // `expectedAt` is the `Type` node the expectation was written on, when the
  // caller has it: it is what lets a message name the type the way the source did
  // (`typeAsWritten`). Optional, because some expectations come from a type that
  // was written nowhere in particular -- a builtin's signature, an element of a
  // literal -- and a message that guessed a position would be worse than one that
  // expands the type.
  void checkAssignable(TypeId from, TypeId to, ast::AstId at, SemaErrorCode code,
                       std::string_view what, ast::AstId expectedAt = ast::AstId{});
  // A type as the source spelled it: the name the position wrote, with the type
  // it stands for in parentheses, or just the type when the position names
  // nothing. The pair is the whole point -- identity is the expansion, and the
  // reader repairs the word they wrote (`type_alias.md`, decision 8).
  [[nodiscard]] std::string typeAsWritten(TypeId type, ast::AstId typeNode) const;
  [[nodiscard]] std::string suggestTypeName(std::string_view word) const;
  // The sentence for an integer and a float, which is the one pair of arithmetic
  // types that does not convert. Two callers ask for it -- `checkAssignable` and
  // an arithmetic operator -- and what the message exists to say is the *fix*:
  // the reader wrote one class of number and can write the other, which is the
  // whole answer while the language has no cast.
  // `from` is the only side the advice depends on: which direction the two
  // classes are being crossed decides the fix, and the caller's prefix has
  // already named both types.
  [[nodiscard]] std::string mixingAdvice(TypeId from) const;

  // --- expressions -----------------------------------------------------------

  // --- builtins --------------------------------------------------------------
  //
  // A builtin call is a call, and everything about it that a reader sees -- the
  // wrong-count sentence, the argument conversion, the caret on the argument --
  // is the machinery a user function's call uses. What differs is the *order*,
  // and the row takes the place of a callee's type: a family's parameter list is
  // decided by the arguments, so they are checked first and the parameter types
  // fall out of them (`src/sema/builtins.cc`).

  // The row this callee names, or null when the callee is not a builtin. Called
  // before the callee is typed: a builtin has no function type until its
  // arguments have been seen, and typing it first would report "not a function"
  // about a name that is one.
  [[nodiscard]] const builtins::BuiltinInfo* builtinCallee(ast::AstId node) const;
  // One row's `BuiltinType`, resolved against the store -- which is what makes
  // `usize` mean the *target's* width, and what keeps a row from holding an id
  // that is local to one compilation (`builtins/builtin.h`).
  [[nodiscard]] TypeId builtinType(builtins::BuiltinType type) const;
  // One argument of an `any-int` parameter: typed by itself, with the default a
  // literal takes when nothing decides it, and refused when it is not an integer.
  [[nodiscard]] TypeId checkIntegerArgument(ast::AstId node);
  [[nodiscard]] TypeId checkBuiltinCall(ast::AstId expr, ExprInfo& info,
                                        const builtins::BuiltinInfo& row);
  // The sentence for a wrong argument count, shared with a user function's call
  // so the two cannot drift into saying the same thing differently.
  [[nodiscard]] static std::string argumentCountText(std::size_t expected, std::size_t given,
                                                     bool variadic);

  // The one entry point for an expression. It computes the node's type, adapts a
  // deferred literal to the context, writes both the type and the facts into the
  // artifact, and returns the adapted type -- so *every* caller sees the type the
  // expression actually has in context, and no call site can forget to adapt.
  [[nodiscard]] TypeId checkExpr(ast::AstId expr, TypeId expected);

  // The per-kind workers. They return the type and fill `info`; they never adapt
  // and never write to the artifact, which is what keeps `checkExpr` the only
  // place a node's answer is stored.
  // The literal needs `expected`: a value too large for the 64-bit core is
  // accepted only when the context asks for a type that can hold it (`i128`,
  // `u128`), and refused with a range error everywhere else.
  [[nodiscard]] TypeId checkLiteral(ast::AstId expr, TypeId expected, ExprInfo& info);
  [[nodiscard]] TypeId checkPath(ast::AstId expr, ExprInfo& info);
  // `x as T` and `(T)x`, one worker for both spellings -- they are one node, so
  // there is nothing to tell apart. The matrix is `castResult`, the operand is
  // typed **by itself** (a cast is where the context stops deciding: that is
  // what crossing a class means), and the pair is recorded at the cast so the
  // lowering converts through the record like every other conversion.
  [[nodiscard]] TypeId checkCast(ast::AstId expr, ExprInfo& info);
  // The type a literal's *suffix* names, resolved against the target: `10u8` is
  // a `u8`, `10L` is the target's `long`, `1.5L` the target's `long double`.
  // Called by `checkLiteral` for both classes, so the two spellings of "this
  // literal is this type" cannot disagree.
  [[nodiscard]] TypeId typeOfSuffix(const support::LiteralSuffix& suffix);
  [[nodiscard]] TypeId checkPrefix(ast::AstId expr, ExprInfo& info);
  [[nodiscard]] TypeId checkPostfix(ast::AstId expr, ExprInfo& info);
  // `&e`: the address of a place. Refused for anything that has no address, and
  // it is *not* an access -- taking an address reads nothing, which is why the
  // definite-assignment pass walks its operand as a place and not as a value.
  [[nodiscard]] TypeId checkAddressOf(ast::AstId expr, ExprInfo& info);
  // `*p`: a place of the pointee's type. This is where an access is recorded,
  // because this is the node the lowering stands on when it loads or stores.
  [[nodiscard]] TypeId checkDeref(ast::AstId expr, ExprInfo& info);
  // `a[i]`, which the language defines as `*(a + i)`.
  [[nodiscard]] TypeId checkIndex(ast::AstId expr, ExprInfo& info);
  // `(a, b)`: a product value. The members are typed *by position*, against the
  // context when it is a product of the same arity (`tuples.md`, decision 8).
  [[nodiscard]] TypeId checkTupleExpr(ast::AstId expr, TypeId expected, ExprInfo& info);
  // `t.0`: a member of a product, chosen at compile time. The member of a place
  // is a place, and a position past the arity is a diagnostic rather than a
  // runtime read (`tuples.md`, decisions 3 and 9).
  [[nodiscard]] TypeId checkField(ast::AstId expr, ExprInfo& info);
  // `a[1..2]`, `a[1..]`, `a[..2]`, `a[..]`: the view. Three bases (array, slice,
  // pointer), two bounds, and the one rule about both of them -- a bound is an
  // integer index and the end is one past the last element (`slices.md`).
  [[nodiscard]] TypeId checkSlice(ast::AstId expr, ExprInfo& info);
  // `[...]` and `T{...}`. The list form has no type of its own until its context
  // gives it one; the typed form carries its type and checks its elements against
  // it, which is where the exact length and the fill rules live.
  //
  // `checkArrayLiteral` takes the context's type because that is where the list
  // form's type comes from; the typed form carries its own.
  [[nodiscard]] TypeId checkArrayLiteral(ast::AstId expr, TypeId expected, ExprInfo& info);
  [[nodiscard]] TypeId checkTypedInitializer(ast::AstId expr, ExprInfo& info);
  // Whether the `;` of a filled initializer is written, which is the one thing
  // that tells a fill from a list.
  [[nodiscard]] bool hasFillSeparator(ast::AstId expr) const;
  // The one sentence for an initializer with nothing in it.
  void errorEmptyInitializer(ast::AstId consumer);
  // The rules every list of elements obeys, shared by both literal forms. The
  // type is the *caller's* answer: for the list form it is the consumer's, and
  // for the typed form it is the one the literal wrote (with `_` resolved from
  // the element count before this runs).
  [[nodiscard]] TypeId checkElements(ast::AstId consumer, std::span<const ast::AstId> elements,
                                     std::uint8_t operandBase, bool isFill, TypeId arrayType,
                                     ExprInfo& info);
  [[nodiscard]] TypeId checkBinary(ast::AstId expr, ExprInfo& info);

  // A binary operator with a **binder** on one side: the class decides whether the
  // operation is allowed, and the result is the binder (arithmetic) or `bool` (a
  // comparison). Split out of `checkBinary` because every rule below that point
  // compares two concrete kinds, and a `Param` is not one (`generics.md`, § 6).
  [[nodiscard]] TypeId checkBinaryOnParameter(ast::AstId expr, Tag kind, ast::AstId lhs,
                                              ast::AstId rhs, TypeId left, TypeId right,
                                              ExprInfo& info);
  // `++p` / `--p` / `p++` / `p--` on a *pointer*: one element step, which is
  // `p + 1` / `p - 1` with the same scaling and the same void rule. `nullopt`
  // when the operand is not a pointer, so exactly one of the two rules owns the
  // operator and neither can silently claim it.
  [[nodiscard]] std::optional<TypeId> checkPointerStep(ast::AstId expr, TypeId type);
  // The pointer arms of `+`, `-` and the comparisons. `nullopt` when neither
  // operand is a pointer, which is how the arithmetic path below stays free of
  // pointer cases and the pointer path free of arithmetic ones: exactly one of
  // the two can claim an operator, and `-Wswitch`-shaped exhaustiveness is not
  // needed for a question that is a single predicate.
  [[nodiscard]] std::optional<TypeId> checkPointerBinary(ast::AstId expr, Tag kind, ast::AstId lhs,
                                                         ast::AstId rhs, TypeId left, TypeId right,
                                                         ExprInfo& info);
  [[nodiscard]] TypeId checkConditional(ast::AstId expr, TypeId expected, ExprInfo& info);
  [[nodiscard]] TypeId checkAssign(ast::AstId expr, ExprInfo& info);
  // `expected` is the type the *value* is wanted as, and it is a parameter rather
  // than something `adaptTo` fixes afterwards because a generic call is one of the
  // two places where a context can decide a type: `let x: i32 = zero();` solves
  // `T` from the annotation, and `adaptTo` runs too late to see it.
  [[nodiscard]] TypeId checkCall(ast::AstId expr, TypeId expected, ExprInfo& info);

  // --- conversions, recorded --------------------------------------------------
  //
  // Every consumer of a value goes through `checkOperand`, which checks the
  // operand *and* records the conversion the consumer will apply. It is the
  // choke point on purpose: a consumer that passed an expected type to
  // `checkExpr` and did not record would produce a tree the lowering converts the
  // wrong way, or not at all, and there would be nothing to notice it by. For an
  // operator whose operands convert to a type only known once they are all
  // checked (`a + b` needs the *common* type first), the operator calls
  // `recordOperationOperand` itself.
  [[nodiscard]] TypeId checkOperand(ast::AstId consumer, std::uint8_t operand, ast::AstId child,
                                    TypeId expected);
  // An argument past the last declared parameter of a variadic function. There
  // is no parameter to check it against, so the argument is typed by itself and
  // what is recorded is the ABI's **default argument promotion** -- `f32` to
  // `f64`, anything of rank below `int` to `i32` -- because that is what the
  // callee will read. Getting this wrong is silent: `printf("%d", x)` with an
  // `i8` prints the wrong integer, and nothing else notices.
  [[nodiscard]] TypeId checkVariadicArgument(ast::AstId consumer, std::uint8_t operand,
                                             ast::AstId child);
  // What the ABI passes for a value of this type as an un-specified argument.
  [[nodiscard]] TypeId promotedArgument(TypeId type) const;
  // The operation type of `a op b` is derived from both operands, so the
  // operator computes it and then records each operand's conversion to it. One
  // function rather than two `recordConversion` calls, because the `decideAt`
  // that turns a deferred operand into a typed one is load-bearing: skipping it
  // records an untyped `from` or no conversion at all.
  void recordOperationOperand(ast::AstId consumer, std::uint8_t operand, ast::AstId node,
                              TypeId opType);
  void recordConversion(ast::AstId consumer, std::uint8_t operand, ast::AstId node, TypeId from,
                        TypeId to);
  // The same record, for a conversion the **source wrote**: the pair is legal by
  // the cast matrix and not by `convertible`, so this is the one path that
  // records a pair the implicit rules refuse (`i32` → `f64`, `*u8` → `usize`).
  void recordCast(ast::AstId consumer, std::uint8_t operand, ast::AstId node, TypeId from,
                  TypeId to);
  // The expression a cast converts: the one interior child that is not the
  // `Type` node. The rule is `bindingInitializer`'s, one node kind over, and it
  // is written here rather than counted because a cast's children differ between
  // its two spellings -- `(`, type, `)`, operand against operand, `as`, type.
  [[nodiscard]] ast::AstId castOperand(ast::AstId expr) const;
  // Give a deferred literal the type something decided for it. A deferred type
  // has no width, so it has no LLVM mapping, and no consumer can convert a value
  // of one: deciding it here is what keeps `1` in `let x: i64 = 1;` from reaching
  // the IR as `<integer literal>`.
  //
  // `decided` is a *hint*: an `i8` context cannot decide a `1.0` literal, so an
  // incompatible hint falls back to the language's default (`i32`/`f64`) and the
  // consumer records the conversion instead. That fallback is what makes the
  // answer total, whatever a future consumer passes.
  [[nodiscard]] TypeId decideAt(ast::AstId node, TypeId decided);
  // Decides whatever is still deferred, walking down from the unit's root. Every
  // node with a concrete type is what its operands take after -- a `BinaryExpr`
  // publishes its operation type, a `let` publishes its binding's type -- so
  // `1 + 2.0` in a `f64` binding becomes two `f64`s and not `i32`+`f64`. Whatever
  // is left when no parent had an answer is decided at the language's default.
  // Runs once, after the walk, so the artifact never holds a deferred type: it is
  // the property the IR depends on, and a backstop is what makes it true of paths
  // nobody has written yet.
  void decideDeferredTypes();
  // `decidedValid` is "the type being pushed down was accepted by the node it
  // came from": a node whose own type is the poison (an expression already
  // refused) pushes *no* type, so a literal inside it is defaulted rather than
  // reported a second time. `negated` is the parity of unary minus between the
  // node and the context, because `-128` in an `i8` is legal while `128` is not
  // and the two are the same literal.
  void decideSubtree(ast::AstId node, TypeId decided, bool decidedValid, bool negated);
  // Does a deferred node's own value fit the type it was decided at? Reports and
  // answers false when it does not, which is the README's rule -- "a literal
  // that does not fit the type its context gives it is an error, not a silent
  // truncation" -- applied to the leaves the context reached indirectly. Without
  // it `let y: u8 = 300 / 3;` would divide the *truncated* `44` and store `14`
  // while the folded value says `100`.
  [[nodiscard]] bool fitsDecided(ast::AstId node, TypeId type, bool negated);
  // The count of a shift has a range, and it is the width of the value moved and
  // not the count's own range: C leaves a negative or out-of-range count
  // undefined and the backend inherits a poison value. Asked by both places a
  // shift can be written, so the two cannot disagree.
  [[nodiscard]] bool checkShiftCount(ast::AstId countExpr, TypeId opType);
  // The divisor of a `/` or `%` has a rule of its own: a constant zero is a
  // mistake the compiler can see, whatever the *other* side is. Asked by both
  // places a division can be written, so `x /= 0` and `x / 0` cannot disagree --
  // and not folded into `foldBinary`, which fires only when *both* operands are
  // constant and so would miss `x / 0` entirely.
  [[nodiscard]] bool checkDivisor(ast::AstId divisorExpr, Tag op);

  // Folding for the operators that have a folded value. False when the result is
  // not a value at all -- today a division or remainder whose divisor is the
  // constant zero, which `checkDivisor` has already reported. There is no token
  // parameter: the diagnostic for that case is not this function's to make, so
  // it has no place to point at and no business knowing where one would be.
  [[nodiscard]] bool foldBinary(Tag op, const ExprInfo& left, const ExprInfo& right,
                                ExprInfo& info);

  // True when `operand` may be stored to. Reports the specific reason when it
  // may not: a `const` binding, or something that is not a place at all. The
  // two are separate diagnostics because their fixes are different.
  [[nodiscard]] bool checkModifiable(ast::AstId operand, TypeId type, ast::AstId at,
                                     SemaErrorCode code, std::string_view what);

  // --- the file scope ---------------------------------------------------------
  //
  // A file-scope binding is a **value the compiler writes**, so its initializer
  // is an expression this stage has to *evaluate* rather than a statement the
  // lowering can emit (`globals.md`, decision 2). Three properties fall out of
  // that one, and all three are why this is a pass of its own instead of a
  // second loop in `runSignatures`:
  //
  //   * the bindings are checked in **dependency** order, because a file-scope
  //     name is visible independently of order (`resolve.md`, decision A): in
  //     `const a = b * 2; const b = 3;` the value of `b` has to exist before `a`
  //     folds, or `a` would be refused for reading a name that is defined one
  //     line below it;
  //   * a **cycle** is the one shape with no such order, so it is refused with
  //     the chain that closes it rather than silently evaluated in whatever
  //     order the walk happened to take;
  //   * every value is **published** (`GlobalInfo`), because the bytes of a
  //     file-scope object are written by the compiler and not by a statement --
  //     a stage below cannot re-derive them without a second copy of these rules.
  //
  // The pass runs after the signatures and before any body, for the same reason
  // `runSignatures` runs first: the file scope is decided once, and everything
  // that reads it reads the same answer.

  // What an initializer came to, or where it stopped being one.
  struct IceValue {
    // False when the expression is not an initializer constant expression: the
    // rest of the record is then the zero value, `offender` is the node to point
    // at, and `reason` is the sentence.
    bool ok = true;
    GlobalValueKind kind = GlobalValueKind::Zero;
    // The value, when `kind` is `Int`.
    support::ConstInt value;
    // The literal whose spelling *is* the value, when `kind` is `Literal`.
    ast::AstId node;
    // True when that literal's value is negated: `-2.5`.
    bool negated = false;
    // The elements, when `kind` is `Aggregate`: one record per element, or
    // exactly one when `splat` is true. The *shape*, never the bytes -- a
    // `[1 << 20]u8{0; 1 << 20}` is one record and one constant.
    std::vector<GlobalElementValue> elements;
    // The initializer was a fill: the recorded element is written `count` times
    // and the count is the type's, so this is a flag and never an expansion.
    bool splat = false;
    ast::AstId offender;
    SemaErrorCode code = SemaErrorCode::GlobalNotConstant;
    std::string reason;
  };

  // One `type Name = T;`, while the pass is deciding it: what was declared, and
  // what its name stands for. The name is a *def* here -- the resolver's, with its
  // scope, origin and shadowing rules -- and never an entry in the type store
  // (`type_alias.md`, decision 2). One row per declaration of the unit, file scope
  // and block scopes alike.
  struct AliasBinding {
    ast::AstId decl;
    ast::AstId nameNode;
    // The `Type` node the name stands for, and the only place the expansion can
    // come from: a name has no other definition to read.
    ast::AstId target;
    resolve::DefId def;
    // For a generic name this is the **template** -- the target with `Param`s in
    // it -- which no stage below this reader ever sees: a use substitutes into it
    // and the substitution is a type the store already has (`generics.md`,
    // decision 8).
    TypeId type = kInvalidType;
    // How many binders the declaration wrote, and the node those binders belong
    // to -- the `owner` half of every `Param`'s identity. Zero binders is a name
    // for one type, and the pass treats the two the same way until a *use* says
    // which one it is.
    std::uint32_t binders = 0;
    std::uint32_t owner = 0;
  };

  // What a declaration's name stands for: reads the target with the names in scope
  // and reports what the reader said about it. Shared by the two scopes, because
  // what a name *means* does not depend on where it was declared. `false` when the
  // name is one the language keeps, which must not be published.
  bool decideAlias(AliasBinding& binding);
  // One entry in the published table (`TypedFile::aliases()`), which is what a
  // dump, an editor, and the debug info read. The index it lands at is the index
  // the row published for that name carries, so the two cannot disagree.
  void publishAlias(const AliasBinding& binding);

  // One file-scope binding, as the pass walks it. `value` is filled in by
  // `checkGlobal`, which is why a read of a binding can be answered by a lookup
  // and not by a second evaluation of its initializer.
  struct GlobalBinding {
    ast::AstId decl;
    ast::AstId init;
    resolve::DefId def;
    TypeId type = kInvalidType;
    IceValue value;
    // False until `checkGlobal` has decided this binding's value. A read of one
    // that is not decided yet is a cycle -- the walk is dependency-ordered, so
    // there is no other way to want a value that does not exist -- and answering
    // `Zero` for it is what keeps the pass total.
    bool decided = false;
  };

  // A read of a file-scope binding inside an initializer: *which* binding, and
  // the node that read it, so a cycle can be reported at the edge that closed it.
  struct GlobalRead {
    std::size_t index;
    ast::AstId at;
  };

  void checkGlobals();
  // The file-scope bindings of this unit, in source order, with the def each was
  // declared as. Source order and not walk order: it is what makes the published
  // table deterministic, and the walk below is in dependency order.
  void collectGlobals();
  // Checks one binding and records its value. One call per binding, in
  // dependency order, which is what lets a forward reference fold.
  void checkGlobal(GlobalBinding& binding);
  // The bindings the initializer reads, deduplicated and in a fixed order. One
  // edge per binding and not one per name: two reads of one name would make the
  // count of unfinished dependencies never reach zero, and an acyclic program
  // would be reported as a cycle.
  [[nodiscard]] std::vector<GlobalRead> readsOf(ast::AstId init) const;
  // The value of an initializer expression. `negated` is the parity of the unary
  // minus between the expression and the initializer as a whole, which is how
  // `-1.0` is still a literal.
  [[nodiscard]] IceValue evalInitializer(ast::AstId expr, bool negated = false) const;
  // An initializer that is an array literal, either form: one record per
  // element, or one record and `splat` for a fill. Recursive, because an element
  // is an initializer of its own type -- `[2][3]i32{[1, 2, 3], [4, 5, 6]}`.
  //
  // It is reached *before* the `isConstant` test in `evalInitializer`, and that
  // fact is deliberately false for an array: the checker refuses to claim a
  // folded value it has no record for (`arrays.md` decision 15). What makes an
  // array constant is therefore this walk and not a second notion of constness.
  [[nodiscard]] IceValue evalAggregate(ast::AstId expr) const;
  // The same answer for an expression that is not one, with the sentence that
  // names *why*: a `let`, a function, a call, a dereference and a local all
  // deserve different words, and "this is not a constant" tells the reader
  // nothing about which of them they wrote.
  [[nodiscard]] IceValue notConstant(ast::AstId expr) const;
  // The value already recorded for a file-scope binding, or nullptr when the def
  // is not one or its value is not known. A value that is not known is the cycle
  // case, which the walk has already reported with its chain.
  [[nodiscard]] const IceValue* globalValueOf(resolve::DefId def) const;
  // Copies a decided value into the published table the lowering reads.
  void publishGlobal(const GlobalBinding& binding);

  // --- declaration lookup ----------------------------------------------------

  void reportLimit(ast::AstId at);

  [[nodiscard]] std::optional<resolve::DefId> defAtName(ast::AstId nameNode) const;
  [[nodiscard]] const resolve::Def* defFor(resolve::DefId id) const;
  [[nodiscard]] TypeId typeOfDef(resolve::DefId id) const;
  [[nodiscard]] bool isConstDef(resolve::DefId id) const;
  // The declaration a `PathExpr` denotes, resolved by the stage below.
  [[nodiscard]] std::optional<resolve::DefId> defOfPath(ast::AstId pathExpr) const;
  // A name to put in a message about `expr`: its spelling when it is a path,
  // and a description otherwise, so no diagnostic says "this expression" about
  // something the reader can see is a name.
  [[nodiscard]] std::string nameOf(ast::AstId expr) const;

  // A folded value as the digits the reader would have written for it. Signedness
  // decides how the two's-complement bits are read; printing the raw bits of a
  // negative value would put `18446744073709551615` in a message about `-1`.
  // Here rather than in one file because two diagnostics print a value and the
  // two spellings must not drift.
  [[nodiscard]] static std::string valueText(support::ConstInt value);

  // How deep the checker will follow a tree before it stops and reports. The
  // lowered tree's depth is bounded by the parser, which asserts the same bound
  // at its entry; this is the second belt.
  [[nodiscard]] bool enterDepth();

  const ast::LoweredFile& file_;
  const resolve::DefMap& defs_;
  const support::Interner& symbols_;
  TypeStore& types_;
  SemaOptions options_;
  SemaOutput out_;

  // The type names in scope, in the order they were decided, for the type reader.
  // Data and not a lookup the reader performs, so `typespec` stays a table plus a
  // function with no knowledge of scopes, definitions or modules
  // (`type_alias.md`, decision 10). Empty for a unit that declares none, which is
  // what makes the reader's answer for `i32` cost nothing.
  //
  // **It is a stack.** The last row for a spelling is the name in scope, and a
  // block drops the rows it pushed when it ends -- which is the whole of the
  // shadowing rule, and the reason the reader searches backwards.
  // --- generics, while the walk runs ------------------------------------------

  // One **abstract site**: a call to a generic function inside a generic body,
  // whose arguments are written in terms of that body's binders. The worklist
  // expands one of these once per instance of the enclosing declaration, which is
  // what makes `id::<T>` inside `id` two instances instead of one
  // (`generics.md`, § 5).
  struct GenericSite {
    ast::AstId call;
    // The declaration the call is *inside*, which is also the binder list its
    // arguments are written in terms of.
    std::uint32_t body = 0;
    std::uint32_t target = 0; // index into `functionTable`
    std::vector<TypeId> args;
  };

  // The declared name of each generic *declaration*, by def index. A call reaches
  // a declaration through the def its callee resolved to, and the def is what
  // distinguishes two same-named declarations the way every other lookup in this
  // stage does.
  std::unordered_map<std::uint32_t, std::uint32_t> genericFunctionByDef_;
  std::vector<GenericSite> sites_;
  // The instances found so far, by the key that makes a repeated pair one pair:
  // the declaration's index and the arguments' ids, which is also what
  // `instances()` publishes in order.
  std::unordered_map<std::string, std::uint32_t> instanceByKey_;
  // Which instance a call reaches, keyed on (the body being lowered, the node).
  // The body is `kNoInstance` for every call written outside a generic body.
  std::vector<CallTarget> callTargets_;
  // A declaration's binders, by owner: the spelling, the `Param` and the class.
  // One entry per declaration that wrote binders, filled by the first read and
  // never rewritten (`pushBinderRows`).
  std::unordered_map<std::uint32_t, std::vector<BinderRow>> binders_;
  // The `(call node, instance)` pairs a constraint refusal has already been
  // reported for. A call inside a generic body is expanded once per instance of
  // the enclosing declaration, and two expansions that ask for the *same*
  // inadmissible argument would otherwise say the same thing twice.
  std::unordered_set<std::string> unsatisfiedReported_;
  // The declaration whose body is being checked, and how many binders it wrote.
  // `binders == 0` is every body that is not generic, and it is what makes the two
  // paths through a generic call one condition instead of a mode.
  std::uint32_t currentBody_ = 0;
  std::uint32_t currentGenericOwner_ = 0;
  std::uint32_t currentGenericBinders_ = 0;
  // The budget is reported once, like every other limit here.
  bool instanceLimitReported_ = false;
  std::vector<TypeName> aliasNames_;
  // The binders in scope: empty outside a generic declaration's signature and
  // body, and the declaration's rows inside it.
  std::vector<TypeName> binderRows_;
  // `aliasNames_` followed by `binderRows_`, kept up to date by `refreshNames`.
  std::vector<TypeName> names_;
  // Every `type` declaration this unit checked, in the order they were decided:
  // the file scope's first, in source order, then each block's as it is walked.
  // One entry per declaration is what makes `TypedFile::aliasAt` an index, and
  // the table is append-only -- indices are handed out by `publishAlias` and never
  // move, even when a block's names leave the scope.
  std::vector<AliasBinding> aliases_;
  // The **file-scope** declarations by the spelling they declared, which is the
  // question the dependency walk asks of every word of a type: "is this one of
  // mine, and is it decided yet?" (`sym` is not enough: the words of a type arrive
  // as text, and the reader compares text). A block needs no such map -- it is
  // checked top to bottom, so nothing below the line is in scope yet.
  std::unordered_map<std::string_view, std::size_t> aliasIndexBySpelling_;

  // The file scope: one entry per binding, in source order, filled by
  // `checkGlobals` and empty outside it. `globalIndexByDef_` is the def -> entry
  // map, so a name read in an initializer is a lookup and not a scan.
  std::vector<GlobalBinding> globals_;
  std::unordered_map<std::uint32_t, std::size_t> globalIndexByDef_;

  std::vector<TypeId> defTypes_;
  std::vector<support::ConstInt> defConstValues_;
  std::vector<bool> defHasConstValue_;
  std::vector<bool> defIsConst_;
  // Name uses and declarations, indexed by where they sit in the unit's text.
  // Built once, so a `PathExpr` costs a lookup and not a scan. The rule itself
  // is `resolve`'s (`def_index.h`): `ir` asks it too, and two copies of it would
  // be two answers waiting to differ.
  resolve::DefIndex index_;

  TypeId currentReturn_ = kTypeError;
  std::uint32_t currentFunctionName_ = support::kInvalidSym;
  std::size_t depth_ = 0;
  bool limitReported_ = false;
};

} // namespace minc::sema
