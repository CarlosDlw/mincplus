# Generics — `<T>` on a function and on a `type`

`fn T identity<T>(value: T) { return value; }` is a declaration with a **hole in
it**. The hole is a type the declaration does not name, and the two halves of the
feature are: *which types may fill it* (an instantiation) and *what the
declaration is allowed to do with it while it is a hole* (a parameter, under a
constraint).

This is the last of the *composition* features in the checklist, and it is the
one that reaches furthest: it forces the type store to represent something
abstract, it forces a naming scheme for symbols, and it is the first feature that
makes the lowering run over the same tree more than once. Each of those three is
a decision here and not a discovery later.

Everything below that says *measured* was run on this machine; the console
transcripts are the evidence, not an illustration.

## What a parameter is, and what it is not

A **`Param`** is a type that stands for a type. It is not `void` (which has no
object), not `Error` (which is the poison), and not a deferred literal (which has
a class and no width). It is the only type in the store that cannot be lowered:
an instance replaces it before `src/ir` ever sees it, and that is an invariant
with a test rather than a promise (§ *Layers*, § *Decisions* 20).

It is a real, interned type, because the body of a generic is checked **once**
with the parameter as *the* type of `value`, `pair.0`, `xs[0]` and `return`. Every
question the checker already asks, it asks about a `Param`: is it an object, does
it convert, is `+` defined on it, what does `spelling` print. Two consequences,
and both are the point:

- The store needs exactly one new kind, and no existing predicate changes: a
  `Param` answers `isObject` true (a type argument is an object, § *Refusals*)
  and everything class-specific false (`isArithmetic`, `isInteger`, `isPointer`,
  `isAggregate`...). The checker's refusals then fire *at the declaration*, in
  the code that already exists, with the sentences that already exist — a `T`
  under `+` says what `*void` under `+` says, because it is the same line.
- The constraint is what *adds* answers. `fn T max<T: Ordered>(a: T, b: T)` is
  the declaration saying 'for this body, `T` answers `isArithmetic`' — see
  § *Constraints*.

## The surface

```
fn  <return type>  <name>  [<binders>]  ( params )  { ... }
type <name>        [<binders>]  = <type> ;
```

```minc
// one binder, one return type
fn T identity<T>(value: T) { return value; }

// the return type is a product, the binders are two
fn (T, K) makePair<T, K>(left: T, right: K) { return (left, right); }

// a binder reaches inside a constructor
fn i32 count<T>(xs: []T, needle: T) { return 0; }
fn T firstOf<T, K>(pair: (T, K)) { return pair.0; }

// the alias, same rule: the binder follows the name being declared
type Pair<T, K> = (T, K);
type Vec<T>    = []T;
type Grid<T>   = [4]T;

// a use: arguments are always written, because a type has no value to infer from
let p: Pair<i32, bool> = (1, true);
let m: Grid<Grid<f64>> = ...;              // and here is the `>>` of § 4

// a call: inferred by default, `::<...>` when nothing infers
let a = identity(5);
let z: i32 = zero::<i32>();
```

And inside a body, a binder is a type like any other name:

```minc
fn i32 f<T>(value: T) {
  type Local = (T, T);        // the binder is in scope for the whole definition
  let pair: Local = (value, value);
  return 0;
}
```

## 1. The position decides, so nothing is heuristic

This is the whole reason the syntax is what it is, and it is worth being precise
about, because C++ spent thirty years in the gap.

The parser **knows which position it is reading**, and the two positions have
opposite meanings for `<`:

| the parser is reading | `<` means | `Pair<i32, bool>` or `a < b` |
|---|---|---|
| a **type run** (`Parser::parseTypeRun`) | an argument list | a type |
| an **expression** (`Parser::parsePostfix`) | nothing — `a < b` is a comparison | nothing |

