# The bottom type — `!`

`!` is the type of an expression that never produces a value. A function declared
`fn ! name(...)` never returns to its caller, and every call to it is an
expression of type `!`.

The whole feature is two rules, and everything else below follows from them:

1. **`!` converts into every type, and nothing converts into `!`.** A value of
   type `!` is never produced, so "it becomes a `T`" is not false — it is
   *vacuous*. That is what makes `c ? 1 : die()` an `i32`, `let x: i32 = die();`
   legal, and `return die();` legal in a `void` function.
2. **`!` is written only as a return type.** `let x: !`, `p: *!` and `f(x: !)`
   are refused, because a type with no values cannot be the type of an object,
   and the reader should not have to guess which of the two value-less types
   they wrote.

The first rule is checked by the type system, for free, in every position a value
can appear. The second is a rule about *writing*, enforced where the position is
known. And one thing a type cannot check by itself — a body that promises
divergence — is *proved* by the checker (§6).

Nothing else in the pipeline had to learn a new concept: the lexer already had
`!` as a punctuator, the grammar already had a type position, the conversion
rule already existed, and the IR already had `noreturn` and `poison`. This
document is mostly the argument that each of those was the right thing to reuse.

## 1. Why `!` and not `never`

The bottom type has two spellings in the market, and the split is not stylistic:

| | `!` | `never` |
|---|---|---|
| Languages | Rust, Swift (`Never`), Zig (`noreturn`), Scala (`Nothing`), TS (`never`) | — |
| Lexical cost | zero: `!` is already a punctuator | a keyword, reserved for the rest of the language's life |
| Collision | impossible: punctuation cannot be an identifier | `fn never f()` is ambiguous with a user type |
| Position | unmistakable at a glance next to `void` | reads like a sibling of `void` |

The deciding argument is the second row of that table taken with §2's rule. A
return type in this grammar is a *bare position* — `fn TYPE name(...)` — so a
word there is indistinguishable from a user-defined type name until something
else says otherwise, and a language that one day has `struct`/`type` aliases
would have reserved an ordinary English word to say nothing more. `!` is a
punctuator, so the position itself is the answer, and there is no list of
reserved words to keep.

The honest cost is diagnostics: `` `!` is not a type an object can have `` reads
worse than `never` would. That is a price paid in error messages, not in grammar
or in the language's surface area, and it buys a keyword back.

## 2. Why a type and not an attribute

The first implementation of this feature was `@noreturn` — an attribute word
beside the signature, which is what C does (`_Noreturn`) and what LLVM calls the
function property. It was replaced, and the reason is worth writing down because
it is a general one:

| | `@noreturn fn i32 f()` (attribute) | `fn ! f()` (type) |
|---|---|---|
| Says what | a fact *beside* the signature | what the call *is* |
| `c ? 1 : die()` | not expressible: the arm is `void`, and `void` is not a value | an `i32`, because `!` converts |
| `f(die())` | not expressible | legal, and the argument is a `poison` |
| Every consumer | had to learn the attribute and consult a table keyed on the def | asks the ordinary type questions it already asks |
| Flow rule | `aborts(expr)` had to know the callee, the call shape and the word | `typeOf(expr) == !`, one line |