So in a type position there is no ambiguity at all: `Pair<i32, bool>` cannot be
anything but a generic type, because no expression may appear there. That is the
property `type_alias.md` already relied on when it put type names in their own
namespace (*'a name in a type position can only be a type name, and the parser
knows which position it is reading'*), applied to punctuation instead of words.

At a **call site** the position is an expression, and there `<` genuinely is
ambiguous: `f < T > (x)` is a valid comparison — `(f < T) > x` — so a bare
`f<T>(x)` would be a rule the parser could only apply by *guessing*, and it would
guess wrong silently in the one case where the reader wrote a comparison and
meant it. So a call site writes the argument list behind `::`:

```minc
let z: i32 = zero::<i32>();        // nothing to infer from: no argument, no expected type
let r = parse::<f64>(text);        // the text is `str` for every T
let big = max::<i64>(a, b);        // an explicit tie-break
```

`::` never begins a comparison, so `::<` is a type-argument list wherever it
appears. Two further properties, and neither is decoration:

- **The parser stays a parser.** No name resolution, no type knowledge, no
  backtracking: at the postfix level the check is `Colon Colon Less` — three
  tokens of *lookahead*, which is a decision, not a reparse. This is the same
  rule the C++ grammar cannot have and Rust had to invent a spelling for
  (*turbofish*, `Vec::<i32>::new()`), so the spelling is not ours to invent — it
  is the one the market converged on.
- **It is where module paths are going.** `modules.md` Q4 lists `a::b::c` as a
  candidate reference syntax, and `import m; m::name` as a row. `std::io::print::<i32>()`
  reads in one rule: after a path, `::` followed by `<` is an argument list and
  `::` followed by a name is a segment. This record decides nothing about
  modules, and it must not close that door — it opens one.

## 2. Inference decides types; it never converts

Inference is **first-order unification over parameters**, and it is deliberately
not Hindley–Milner:

- There are no polymorphic *values* (a generic name is not a value, § *Refusals*),
  so there is nothing to generalize and no let-generalization step exists.
- Every type constructor in the language is first-order: `*`, `[]`, `[N]`, `(...)`,
  and a function value's type. Unification over them is a single walk with no
  occurs-check subtleties in the common case (§ *Occurs*), no higher-order
  unification, and a *unique* most general solution when one exists — so a
  'cannot infer' is a fact about the program and not about the algorithm.

Sources of equations, in the order they exist:

1. **Each argument against its declared parameter type.** `[]T` against `[]u8`
   gives `T = u8`; `(T, K)` against `(i32, bool)` gives two equations.
2. **The expected type at the call site** — the annotation of the binding being
   initialized, the declared return type of the enclosing function, the parameter
   type of an enclosing call. `let p: Pair<f64, bool> = makePair(a, b);` decides
   both binders from the annotation.

Two rules keep the strictness of this language intact, and both are measured
consequences of decisions already taken:

- **A literal never decides a binder.** `identity(5)` cannot make `T` the
  deferred `<integer literal>`; the equation is recorded as a *candidate* and, if
  nothing else decides the binder, the default applies (`i32` for an integer
  literal, `f64` for a float one) — exactly the rule `let x = 5;` already
  follows. So `identity(5)` is `i32`, and `identity(5.0)` is `f64`.
- **Inference never widens, narrows, or converts.** Measured on the current
  compiler:

  ```console
  $ mincc check   # let f: f64 = 1;
  error[sema-invalid-assignment]: `i32` does not convert to `f64` in this initializer:
        an integer and a float are different classes of number and do not convert into
        each other -- write the value in the class you want, as in `1.0` for a float
  $ mincc check   # let f: f32 = 1.0;          (a float literal, a float type)
  ok
  ```

  So a substitution is types and only types: if a binder is solved to `f64` and
  an argument is an `i32` *value*, the argument is checked against `f64` and gets
  that same sentence. Inference is not a back door around the assignment rules.

When a binder cannot be solved, the diagnostic names **that binder** and the fix:

```
error[sema-cannot-infer]: `T` cannot be inferred: no argument mentions it and the
      expected type here says nothing about it; write the argument list, as in
      `zero::<i32>()`
```

### Occurs

A substitution is `{Param(owner, i) ↦ τ}` and `τ` may mention parameters of an
*enclosing* generic — that is ordinary and is what makes a generic callable from
a generic (§ *Instantiation is transitive*). What must not happen is a solution
that mentions the binder being solved: `T = []T` has no finite instance. It is
refused where the substitution is built, with its own sentence, because a store
holding an infinitely nested type is a hang somewhere later rather than an error
here.

## 3. The `>>` that closes two lists

Our lexer is maximal-munch (`src/lex/lexer.cc`), so `>>` is **one** token, and so
is `>>=`. Measured:

```console
$ grep GreaterGreater src/lex/lexer.cc
      return c2 == '=' ? punct(offset, 3, TokenKind::GreaterGreaterEqual)
                       : punct(offset, 2, TokenKind::GreaterGreater);
```

`Grid<Grid<f64>>` and `let p: Pair<i32, Pair<i32, bool>>= t;` are therefore not
splittable by the lexer, and moving the rule into the lexer would be worse — the
lexer has no modes by design (`lexer.md`: it decides trivia and tokens, never
grammar).

So the **list reader** closes on `Greater` and splits a compound token that closes
two lists. Four cases, **all four handled**, and the mechanism is what matters:
closing a list *returns what it consumed*, so a compound closer is observed where
it happens and paid for where it is owed. No parser state, no sub-token cursor,
and no space required in the source:

```cpp
// What closing a list of arguments (or of binders) consumed.
//
// The cases are `>`, `>=`, `>>` and `>>=`, and the last three carry something the
// *enclosing* frame owns: a second closure, an `=`, or both. Which frame owns
// what is why this is a value and not a flag: the type run returns it, the list
// returns it, `parseType` returns it, and the two positions that have an `=` of
// their own -- a binding and an alias -- use it instead of reading the next
// token. A `Parser` member would leave state behind; a value cannot.
struct ListClose {
  bool closedParent = false;   // `>>` or `>>=`: the enclosing list is closed too
  bool sawEqual = false;       // `>=` or `>>=`: the token also carried the `=`
};
```

Where a position cannot have an `=` (a parameter, a product member, a cast), a
returned `sawEqual` is **reported** rather than swallowed, so a stray `=` costs
one sentence instead of a misread. It is also exactly what the market does,
measured:

```console
$ rustc --edition 2021 t2.rs                     # Vec<Vec<i32>> and Vec<Vec<i32>>= and Option<Vec<Vec<i32>>>
(compiles, runs)
$ clang++ -std=c++20 -fsyntax-only t5.cpp        # std::vector<std::vector<int>> v;
(accepted -- C++11)
$ clang++ -std=c++20 -fsyntax-only t4.cpp        # std::vector<std::vector<int>>= {};
t4.cpp:2:41: error: a space is required between consecutive right angle brackets (use '> >')
```

Rust splits, and the reader never needs a space. C++ fixed `>>` in C++11 and
**still** refuses `>>=`. We take the full split, all four cases: the returned
value is what makes `>>=` cost one field instead of a parser state, and a rule the
reader has to remember at the one place where they are writing a nested type is
exactly the kind of rule this project does not ship.

There is a second, future-facing note here, and it is the reason to write the
rule down rather than patch it later: today an array count is a **literal**, so
nothing else in a type can contain a `>>`. The day a count accepts a name or an
expression (`arrays.md` already contemplates a count that folds), a compound
count inside an argument list needs delimiters — Rust's own reference documents
the same restriction for const arguments as *'necessary to avoid requiring
infinite lookahead when parsing an expression inside of a type'*, which is the
general form of the problem.

## 4. Layers: what each stage knows

The four stages answer four different questions, and the split is what keeps the
feature from spreading:

| stage | what it knows | what it does | what it must not do |
|---|---|---|---|
| **parse** | the syntax | `parseGenericParams` between `parseTypeAndName()` and `expect(LParen)`, and between the alias's `Name` and its `=`; an argument list inside `parseTypeRun`; `::<...>` in `parsePostfix` | resolve names, know types, backtrack |
| **resolve** | a binder is a name | a `Def` in the **`Tag`** namespace, scoped to the whole definition, with the same duplicate/`-Wshadow` rules as an alias; a built-in word (`i32`) cannot be a binder | know arity, know types |
| **sema** | types | one check of the body under the constraints; the inference of each instantiation; the published tables | lower, mangle, emit |
| **ir** | concrete types | one `llvm::Function` **per instance**, the body lowered once per instance, the substitution applied at the boundary | see a `Param` at all |

The type reader needs *one more row in a table it already takes*. `typespec.h`'s
`TypeName` is `{spelling, type, alias}` and `readType` is handed a span of them,
defaulted empty, so that a unit's vocabulary is **data** and the reader never
calls back into `resolve` — `type_alias.md` decision 10, written before generics
existed, for exactly this. A binder is a row with an abstract `type`; the
argument-list syntax is read by the reader into constructors, and a use of a
generic name in a type position becomes an **instantiation**, which for an alias
is a substitution and nothing else (§ 5).

## 5. Instantiation

An instantiation is `(declaration, argument list)`, and the pair determines
everything else. Three properties, in the order they matter:

- **For an alias, instantiation is substitution and nothing else.** `type Pair<T, K>
  = (T, K);` plus `Pair<i32, bool>` produces `(i32, bool)` — a type the store
  *already has*, and the check is a `TypeId` equality. This is `type_alias.md`
  decision 2 (an alias never enters the store) carried one step: a generic alias
  never enters the store either, and the store does not grow by an instantiation.
  It is a stated invariant with a test (`TypeStore::count()` unchanged), and it
  is why a generic alias is ABI-free in a way a generic `struct` will not be.
- **For a function, instantiation produces a signature the store already knows
  how to build.** `fn T identity<T>(value: T)` instantiates at `<i32>` to the
  function type `fn i32(i32)` — the same `TypeId` a hand-written `fn i32
  identity(value: i32)` would have. So after instantiation **the store contains
  no generics at all**, and the lowering's type mapper, the ABI code and the
  debug info need no new case; what identifies an instance is the *pair*, not the
  type.
- **Substitution is total, so there is no capture and no renaming.** A
  substitution is built for one declaration and covers **every** binder of it, so
  no `Param` of that declaration survives it. `*T` binds tighter than any notion
  of a binder, so a body referring to `T` gets the argument list the caller
  solved, and a body referring to an *enclosing* `T` keeps that `Param` — which
  is correct, and is how `fn i32 outer<T>(x: T) { return inner(x); }` works.

### Instantiation is transitive, and that is a worklist

A generic body contains calls to other generics, and their type arguments are
written in terms of the body's own binders:

```minc
fn T id<T>(x: T) { return id::<T>(x); }                  // the inner argument is the *binder*
fn (K, T) swap<T, K>(p: (T, K)) { return (p.1, p.0); }
fn (T, K) twice<T, K>(p: (T, K)) { return swap(swap(p)); }   // inferred as <K,T>, then <T,K>
```

So the set of instances **cannot be enumerated by walking the program once**: the
inner instantiation of `id<i32>` is `(id, [i32])`, and of `id<f64>` is
`(id, [f64])`, and both come from the same source text. Discovery is therefore a
worklist:

1. seed it with every **concrete** call — a call written outside any generic body;
2. take an instance, apply **its** substitution to the type arguments its body
   recorded, and add the resulting instances;
3. a pair already in the set is not re-expanded.

Step 3 is what makes a recursive generic terminate: `id<i32>`'s body asks for
`(id, [i32])`, which is itself. And the checker's body check happens **once**,
independently of this list, which is why the list is the *lowering's* input and
not a second type-checking pass.

**The list is bounded, and the bound is a diagnostic.** Step 2 can grow without
limit — `fn i32 g<T>(x: T) { return g::<*T>(x); }` asks for `*i32`, then `**i32`,
then... — and a compiler that follows it runs out of memory instead of reporting.
So there is a budget per compilation, in the shape the project already uses for
everything unbounded (`kMaxTypesPerUnit`, the preprocessor's token and byte
budgets), and overrunning it is a sentence that names the declaration and the
count. rustc reports the same situation as *'reached the recursion limit while
instantiating'*; the difference here is that the number is stated and settable
rather than a compiler constant nobody can see.

## 6. Constraints

A constraint says **which operations the body may perform on the hole**. It is a
compiler-known capability class, because there is nothing else in the language
for it to be yet — no `interface`, no `trait`, no user-declared type classes —
and the syntax has exactly one slot for one to occupy later:

```minc
fn T max<T: Ordered>(a: T, b: T) { return a < b ? b : a; }
fn T twice<T: Number>(x: T) { return x + x; }
fn T identity<T>(value: T) { return value; }        // no constraint: the default
```

### The two facts of a class, and why they are two

A class is **members** and **grants**, and the pair is stated separately on
purpose:

* its **members**, a predicate over types — the predicate the operation rules
already use (`isArithmetic`, `isInteger`, `isFloat`, `isScalar`, `isPointer`);
* its **grants**, the operations a body may perform on a hole of that class.

The temptation is to *derive* the grants from the members — a class grants
whatever its members admit. That derivation is wrong, and the pair `Ordered` /
`Number` is why: they admit exactly the same types and promise different
operations. `fn T max<T: Ordered>` says *"all I do with it is compare"*, which is
a smaller promise than `Number` and therefore a better one; a derivation would
collapse the two into one class and take away the reader's ability to say which
capability they meant. The same shape appears in Rust (`Add` ≠ `PartialOrd`, and
a bound `T: Add` does not give `<`) and in Haskell (`Num` and `Ord` are separate
classes over overlapping types).

What *is* derived — and what a test in `tests/unit/sema/constraint_test.cc`
asserts over representative types — is soundness:

> **Every operation a class grants is legal for every type in it.**

That is the property that keeps the grants from being decoration, and it is a
test rather than an argument because it is exactly what fails when the two facts
drift apart: a `Float` that granted `%` would grant an operation no float admits,
and a `Number` that granted `%` would be a lie for half its members. It is the
failure C++ has, where the constraint is inferred from whatever the body did and
surfaces at a call site two layers away.

The classes, each written as *members ← the operation rules*, and *grants*:

| class | members | grants |
|---|---|---|
| `Any` | every object | nothing beyond the universal rules (store, copy, pass, return, be an element) |
| `Eq` | `isScalar` — arithmetic, `bool`, `str`, pointer | `== !=` |
| `Ordered` | `isArithmetic` | `Eq` and `< <= > >=` |
| `Number` | `isArithmetic` | `Ordered` and `+ - * / \| ± ++ --` |
| `Integer` | `isInteger` — the integers and `char` | `Number` and `% & \| ^ ~ << >>` |
| `Float` | `isFloat` | `Number`, and **fewer members**: `f32`, `f64`, `f80` |
| `Pointer` | `isPointer` | the comparisons, and nothing else |

Three of these are worth a sentence rather than a row:

* **`Eq` is wider in members and narrower in grants than the arithmetic** — a
  `str`, a `bool` and a pointer are equatable and are not numbers, which is why
  equality is a class of its own and not part of `Number`.
* **`Float` is a narrowing, not a capability.** Its whole use is *"this works for
  floats and must be refused for an integer"* — `fn T mean<T: Float>(xs: []T)`.
  It grants everything `Number` does, over fewer types.
* **`Pointer` grants only the comparisons**, `p < q` being an address comparison
  this language defines. `*p`, `p[i]` and `p + i` are the *pointee's* type, and an
  abstract pointer does not name one — so no class grants them, and the refusal
  says to write the pointed-at type instead of widening the class to `Number`,
  which would be advice about a different kind of value.

**No class for `bool`.** `!`, `&&`, `||` and the condition of `if`/`while`/`for`
are `bool`-only, and no class grants them *on purpose*: a class whose members are
one type is not a constraint, it is the type. A binder in one of those positions
is told to write `bool`, the honest fix.

### A constraint on a generic `type`, where only one rule applies

An alias's target is a type expression, and a type expression performs no
operation — so of the two halves above, only the **satisfaction** half can say
anything about `type Vec<T: Number> = [4]T;`. It is not decoration either way: the
declaration wrote which types may fill the hole, and a use is where a hole gets
filled.

So a use is checked, in the type reader, at the substitution: `Vec<bool>` is
refused with the argument, the binder and the class named, and no need to read the
target first. This is C++'s rule for a constrained alias template and not Rust's,
which ignores bounds on an alias — and the reason to have it is the reason the
whole table exists: a declaration that promises something no stage checks is the
failure `parse-constraint-not-read` was there to prevent.

**Composition is not here yet, and nothing needs it.** Every pair of these
classes is either redundant or contradictory (`Number` already grants everything
`Ordered` does; `Float` and `Pointer` share no member), so `<T: A + B>` has no use
today. The syntax that will need it is the **second independent class**, which is
what a user-declared `interface` brings — a class that says nothing about
operations this table already covers.

Two rules, and they are the two halves of soundness:

1. **Only the constrained operations are legal.** A `T` under `+` with no
   `Number` is refused *at the declaration*, and the sentence names the **least
   powerful** class that grants the operation — a body that only compares is told
   `Ordered`, because naming `Number` would make the declaration promise
   arithmetic it never uses. This is Go's rule, stated in its design document:
   *'Generic functions may only use operations supported by all the types
   permitted by the constraint.'*

   The advice is **checked before it is given**: `%` on a `Float` binder is told
   that no class can be widened to, because `Float` and `Integer` share no member
   and following "widen to `Integer`" would make every call fail. A refusal whose
   repair is worse than the mistake is not a refusal, and `classesOverlap` — asked
   over one witness per kind — is what keeps the sentence honest.
2. **Satisfying a constraint is checked at the instantiation**, and then the
   instance needs no re-checking. That is the payoff of checking the body once,
   and it is why the lowering can substitute and emit with confidence.

The counter-example is C++, and Go's document names it: the constraints get
*derived from whatever the body happens to do*, so a call `v.String()` inside a
template that is never instantiated with a stringy type is fine until one day it
is not, and then the error surfaces at the *call*, *'as there may be several
layers of generic function calls before the error occurs, all of which must be
reported to understand what went wrong.'* C++'s own answer to that is two-phase
lookup — cppreference, *Dependent names*: *'Non-dependent names are looked up and
bound at the point of template definition. This binding holds even if at the
point of instantiation there is a better match.'* Our body is checked once, so
every name in it is bound once, at the declaration. The rule and its reason are
the same ones; the difference is that in C++ the *body* is still re-checked per
instantiation, because C++ has no constraints to make the once-check sufficient.

### The one place the body check consults the constraint

`let x: T = 1;` is not decidable from `T` alone: `1` is an integer literal, and
this language refuses an integer literal in an `f64` position (measured above).
So the rule is stated in terms of the class and not in terms of the literal:

> **A literal is assignable to a parameter when the constraint's class is the
> literal's class.** `T: Integer` accepts `1`; `T: Float` accepts `1.0`; `T:
> Number`, which admits both kinds, accepts neither — because the body would then
> mean different things for an int and a float, and this language converts neither
> into the other without a cast.

That is Go's *'representable in all types in the constraint set'*, with our
no-int→float rule applied on top of it, and it is the only rule in the body check
that needs the constraint for anything other than 'may this operator be used'.

When the class admits it, the literal is **decided as the binder** — not as its
own default. That is what makes the value right in every instance: the text is one
`1` in the source, and the type travels through the substitution boundary like
every other type of the node, so `fn T zero<T: Integer>() { return 1; }` gives an
`i32` `1` for one instance and a `u8` `1` for another from one body.

The rule is written **once**, in `support::literalAdmittedBy`, and asked by the
three sites that can meet it (`decideAt`, `checkAssignable` and the binary case
where a literal sits beside a binder); the sentence that reports a refusal is
`sema::Checker::refuseLiteralInBinder`, also once. A rule asked in three places is
a rule that drifts, which is why neither half is written out at a call site.

**The seam this leaves, named.** A `Number` body cannot write a constant at all:
`(a + b) / 2` is refused, because `2` would have to be `2i32` in one instance and
`2.0` in another. The honest fix is the *type's own* constant (Rust's associated
constants, `T::ZERO`), which needs `interface` and is the same slot as
`Fn`-shaped constraints below. Until it exists, a body that needs two writes
`Number`-shaped arithmetic over the values it was given, or a class whose members
are one kind (`Float` with `2.0`).

### `Fn`, and why it is not here

A generic that takes a callback — `fn i32 apply<F>(f: F, x: i32) { return f(x); }`
— needs two things that do not exist: a constraint that says 'callable with this
signature', and a **function type writable in a type position** (measured:
`fn i32 apply(f: ???)` is `error[parse-expected-type]: expected a type`; a
function *value*'s type is `fn i32(i32)` and nothing can spell it). Both are
named, neither is in this record, and the second is the one to build first.

## 7. Symbols: the mangling generics force

There is no mangling today: `Lowering::linkageName` returns the source name, so
two instances of `identity` would be one symbol. That is the first decision
generics force, and it is a link-time correctness decision, not a cosmetic one.

The scheme is `__M<name><args>`, and each argument is a mangling of its own. It
needs no escaping, and the reason is a measured property of the vocabulary: the
primitive names are `i8 … i128`, `isize`, `u8 … u128`, `usize`, `f32`, `f64`,
`f80`, `bool`, `char`, `str` — all lowercase, and **prefix-free** (no one is a
prefix of another), so a concatenation of them reads back unambiguously. So every
constructor takes an uppercase letter, which no primitive begins with:

```
i32              a primitive, by name
Pi32             *i32          a pointer
Si32             []i32         a slice
A4_i32           [4]i32        an array (the `_` ends the count)
T2_i32bool       (i32, bool)   a product: the count, then that many manglings
Fi32_1_i32       fn i32(i32)   a function: the return type, the count, the parameters
4_Name           a future named type, by length
```

so the symbols are

The name is **length-prefixed** too, which is what delimits it without a
separator, and every argument is self-delimiting:

```
__M8_identityi32
__M8_makePairi32bool
__M5_countSi32
__M7_firstOfT2_i32bool
__M4_pickPi32A4_u8
```

### Three decisions, and the first reverses an earlier draft

**1. Only `[A-Za-z0-9_]`, which is what the market's most carefully-argued scheme
says.** Rust's v0 mangling lists its properties, and states the reason:
*'Symbols can be restricted to only consist of the characters A-Z, a-z, 0-9, and
_. This helps ensure it is platform-independent, where other characters might
have special meaning in some context (e.g. `.` for MSVC DEF files).'* The first
draft of this record used `$`, and it **was** measured through the path this
compiler uses — LLVM emitted `identity$i32` for all three targets and the linked
binary exited 7:

```console
$ for tgt in x86_64-pc-linux-gnu x86_64-w64-windows-gnu x86_64-apple-macos; do
>   clang -x ir -c t.ll -o t.o     # 'define i32 @"identity$i32"(i32 %v)'
>   llvm-nm t.o | grep identity
> done
x86_64-pc-linux-gnu        emitted   T identity$i32   T makePair$i32$bool
x86_64-w64-windows-gnu     emitted   T identity$i32   T makePair$i32$bool
x86_64-apple-macos         emitted   T identity$i32   T makePair$i32$bool
$ clang t.o -o t.bin && ./t.bin ; echo $?
7
```

That measures the **emitter and nothing else**. What it does not measure is every
tool that reads the object afterwards — a `.def` file, an export list, `dlsym`, a
linker script, a debugger's symbol parser — which is the half Rust's property is
about, and the half this project's cross-platform rule is about. `$` is also not
a C identifier character, so such a symbol could never be named from a C header or
an `extern` export list. The safe set costs two characters of readability and
removes a whole class of question, so the safe set wins.

**2. `__M` is the leader, and the language reserves the `__` prefix.** With the
separator no longer a character the language cannot produce, unreachability has to
come from a rule, and it comes from the smallest one that works: a declaration
whose name **begins** with `__` is refused, exactly as `__builtin_` already is
(`support::builtins::isReservedPrefix`, widened from the prefix to the prefix
class). That is C's rule — *the implementation reserves `__*` and `_[A-Z]*`* — and
Rust's (`__rust_*`). It buys three things at once: the mangling is unreachable
from source, the symbol is a valid C identifier, and a future `cinterop` export
list or `dlsym` can name one.

**3. The length prefixes are what make the encoding decodable, not a separator.**
A name is `<decimal length>_<name>` and a count is `<decimal count>_`, which is
what Itanium does (`_Z8identityIiET_S0_`) and what v0 does (`_RNvCs15kBYyAo9fc_7mycrate7example`)
— and the reason is that both had to be *decodable* as well as injective. A named
type is `4_Name`, so a digit can never start a primitive and a length prefix
inside an argument cannot be confused with the digits of a count; primitives are
prefix-free (measured above), so a run of them reads back greedily and
unambiguously.

Two properties the scheme must keep, and they are why it is *readable* instead of
a hash:

- **Deterministic across compilations.** Rust's legacy mangling folds a hash of
  the crate's metadata into the symbol (`_ZN2mg8identity17ha40aaaaaa9cd2685E`,
  measured), which is why two instantiations of the same function carry
  indistinguishable demangled names and why the scheme had to be redesigned.
  Ours is a function of the declaration's name and the arguments, so two units
  compiling the same instantiation produce the same symbol and the linker
  de-duplicates them.
- **Ready for a unit.** When modules land, two units may each declare an
  `identity`; the encoding has room for a unit component in front of the name,
  and the record does not decide the module syntax, only that the mangling is
  `(unit, name, args)` with the unit empty today. `modules.md` is where
  `Lowering::linkageName` gets that component.

## 8. Debug information: one subprogram per instance

The bar is measured, from both leaders, on a generic function instantiated twice:

```console
$ llvm-dwarfdump --debug-info mg_cpp.bin | grep -A6 'DW_AT_name.*identity'
                DW_AT_linkage_name  ("_Z8identityIiET_S0_")
                DW_AT_name          ("identity<int>")
                DW_AT_decl_file     ("/tmp/gen/mg.cpp")
                DW_AT_decl_line     (2)
                DW_AT_type          (0x0000002b "int")
$ nm mg_cpp.bin | grep identity                        # C++: the arguments are in the symbol
_Z8identityIdET_S0_ ,  _Z8identityIiET_S0_
$ nm mg_rs.bin  | grep identity                        # Rust: a hash is in the symbol
_ZN2mg8identity17ha40aaaaaa9cd2685E , _ZN2mg8identity17be6506d38a70d3c5E
```

Both emit, **per instance**: a `DW_TAG_subprogram` whose `DW_AT_name` is the
*human* spelling with the arguments (`identity<int>` / `identity<i32>`), whose
`DW_AT_linkage_name` is the raw symbol, whose `DW_AT_decl_line` is the
**declaration's** line (2 in both files, not an instantiation site), and whose
`DW_AT_type` and formal parameters are the **instantiated** types. Both also emit
`DW_TAG_template_type_parameter` per binder (12 DIEs in the C++ object, 15 in the
Rust one).

And the capability that falls out of naming the DIE `identity<int>` — which is
the reason to copy that spelling rather than invent one:

```console
(gdb) info functions identity
2:      int identity<int>(int);
2:      double identity<double>(double);
(gdb) break identity                    # the generic name, no argument list
Breakpoint 1.1, identity<int> (value=7) at mg.cpp:2
```

A regex on the bare name matches **every** instantiation, and the breakpoint is a
multi-location breakpoint. Rust's gdb session is the same shape
(`static fn mg::identity<f64>(f64) -> f64`).

So the `-g` contract is: one instance, one subprogram DIE, `DW_AT_name` =
`identity<i32>` (the same spelling a diagnostic prints, one formatter), the
declaration as the source position, the instance's types on the parameters, and
one `DW_TAG_template_type_parameter` per binder. `DebugInfoBuilder::enterFunction`
*already* takes `name` and `linkageName` as two arguments (`debug.h`: *'`name` is
the source spelling, `linkageName` the symbol the linker sees'*), which is exactly
this slot — the instance form goes in the first and the mangled symbol in the
second, and that signature does not change.

### Why monomorphization and not dictionaries

Go compiles generics the other way — one function body per *shape*, with a
dictionary passed at every call — and its own design document states the debug
cost: *'For each argument or local variable that has a parameterized type, the
DWARF info also indicates the dictionary entry that will contain the concrete
type of the argument or variable.'* The debugger then resolves a variable's type
*at run time*, through the dictionary. Our `value` in `identity<i32>` is `i32` in
the DWARF, statically, because the instance is a real function with real
parameter types. That, plus the language having no interfaces for a dictionary to
carry, is the whole reason for choosing monomorphization; the cost (code size,
compile time) is the cost every one of these leaders pays.

## 9. Refusals

Each with its own code and sentence, each at the earliest stage that can decide:

| written | refused because |
|---|---|
| `extern fn T identity<T>(value: T);` | a generic has no single C symbol; the ABI boundary is one signature |
| `extern fn i32 f<T>(x: T, ...);` | the C ABI is neither generic nor its variadic tail parameterized |
| `fn i32 main<T>()` | the entry point is one function with one signature |
| `fn i32 f<T, T>()` | one name, one binder |
| `fn i32 f<i32>()` | a binder may not shadow a word of the language (`type_alias.md` decision 4, one namespace) |
| `let g = identity;` | a generic name is **not a value**: it has no type until it has an argument list. `let g = identity::<i32>;` is a function value |
| `let p: Pair = ...;` | a generic name in a type position needs its arguments: a type has no value to infer from |
| `let p: Pair<i32> = ...;` | arity: names both numbers |
| `zero();` | nothing infers `T`; names the binder and the fix (`zero::<i32>()`) |
| `fn i32 f<T>() { return T; }` | a type is not a value; that sentence already exists |
| `type A<T> = A<T>;` | a cycle, by the rule `type_alias.md` already states for an abbreviation |
| `identity::<void>(x)` / `identity::<!>(x)` | **every type argument is an object**, the rule `[N]T` and `(T, U)` already state for their parts, with the same sentence |
| `identity::<str>(x)` where the body uses `+` | the argument does not satisfy the constraint; the class is named |
| `f::<[]T>(...)` where the solution mentions its own binder | no finite instance exists (§ *Occurs*) |
| an instance list past its budget | § *Instantiation*; names the declaration, the count and the limit |
| `x as Pair<i32, bool>` | a `<` after a type in a **cast** is a comparison (`x as i32 < 3`), so the list cannot be read there. Refused by name (`parse-cast-to-generic-type`), with the list consumed so one mistake stays one sentence and the sentence names the fix: a generic type *is* the type it abbreviates |
| a structure past `kMaxTypeNodes` | the store is a DAG, so a type that reuses a large one twice per level doubles per level; refused where it is built, before the arithmetic that would lay it out |

Two of those deserve the note that they are *already* right: a `void` type
argument becomes `fn void identity(value: void)` after substitution, which the
existing parameter rule refuses with its existing sentence, and a `!` argument
becomes a parameter of type `!`, refused the same way. **Substitution happens
first and the rules that already exist do the judging** — the reason generics need
no second validity checker.

## 10. What this must not break

Measured facts about the language as it stands, each of which constrains the
design above:

- **Identity is structural and an alias is transparent.** So an instantiated
  alias is the type it abbreviates and the store must not grow (§ 5). A test
  compares `TypeStore::count()` before and after.
- **A function is already a value**, with a type: measured, `let g = main;` gives
  `g` the type `fn i32()` and `g()` calls through it, and the diagnostic spells
  it `fn i32(i32, i32)` for a two-parameter function. So the refusal for a bare
  generic is a refusal about *this* value system, and a generic instantiation must
  be a normal function value: `let g = identity::<i32>; g(7);` has to work.
- **`let f: f64 = 1;` is an error and `let f: f32 = 1.0;` is not.** Inference
  invents no conversion, and the literal-vs-parameter rule is stated in terms of
  the constraint's class (§ 6).
- **`*void` and `*T` convert to each other.** So a type argument that is a
  pointer is comfortable, and `identity::<*void>` is a legal instance.
- **`__builtin_` is reserved** (`support::builtins::isReservedPrefix`), and the
  mangling needs a leader the language cannot declare. It takes the reserved space
  itself (`__M`), which widens the reservation from `__builtin_` to the `__`
  prefix — one rule, and the mangling's unreachability follows from it (§ 7).
- **The lowering has per-unit tables keyed by def** (`locals_` cleared per
  function, `functions_` keyed by `DefId`) and the checker has per-node tables
  (`defTypes_`, indexed by `DefId`). Lowering a body **twice** means the function
  table becomes keyed by *(def, instance)* and the local table is cleared per
  instance, not per declaration — that is a change to `src/ir`, not a new idea,
  and it is the one place where the "one pass over the tree" assumption of
  `lowering.h` is deliberately given up.
- **`readType` is handed the unit's type names as data** and defaults to empty;
  binders are rows in that table (§ *Layers*).

## 11. The walk through the pipeline

Each step with the test it comes with.

1. **`lex`** — nothing. No new token, and that is a test: `$` is not lexable, `>>`
   is one token, `<` and `>` are what they are.
2. **`parse`** — `SyntaxKind::GenericParams` and a type-argument list;
   `parseGenericParams` in the two declaration positions; argument lists inside
   `parseTypeRun`; `::<...>` in `parsePostfix`; the `>>`/`>>=` splitting in the
   argument-list reader; recovery for a missing `>`, an empty `<>`, a missing
   binder name, a stray `,`. Tests: the shape, each type form as an argument,
   nesting, each recovery, and the leaves concatenating back to the source.
3. **`ast`** — lower and validate the two new nodes: a binder list is `Name` per
   binder (plus its constraint name), an argument list is one `Type` node per
   argument. Tests: a valid form lowers; a binder list with no name, an argument
   that is not a type, and an argument list in the wrong position are refused.
4. **`resolve`** — `DefKind::GenericParam` in `Tag`, scoped to the definition,
   duplicate refused with the first declaration in a note, `-Wshadow` through the
   same path, a built-in word refused. Tests: the def's kind and namespace; the
   scope ends with the definition; a binder is not visible in a *later*
   declaration.
5. **`sema`** — `TypeKind::Param`; the binder table handed to `readType`; the
   single body check under the constraints; the constraint table; the inference
   (equations, literals as candidates, the expected type, the occurs check);
   the instantiation worklist and its budget; `TypedFile::instantiations()` and
   `TypedFile::instantiationAt(callNode)`. Tests: identity and storage of a
   `Param`; `spelling` of a parameter; each refusal in § 9; inference from
   arguments, from the expected type, and the 'cannot infer' sentence; a literal
   defaulting; a nested constructor; a param-in-param; the worklist's closure and
   its budget; the store not growing for an alias; the dump text.
6. **`ir`** — `linkageName` gains the mangled form; `declareFunctions` becomes
   per instance; the substitution applied where the lowering reads a type from
   `sema`; `lowerBinding`/`lowerExpr` able to run more than once per node; the
   DWARF subprogram per instance with `DW_AT_name`, `DW_AT_linkage_name`,
   `DW_AT_decl_line` and one `DW_TAG_template_type_parameter` per binder. Tests:
   two instances are two functions with two symbols; each instance lowers; a
   `Param` reaching the lowering is an `ir-internal` refusal (the invariant); the
   DWARF read back with `llvm-dwarfdump`, and a gdb session that breaks on the
   bare name and sees both instantiations.
7. **`driver` / CLI** — `check`'s summary and dumps, `ir`, `build`, `run`, plus
   `examples/021_generics.mx` that `make examples` checks, dumps and runs.
8. **`docs`** — the site's language pages, the feature checklist, and the
   roadmap item pointing here.

## What has landed

The whole of it: the **parser**, the **alias**, the **generic function** and the
**constraint**, from the declaration to the emitted instance.

| Where | What |
|---|---|
| `lex` | `::` is a token, longest match over `:`, next to `..` and `##`. Two characters this language writes together; a tokenizer that split them would leave the parser to decide from adjacency alone |
| `parse` | `GenericParams` (binders, after the name of a `fn` or a `type`) and `TypeArgList` (arguments, in a type run or behind `::` of a call, one `Type` per argument). `parseTypeRun` gained the `<` branch and became a **conduit** for a compound closer; `parseGenericParams`, `parseTypeArgList`, `closeList` and `reportUnusedListClose` are the list reader, and `parseType` returns what its closer consumed |
| `parse` | Two new codes, and only two: `parse-expected-type-arg-close` (a list with no `>`) and `parse-stray-type-arg-close` (a `>`/`=` that closes nothing). The `:` of a constraint reads a `Constraint` node holding a `Name` — the same shape a `type` use has, so `resolve` treats the word as a language word and not as a user binding, and no stage has to special-case it. `parse-constraint-not-read` refused `<T: Ordered>` while the checker could not read it; the code is **retired** with the stage that made it temporary |
| `support` | `support/constraint`: the class table, one row per class — the name, the member predicate *named* rather than encoded, the grant bit set, and the literal class. `constraintForOperation` answers the refusal's "write this one", and it picks the **least powerful** class that grants the operation |
| `sema` | the class on a `Param`: `BinderRow` carries the spelling, the `Param` and the class, read **once** per declaration by `readBinderRows`, so the two passes over a signature (signature, then body) cannot produce two classes for one binder or two sentences for one fault. A `Param`'s class is *not* part of its identity — `equalFields` compares the explicit field list — so `T` under two constraints is one `Param` |
| `sema` | the **body check**: `refuseOperation`, called at every site where an operation is about to be allowed (binary, unary, the step operators, and the condition of a control statement), refuses a binder whose class does not grant it and names the class to write — or says there is none, when the two classes share no member (`classesOverlap`, one witness per kind). A binder is granted the *operation*, and the concrete rules below it are skipped: the class is the promise and there is no width to promote to |
| `sema` | the **satisfaction check** at the instantiation and not at the call: `satisfies` answers each class with the operation rule's own predicate, and `internInstance` asks it before the substitution, so an inadmissible argument produces no instance. One sentence per *(node, argument list)* — a call inside a generic body is expanded once per instance of the enclosing declaration, and two expansions of one call are one fact about the source |
| `sema` | the **literal rule** in one place: `support::literalAdmittedBy` decides and `Checker::refuseLiteralInBinder` reports, asked by `decideAt`, `checkAssignable` and the binary case. The literal is **decided as the binder**, so one body gives the right value per instance |
| `sema` | the class of a binder reaches the **type reader** through the name row: `TypeName::rows` carries the declaration's `BinderRow`s (one struct, `typespec.h`, so the checker's table and the reader's input cannot disagree), and a use of a generic *alias* is checked against them where the substitution happens — `Vec<bool>` is refused, `Vec<i32>` is not. The predicate mapping moved to `TypeStore::satisfies`, which is where the predicates it names already live |
| `ir` | nothing new, and the rule that made it nothing: a binder operation is lowered at the **instance's** type, because the substitution happens at the checker's boundary — `typeOf` and `opTypeOf`, the latter being the one accessor that reads a record's *type* instead of its flags, precisely because `infoOf` hands back what `sema` wrote and a `Param` reaching `llvmType` is an internal error. A binary expression stores no `opType` at all: its operation happens at its left operand's type, and `lowerBinary` reads that from the operand |
| `ir` | two **latent gaps** closed, both reachable only once a float could be an operand: a negation is `sub 0, x` for an integer and `fneg` for a float, and a step adds one *of the operand's own kind* (`stepValue`). Both were single-branch code that built a module the verifier refuses (`sub double 0.0, x`, `add double %x, i0 0`), and both are now chosen by the operand's type |
| `parse` | The scanner that splits `fn`'s return type from its name treats an identifier's `<...>` as **part of the word** and remembers the name as the word's *first token*: `fn Vec<i32> f<T>()` is the type `Vec<i32>`, the name `f`, and the binders `<T>`. The lookahead's copy of the closing rule is `skipTypeArgList`, held to `closeList` by the tests that pin all four spellings |
| `ast` | nothing: the tree lowering is structural, so both nodes ride it. The `NodeKind` values are `GenericParams` and `TypeArgList` |
| `a declaration's return type` | read by the **type reader** and not walked token by token, and that needed a mechanism the grammar did not have: a **bound**. The scan (`scanTypeRun`) finds where the name begins, and `parseBoundTypeRun` reads up to that token with `TokenBound` making `atEnd()` true there — so `fn Vec<i32> f()` gets a `TypeArgList` like every other type position instead of the flat words `Vec < i32 >`, and `fn Pair<T, K> make<T, K>()` is a return type the reader below can resolve. A bound is a token *range*, restored by RAII; a flag on the parser would be state that survives the token stream going the wrong way, which is the failure this parser does not recover from |
| `sema` | the **instance**: `runInstantiations`, a worklist keyed on `(declaration, arguments)` seeded by the concrete calls and expanded per instance of an enclosing generic, with `options.maxInstances` as the budget and `CallTarget` recording which instance each **call site** reaches — keyed on the pair, because one call node inside a generic body is lowered once per instance of the function that contains it |
| `sema` | the **two names** of an instance: `identity<i32>` (what a diagnostic and a `DW_AT_name` print) and `__M8_identityi32` (what the linker sees), both derived from `(name, arguments)` |
| `ir` | a **function per instance**: `defineFunction` takes the instance's substituted signature, the body is lowered once per instance with the binders replaced, the debug record emits one `DW_TAG_subprogram` per instance with `DW_AT_name = identity<i32>` and a `DW_TAG_template_type_parameter` per binder, and a call reaches its instance through `callTarget` |
| `driver` | `check --ast` prints the instance table (`# instances`) — the one fact the tree cannot show, because every call says `identity` and the *set* of functions is what changes |
| `sema` | one new kind, `Param`, whose identity is `(owner, binder)` and whose spelling is a third fact kept beside it; `TypeStore::substitute`, which rebuilds a type through the same builders so the *result* meets the array and product rules a written type meets; `TypePart.hasArgs` and `TypeName.{binders, owner}`, which is how a use reaches the template; and the three refusals a use can produce — a generic name with no arguments, the wrong count, arguments on a name that takes none — each a `sema-malformed-type` sentence that names the numbers and the spelling to write |
| `sema` | a **gate on functions only**: `sema-generics-not-read`, once per unit, when a `fn` declares binders — its parameters, its body and its instantiations are the next stage. Without it, a binder would be an unknown type name with a "did you mean `i8`?" note, which is a sentence about a typo for something the reader wrote on purpose |
| `ir` | nothing new, and an **invariant** added: a `Param` reaching the LLVM mapper or the debug records is `ir-internal`. Instantiation substitutes every binder before a module is built, and a generic *alias* use is already a concrete type by the time the lowering sees it — `Rows<i32>` is `[4]i32` |
| `examples` | `021_generic_alias.mx`: nested uses, a constructor over a use, a use in a product member, a count that comes from the argument, and an alias built on another. `022_generics.mx`: a binder in the parameter and in the return type, two binders, a generic alias as the return type, an explicit `::<...>` call, a template calling a template with its own binder, and the instances all of it produces. `023_constraints.mx`: every class, the arithmetic/sign/step/bitwise grants, the literal rule, and the eight refusals written out in the compiler's own words. All three are lexed, parsed, resolved, checked and lowered by `make examples` |
| `tests` | `tests/unit/sema/constraint_test.cc`: the **matrix** (one body using every grant of a class, instantiated at every member of it, plus every type the class excludes refused at the instantiation) — which is the invariant of § 6 as a test rather than as an argument — plus the inclusion rows, the literal rule's four shapes, the `bool`-only positions, the two-binders case, and the dedupe. `tests/unit/ir/lower_test.cc`: the negation and the step at the operand's own kind, in a written type and in a substituted instance |

Two bounds came out of this stage, and both are `support/limits.h`: the
**instance budget** (`SemaOptions::maxInstances`, reported once, § 5) and
`kMaxTypeNodes`, the number of types one *type* may be made of. The second is
worth a sentence here because it was found by building this stage and not by
reviewing it: `type P1 = (P0, P0);` doubles per line, so sixteen lines is a
million-node structure, and the store is a **DAG** -- interning means the two
halves of `(P0, P0)` are one type with two parents. Every walk that read the
type as a *tree* was therefore exponential in its depth, and the layout was the
worst of them because it is asked per use. So: the structure is refused past
`kMaxTypeNodes`, before the arithmetic that would lay it out, and `size`,
`align` and `unknownSize` are **stored** on the `Type` when it is built
(`layoutOf`, in `type_store.cc`) instead of recomputed by walking. A 2^14-node
alias chain went from 483 ms to 56 ms, and the refusal from 501 ms to 22 ms.

Tests: `tests/unit/parse/generics_test.cc` (19 cases: both declarations, the
nested `>>` and `>>>` splits with the *token* still one token in the tree, `>=`
and `>>=` reaching the binding that owns the `=`, the three refusals, the two
call-site forms), `tests/unit/sema/generic_alias_test.cc` (16 cases: the
substitution and its structural identity, nesting, a constructor around a use, an
array whose size comes from the argument, one generic alias built on another, a
use inside a product member, two declarations each calling their binder `T`, and
every refusal with the numbers its sentence names),
`tests/unit/sema/generic_call_test.cc` (11 cases: an explicit instance and its
two names, inference from an argument, two calls collapsing to one instance, a
binder nothing decides naming the form to write, a generic alias as a return
type, a binder under `/` needing its constraint, the wrong count, an argument
that cannot be stored, a doubling structure refused by name rather than built,
and a wide product that is ordinary), the `::` row in the lexer's punctuator
table, and one input for each new code in the tables that prove every code is
reachable.

## Decisions

| # | Decision | Why |
|---|---|---|
| 1 | **Binders follow the name being declared; type arguments follow the name being used** | The return type is a run of words, so anything between `fn` and the name is part of the type; after the name, `(` already ends it. `type_alias.md` left this slot in the production with the same reasoning |
| 2 | **`::<...>` at a call site, plain `<...>` in every type position** | In a type position `<` can only be an argument list, so nothing is ambiguous. In an expression `f < T > (x)` *is* a comparison, so a bare `f<T>(x)` is a rule the parser can only apply by guessing — Rust reached the same conclusion and that spelling is the market's, not ours to invent |
| 3 | **`::` is also where modules are going** | `modules.md` Q4 lists `a::b::c`; `path::<args>` and `path::segment` are one rule. This record does not decide modules, and it must not close that door |
| 4 | **The list reader splits `>`/`>=`/`>>`/`>>=` by *returning what it consumed*, all four cases** | Measured: our lexer is maximal-munch, so `>>` is one token and so is `>>=`; measured: rustc splits both, clang++ accepts `>>` (C++11) and still refuses `>>=`. Splitting in the lexer instead would put a lexical concern into `sema` and `ir`, which both read the operator token to decide the operation; a `Parser` flag would leave state behind. A returned `ListClose` does neither |
| 5 | **One new kind, `Param`, with identity `(owner, index)` and the source spelling kept** | The store's answer to 'same type?' is structural, so a parameter needs an identity that is stable and cannot collide between two declarations. The spelling is kept because `spelling` is what a diagnostic prints and it has no other source |
| 6 | **Substitution is total for the declaration it belongs to, so there is no capture and no alpha-renaming** | A substitution is built for one binder list and covers all of it; a body referring to an *enclosing* binder keeps that `Param`, which is how a generic calls a generic |
| 7 | **The body is checked once, abstractly, under the constraints** | Go states the rule (*'may only use operations supported by all the types permitted by the constraint'*) and the reason (*'we don't want to derive the constraints from whatever the body happens to do'*); cppreference states C++'s *two-phase* answer for the same problem. C++ still re-checks per instantiation because it has no constraints to make one check sufficient |
| 8 | **A generic alias is an abbreviation: instantiation produces a type the store already has** | `type_alias.md` decision 2 carried one step. `Pair<i32, bool>` *is* `(i32, bool)`, the check is a `TypeId` equality, and the store does not grow — a stated invariant with a test |
| 9 | **Inference is first-order unification from arguments and the expected type; no generalization** | There are no polymorphic values to generalize, and every constructor is first-order, so a solution is unique when it exists and 'cannot infer' is a fact about the program |
| 10 | **A literal never decides a binder, and a binder is never a literal** | `identity(5)` is `i32` and `identity(5.0)` is `f64`, by the defaulting rule `let x = 5;` already follows. A parameter typed `<integer literal>` would be a second kind of deferred type for no gain |
| 11 | **A literal is assignable to a parameter only when the constraint's class is the literal's class** | Measured: an integer literal in an `f64` position is an error here, so `T: Number` cannot accept `1`. Go's *'representable in all types in the constraint set'* with our strictness on top — and the seam that follows (a `Number` body needs `T::ZERO`) is named in § 6 |
| 12 | **Constraints are compiler-known capability classes, from the measured operation table** | There is no `interface` yet, and inventing one for this is designing two features at once. Each class is the set the existing operation rules already accept, so the body check reuses the sentences that exist |
| 13 | **Only the constrained operations are legal on a parameter, checked at the declaration** | Otherwise the constraint would be documentation and the real rules would be the ones C++ has |
| 14 | **Instantiation is a worklist keyed on `(declaration, arguments)`, expanded with the instance's substitution** | The arguments inside a generic body are written in terms of its binders, so the set cannot be enumerated by one walk of the program (`id<i32>` and `id<f64>` come from one body). A repeated key is not re-expanded, which is what makes a recursive generic terminate |
| 15 | **The instance list has a budget, and overrunning it is a diagnostic** | `g::<*T>` grows forever; a compiler that follows it exhausts memory instead of reporting. rustc reports 'reached the recursion limit while instantiating' |
| 16 | **The symbol is `__M<name><args>`, restricted to `[A-Za-z0-9_]`, with the `__` prefix reserved by the language** | Rust's v0 mangling states the reason to stay inside `[A-Za-z0-9_]`: *'other characters might have special meaning in some context (e.g. `.` for MSVC DEF files)'*. The `$` this record first proposed was measured through LLVM for ELF, COFF and Mach-O — but that measures the emitter, not the tools that read the object afterwards, and it is not a C identifier character. Unreachability from source then comes from a rule (C's `__*` reservation) instead of from a character, and the length-prefixed name is what delimits it without a separator |
| 17 | **The mangling is readable and deterministic, not a hash** | Rust's legacy mangling folds a metadata hash into the symbol (measured), which is why its demangled names cannot distinguish two instantiations and why it was redesigned. A function of the name and the arguments de-duplicates across units |
| 18 | **One subprogram DIE per instance: `DW_AT_name` is `identity<i32>`, `DW_AT_linkage_name` is the symbol, `DW_AT_decl_line` is the declaration, one `DW_TAG_template_type_parameter` per binder** | Measured from clang and rustc, and measured again in gdb: `info functions identity` lists every instance and `break identity` is a multi-location breakpoint |
| 19 | **Monomorphization, not Go's dictionaries** | Go's own document says its DWARF *'indicates the dictionary entry that will contain the concrete type'* — the debugger resolves a variable's type at run time. Here `value` is `i32` in the DWARF, statically, because the instance is a real function |
| 20 | **A `Param` never reaches `ir`; the lowering stopping on one is an internal error** | The invariant that makes the substitution boundary a fact instead of a hope, and it is the one thing a test can assert for the whole stage |
| 21 | **`extern`, variadic, `main`, a repeated binder, a built-in word as a binder, a bare generic name as a value, and a generic name without arguments in a type are all refused** | Each has no meaning rather than a meaning we would have to invent, and the earliest stage that can decide decides |
| 22 | **A declaration's return type is read against a *bound* the scan found, and the bound is a token range restored by RAII** | The type reader cannot stop (a word after a type is another word -- `fn Vec<i32> f()` would swallow `f`) and the scan cannot parse. Rather than a flat token run in the tree -- which is what made `fn Pair<T, K> make<T, K>()` unresolvable -- the run is read by the reader with `atEnd()` true at the token the scan named. A *range* and not a flag: a flag is state that survives the stream going the wrong way, and this parser recovers by bailing out, not by resynchronizing |
| 23 | **The type store is a DAG, so its layouts are stored and its structures are bounded** | Interning makes `(P0, P0)` one type with two parents, so a walk of the *tree* it spells out is exponential in depth; `kMaxTypeNodes` refuses a structure past 65536 nodes before the arithmetic, and `size`/`align`/`unknownSize` are computed once, when the type is built. Measured: a 2^14-node alias chain 483 ms → 56 ms, and the refusal 501 ms → 22 ms |

## What is deliberately not done here

- **No user-declared constraints** (`interface`/`trait`/concept). The syntax has
  one slot for one (`<T: Name>`) and the constraint table is a table, so this is
  one more row when the declaration exists — with the machinery for
  user-defined types, not before.
- **No `const`/value binders** (`type Buf<N> = [N]i32;`). It is a second kind of
  binder with a second kind of argument, and it depends on a count that is
  currently a literal.
- **No defaults** (`<T = i32>`) and **no `where` clause**. Both are surface for a
  problem the inference already solves; a default is worth adding when a real
  program needs it, and it would be the only place a `>>` appears inside a binder
  list.
- **No higher-kinded or associated types**, no specialization, no variadic
  binders (`<T...>`), no template metaprogramming. Each is its own feature with
  its own record; none of them is reachable from `Param` by accident.
- **No generic `struct`/`enum`.** They need nominal identity, which the store does
  not have yet (`modules.md` S4, `type_alias.md`'s *Seams*). This record is the
  substitution and the instantiation; the nominal half is the aggregate record's.
- **No `Fn`-shaped constraint**, because a function type cannot be written in a
  type position yet (§ *`Fn`*).
- **No inlining or instance sharing.** An instantiation is a function until an
  optimiser says otherwise, and 'two instances with one body' is a Go shape.

## References

Read while writing this, and quoted where quoted:

- The Rust Reference, *Generic parameters*: the `<...>` position *'usually
  immediately after the name of the item'*, `TypeParam → IDENTIFIER ( :
  Bounds? )? ( = Type )?`, *'Generic parameters are in scope within the item
  definition where they are declared'*, the const-argument restriction
  (*'must be a block expression ... unless it is a single path segment or a
  literal'*) and its stated reason — *'necessary to avoid requiring infinite
  lookahead when parsing an expression inside of a type'* — which is the general
  form of § 3's future case.
- cppreference, *Dependent names*: binding rules for non-dependent names
  (*'looked up and bound at the point of template definition ... even if at the
  point of instantiation there is a better match'*) and the lookup rules for
  dependent names with the ODR example they exist to prevent.
- The Go *Type Parameters Proposal* (Taylor, Griesemer): the constraint rule for
  bodies, the explicit rejection of C++'s error style (*'these errors can be
  lengthy, as there may be several layers of generic function calls'*), and
  *'Type inference via a unification algorithm permits omitting type arguments
  from function calls in many cases.'*
- The Go design document *Generics implementation: dictionaries and gcshape
  stenciling*: a dictionary per call site, `main..dict.Map[int,bool]` as a name,
  and the debug consequence quoted in § 8.
- Measured locally, not quoted: the two `.ll`/`.rs`/`.cpp` symbol tables (`nm`,
  `llvm-nm`); the DWARF of both (`llvm-dwarfdump`: `DW_AT_name` `identity<int>`,
  `DW_AT_linkage_name`, `DW_AT_decl_line`, `DW_TAG_template_type_parameter`
  counts); gdb's `info functions` and a multi-location `break identity`; rustc
  splitting `>>` and `>>=` while clang++ refuses `>>=`; LLVM emitting
  `identity$i32` for ELF, COFF and Mach-O through the IR path, and the linked
  binary exiting 7; the current compiler's operation table (`%` on `f64`,
  `<` on `str`, `==` on a product, `let f: f64 = 1;`, `let f: f32 = 1.0;`, `let g
  = main;` and `g()`); the identifier charset in `src/lex/byte_class.h`; the
  `GreaterGreater`/`GreaterGreaterEqual` rows in `src/lex/lexer.cc`.
- This project: `type_alias.md` (identity, the `Tag` namespace, decision 10's
  name table, and the `Generics` seam it left), `sema.md` (the type model and
  interning), `parser.md` (types are positions, not kinds), `tuples.md` (the
  arity-≥-2 product that makes a multi-value return possible, and the
  `firstParam`/`paramCount` sequence this record's `Param` sits beside),
  `arrays.md` (`[N]T` and the count), `never.md` (why a type argument can be
  refused by an existing rule), `extern.md` (the ABI boundary and variadics),
  `memory.md` (the access model the body check inherits), `modules.md` (Q4 and
  `Lowering::linkageName`), `ir.md` / `codegen.md` (the lowering and the debug
  info), `lexer.md` (why the split is the parser's), `cli.md` (the commands a new
  example runs through).