The attribute version worked — it fixed the false positive, it cut the edge in
the IR — and it could not be extended past the three shapes the attribute table
happened to cover, because an *annotation* is a fact a consumer has to look up
while a *type* is a fact that travels with the value. Rust reached the same
conclusion from the other direction and states it exactly: `!` coerces into any
other type (`RFC 2306`'s predecessor, `RFC 1216`, and `std::primitive::never`).

What survived the rewrite is the *proof*: §6 is the same check, keyed on the
return type instead of on a word. What did not survive is the mechanism — the
attribute list, the `@` token, the syntax node, the third def-keyed array, and
the two diagnostics that only existed to keep an annotation and a return type
from contradicting each other.

## 3. The conversion rule, and its direction

`convertible` gains one arm:

```
from == Never  →  true
```

and that is the whole of it. The asymmetry with the other direction is the
feature:

- **`!` into `T`** is vacuous rather than false: the expression never produces a
  value, so there is every `T` it will fail to produce. A consumer that asks
  "does the value convert?" is asking about a value that does not exist, and
  answering "no" would refuse a program the language should compile.
- **`T` into `!`** has no reading at all: a value that is *not* a `!` cannot
  become one, and a type that admits no values cannot be arrived at. It stays
  false, and surfaces as the type mismatch it is.

This is deliberately expressed as a **conversion** and not as subtyping, which is
where Rust puts it too — and the choice matters here for a concrete reason: this
language has no subtyping to hang a bottom type on, and a conversion is the
relation `checkAssignable` already asks about. The feature lands in the existing
rule instead of adding a second relation that every consumer would have to be
taught.

The consequences, one per position, all from the same line:

| Source | Why it works |
|---|---|
| `fn i32 f() { exit(1); }` | the call's type is `!`; the body cannot reach its end |
| `let x: i32 = exit(1);` | `!` converts to `i32`; the binding is never actually reached |
| `return exit(1);` in `fn void` | `!` converts to `void`, which is what "returns nothing" means |
| `take(exit(1))` | the argument converts to the parameter's type |
| `c ? 1 : exit(1)` | §5 |
| `x = exit(1)` | the assignment conversion, like any other |

## 4. Written where

`!` is accepted by the type *reader* as a whole run — `readType` answers
`kTypeNever` for a run of exactly one `!` — and refused by it the moment it is
combined with anything:

```
`!` is a type on its own: it cannot be combined with a type name or a `*`
```

`readType` does not know *where* the run was written, and that is on purpose:
which positions a type is legal in is a question about what the type means, so it
belongs to the stage that has types. The two positions that build an object ask
one shared question — `notAnObjectWord` — and get the word back:

| Position | Message |
|---|---|
| `let x: !` | `` `!` is not a type an object can have `` |
| `f(x: !)` | `` `!` is not a type a parameter can have `` |
| `*!`, `!i32`, `! !` | `` `!` is a type on its own: it cannot be combined with a type name or a `*` `` |
| `let x = die();` | `this expression never produces a value, so there is nothing to bind; write the type the value would have had` |

The last row is the one that costs the reader a decision, and the message makes
it: an unannotated `let` whose initializer is `!` would be an object of type `!`,
which no program ever reaches, and the nearly-always-intended fix is to *write*
the type the value would have had. With the annotation the same line is accepted,
because then the conversion has somewhere to land.

`void` and `!` are refused by the same helper so the two cannot drift apart, and
the sentence names the word because the reader typed one of the two.

## 5. Flow, and the conditional

`terminates` gains two cases, and both are one line:

```
ExprStmt      →  diverges(the expression)
Let/ConstStmt →  diverges(the initializer)
```

where `diverges(expr)` is `types_.isNever(typeOf(expr))` — the answer the checker
already computed and wrote into the artifact. A statement after either one is
unreachable, which is what `sema-unreachable-code` reports and what the optimiser
needs to be told.

`?:` gets the third case in its common-type rule. When one arm is `!` and the
other is not, **the surviving arm's type is the result**, and it is decided on
the spot (`decideAt`) rather than left deferred:

- `c ? 1 : die()` with no context is an `i32`, not `<integer literal>`, because a
  deferred literal is not a type `!` can convert into — and the coercion the
  lowering reads is recorded at the type the conditional ended up with.
- With a context (`let v: u8 = c ? 1 : die();`) the literal already took the
  context's type, and the rule is the same one.

This is `never`-fallback in the sense every language with this type has it: the
branch that cannot produce a value cannot be the branch that decides the type.

What is deliberately *not* extended is the arithmetic and comparison operators.
`x + die()` is `sema-invalid-operands`, not `!`, and that is a decision rather
than an omission: absorbing `!` through every operator would need a rule per
operator (what does `die() == die()` mean? is `die() && x` a `bool` or a `!`?), and
the languages that do absorb it lean on a trait system to answer. Here the honest
answer is that the expression is not arithmetic, and the fix — call it for its
effect, or return it — is one word. Rust refuses the same expression for the same
reason (`Add` is not implemented for `!`), so this is not a corner being cut.

## 6. The proof

A type is a claim about a *value*; a body is an implementation, and it can be
wrong. So `fn ! f()` is checked, in `checkFunction`, with two diagnostics that
differ because their fixes differ:

| Code | When | The sentence |
|---|---|---|
| `sema-never-returns` | a `return` the body can execute | `f` returns `!`, so control never comes back to its caller, and a `return` is control coming back |
| `sema-never-body-completes` | the body can reach its end | `f` returns `!`, so its body cannot reach its end — it has to loop forever, or call another function that never returns |

Both are *reachability* questions, not syntactic ones, and that is what separates
this from C's `_Noreturn` warning:

- `reachableReturn` walks the body and stops wherever `terminates` says control
  cannot continue, so `while true {} return;` is accepted — the `return` is never
  executed, and reporting it would refuse a program that keeps the promise.
- A `return` whose *operand* is `!` is not a return either: `return die();` never
  gets as far as handing a value back, and the same one-line rule decides it.

The `return` arm of `checkStatement` defers to this walk entirely for a `!`
function: the operand is typed as a value on its own and no mismatch is reported,
so `return 1;` costs exactly one sentence — the one about the statement the
reader wrote — instead of "and `i32` cannot be used as `!`" as well.

**The promise is written, never inferred.** `fn void die() { while true {} }` is
not `!` for its callers: nothing told the compiler it may assume so, and
inferring it would make a caller's type depend on a body it may not even have.
The optimiser sees the body and infers whatever it likes; the type system asks
for the word. C requires `_Noreturn` for the same reason, and Rust's `-> !` is
written too.

A declaration with no body has nothing to prove: `extern fn ! exit(code: i32);`
is taking the promise on the caller's word, which is exactly what an `extern`
prototype is for.

## 7. Where it lands in the compiler

| Stage | File | What |
|---|---|---|
| lexer | — | **nothing.** `!` was already a punctuator (`TokenKind::Bang`), which is §1's argument made concrete |
| parse | `src/parse/declaration.cc`, `src/parse/type_scan.h` | `Bang` joins the type run in `scanTypeRun`, `parseTypeAndName` and `parseType`. No new syntax kind, no new error code |
| syntax | — | the `Type` node holds a `!` token; `identifierText` of such a node is empty, which is what a `!` type is |
| sema (types) | `type.h`, `type_store.cc`, `typespec.cc` | `TypeKind::Never`, `kTypeNever` (id 20, appended so no existing id moves), spelling `!`, size/align 0, the run reader |
| sema (conversion) | `convert.cc` | the one arm of §3 |
| sema (positions) | `check_stmt.cc` | `notAnObjectWord`, the `let`/`const` and parameter refusals, the inferred-`!` message |
| sema (flow) | `check_stmt.cc` | `diverges`, the two `terminates` cases, `reachableReturn`, the proof |
| sema (conditional) | `check_expr.cc` | the third common-type arm of §5 |
| ir | `types.cc` | `!` maps to `void`; `lowerOperand` turns a `!` operand into a `poison` of the type the consumer asked for |
| ir | `declarations.cc` | `noreturn` derived from the return type, on the single `Function` per def |
| ir | `debug.cc` | `!` has no DWARF type, like `void` and the poison |

## 8. The IR, and why `poison` is honest

`!` is `void` on the ABI side: a function returning it returns nothing, so
`declare void @exit(i32)` is the declaration, and the fact that it never comes
back is an **attribute derived from the type**:

```
declare void @exit(i32) #0
attributes #0 = { noreturn }
```

That derivation is the payoff of §2: the fact is a property of the type, so a
declaration and a definition of one name (one `llvm::Function`) cannot disagree
about it, and there is no second place that has to be told.

The value that does not exist is `poison`. `lowerOperand` is the one place a
consumer pulls a value out of a child, and it is where the rule lives:

```
if the child's type is `!`:
    the coercion record says what type the consumer asked for
    → poison of that type
```

Two things make this the right answer rather than a workaround. First, the
incoming value of a `phi` and the operand of a `call` have to *be* of the type
the consumer declared, so something has to be named; `poison` is precisely "a
value of this type that this program can never reach". Second, it is never read:
the edge it travels was left by a call that does not return. LLVM is entitled to
assume `poison` is not observed, and it is not.

The check is on the child's **type** and not on the value being null, which is a
bug the first version of this code had and the tests caught: a call that never
returns still produces a real `llvm::CallInst` — it is `void`, not absent — and
handing one to a `phi` is a verifier abort rather than a wrong answer.

```
cond.end:
  %cond = phi i32 [ 1, %cond.then ], [ poison, %cond.else ]
```

## 9. What this deliberately does not do

- **`let x: ! = die();`** is refused. Rust allows it behind `never_type` and it is
  harmless in principle; here it would mean an object whose type has no LLVM
  shape, and permitting it would put a `!` value in the storage path for no gain
  a program can observe. Writing the type the value would have had costs one
  annotation and says more.
- **No `!` in arithmetic or comparison.** §5.
- **No inferred divergence.** §6.
- **No `unreachable` builtin.** The IR writes `unreachable` where control runs off
  a block that cannot continue; a source-level spelling for it is a builtin
  (`builtins.md`, family 3) and would be the *statement* form of the same fact,
  not a second type.
- **No `!` in a generic or error position.** There are no generics and no enums
  yet; when there are, `Result<T, !>` is the case Rust's doc is built around and
  the conversion rule above is already what it needs.
- **No `!` for a partial function.** A function that *might* not return is not
  `!`. The type says never, and a `?`-like or `Option`-like spelling for the
  other case is a different feature.

## 10. How the claims above are checked

| Claim | Test |
|---|---|
| `!` is a whole run; combining it is refused by name | `TypeSpecTest.TheBottomTypeIsAWholeRunOfItsOwn` |
| the store's spelling, size and kind | `TypeStoreTest.TheBuiltInsHaveTheConstantsIds`, `TypeStoreTest.SizesFollowTheTarget` |
| `!` converts into everything and nothing into it | `ConvertTest.TheBottomTypeConvertsIntoEverything` |
| the run splits like any other, and the position is not the parser's question | `ParserTest.ANeverReturnTypeIsATokenInTheRun`, `ParserTest.ThePositionOfANeverTypeIsNotTheParsersQuestion` |
| the false positive is gone | `NeverTest.ACallEndsTheStatementSoTheBodyCannotFallOffItsEnd` |
| what follows a `!` statement is unreachable | `NeverTest.WhatFollowsANeverStatementIsUnreachable` |
| the conditional takes the other arm's type | `NeverTest.AConditionalWithANeverArmTakesTheOtherArmsType`, `NeverTest.BothArmsOfANeverConditionalAreNever` |
| the two broken promises, and every way to keep one | `NeverTest.APromiseNeedsSomethingThatKeepsIt`, `NeverTest.EachWayOfKeepingThePromiseIsAccepted`, `NeverTest.AnUnreachableReturnIsNotAReachableOne` |
| one sentence per mistake | `NeverTest.ABadReturnIsOneSentenceAndNotTwo` |
| `void` and `!` in the return position | `NeverTest.AVoidFunctionMayReturnANeverOperand` |
| a value wherever a value is expected | `NeverTest.TheTypeCanBeAValueWhereverAValueIsExpected` |
| the written-word rule | `NeverTest.TheWordCannotBeWrittenOutsideAReturnType`, `NeverTest.ABindingInferredFromANeverCallAsksForAType` |
| `main` cannot be `!` | `NeverTest.MainCannotReturnNever` |
| the promise is never inferred | `NeverTest.ThePromiseIsWrittenAndNeverInferred` |
| every code is reachable | `ErrorsTest.EveryCodeIsReachableFromAnInputTheGrammarAccepts` |
| the attribute and the edge | `IrLowerTest.ANeverReturnTypeCutsTheEdgeAfterTheCall`, `IrLowerTest.OnlyANeverReturnTypeGetsTheNoreturnAttribute` |
| the poison incoming | `IrLowerTest.ANeverArmOfAConditionalContributesPoison` |
| the example runs | `examples/012_never.mx` (`make examples`) |

## References

- `std::primitive::never` — the coercion rule, and "on stable it can be used only
  as a function return type".
- RFC 1216 / the never-type initiative — why `!` coerces rather than subtypes.
- `_Noreturn` (C11 6.7.4) — the same promise without a type, and the syntactic
  check that §6 improves on.
- LLVM `noreturn` function attribute, and `poison` as "a value that may never be
  observed".
- `docs/architectures/sema.md` — the type system this lands in.
- `docs/architectures/ir.md` — the module contract, the coercion record, and the
  verifier rules the `poison` above has to satisfy.
- `docs/architectures/builtins.md` — `unreachable`, `trap` and the family `!` is
  not.
- `docs/architectures/extern.md` — declarations without bodies, which is what a
  `!` prototype usually is.
