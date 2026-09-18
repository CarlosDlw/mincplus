# Type constants — `T::ZERO`, `i32::MAX`

A **type constant** is a value a *type* has, named by writing the type, `::`, and
the constant: `i32::MAX`, `f64::EPSILON`, `char::MAX`. It is one value, known at
compile time, and the type it belongs to is what decides it — so the same text
means `0` for an `i32` instance of a generic body and `0.0` for an `f64` one.

```minc
let m = i32::MAX;                 // 2147483647, folded like a literal
let e = f64::EPSILON;             // the gap above 1.0, at f64's width
type Meters = f64;
let z = Meters::ZERO;             // the alias is transparent: it *is* f64::ZERO

fn T nonNegative<T: Ordered>(x: T) {
  return x < T::ZERO ? T::ZERO : x;   // a value the body cannot name any other way
}
```

The record below is the whole of the feature: **why a value belongs to a type**
(§ 1), **what the vocabulary is** (§ 2), **which class promises which constant**
(§ 3), **how the parser reads it without a symbol table** (§ 4), **where the value
comes from** (§ 5), and **what the change turned up in code that already existed**
(§ 6 — the one part of this that is a bug fix and not a feature).

---

## 1. Why a constant belongs to a type

The alternative spellings were all considered and each fails for a reason that is
about this language rather than about taste:

| spelling | why not |
|---|---|
| `MAX` as a global name in each scope | a name and not a value: `MAX` beside `MAX` of another type is a redeclaration, and a generic body cannot name the one *its* type has |
| `MAX(i32)` — a builtin | it is a *type* being applied to an argument, which is what a cast already is (`i32)MAX` is nonsense, `(i32)MAX` is a cast of a name that does not exist), and a builtin table keyed by type is a second type system |
| `#define MAX 2147483647` | the preprocessor has no types: `f64::MAX` and `i32::MAX` are different numbers, and `#if` arithmetic is 64-bit signed (`support/consteval`) |
| a name per type (`I32_MAX`, `F64_EPSILON`, …) | three names per type, and a body under `T: Float` still cannot name the one its instance has |
| `T::ZERO` — **this** | the value is a fact *about the type*, the type is already in scope wherever the constant is needed, and a binder's value is decided at the instantiation, which is the only place it *can* be decided (`generics.md`, § 6) |

The last row is the decision, and the first four are why the decision is not a
convenience: every one of them either loses the type or needs a second table of
"which name belongs to which type".

## 2. The vocabulary, and why it is seven words

`support/constraint/constraint.h` holds the set, in one table with one spelling
each:

| name | what it is | who has it |
|---|---|---|
| `ZERO` | the additive identity | every integer, every float, `char` |
| `ONE` | the multiplicative identity | every integer, every float, `char` |
| `MIN` | the **smallest value** of the type | every integer, every float, `char` |
| `MAX` | the **largest value** of the type | every integer, every float, `char` |
| `EPSILON` | the gap above `1` | a float |
| `INFINITY` | the value past every finite one | a float |
| `NAN` | not a number | a float |

Three rules about the names, and each one is a decision:

- **`MIN` is the smallest value, not the smallest positive one.** Rust's split
  (`f64::MIN` = most negative, `f64::MIN_POSITIVE` = smallest positive,
  `f64::LOWEST` = …) is the cautionary case: one name, two questions, and a
  reader who has to remember which. Here `MIN` < `MAX` always, for every type, and
  "the smallest positive" is a question the language does not answer with a name
  (it is `MAX`'s neighbour, and nothing in an algorithm needs it as a constant).
- **`ZERO` and `ONE` are shared by the integer and the float classes** because
  they are the same two *facts* about both: `0` and `1` are the identities of
  every number this language has. `MIN`/`MAX` are shared on the same grounds.
- **No `TRUE`/`FALSE` and no `NULL`.** `bool` is not a class (a class with one
  member is the type — `support/constraint`), and `true`, `false` and `null` are
  already values of the language with their own spelling (`tuples.md`).

## 3. What a class promises

A constant is granted by a class under the **same sentence as an operation**, and
it is the sentence that makes the grant table a table rather than a list somebody
maintained:

> **A class grants every constant all of its members have.**

So the rule is mechanical, and the two groups fall out of it:

| class | grants | why |
|---|---|---|
| `Integer` | `ZERO`, `ONE`, `MIN`, `MAX` | every member is a bounded machine integer |
| `Float` | the four, plus `EPSILON`, `INFINITY`, `NAN` | every member is a float |
| `Number`, `Ordered` | the four | their members are the same arithmetic types (`support/constraint`), so they have exactly what those types have |
| `Any` | nothing | `Any` admits everything, products and stores included |
| `Eq` | nothing | `bool`, `str` and a pointer are its members and **none of them has a zero** — this is the row that would be a lie if the table were hand-written from habit |
| `Pointer` | nothing | an address has no bounds a constant could name |

The invariant is checked by the compiler's own tests, exactly as the operation
grants are (`constraint_test.cc::EveryClassGrantsEveryOperationItsMembersAdmit`):
the generated program uses **each granted constant** in the body of a function
whose binder is the class, instantiates it **once per member**, and requires zero
errors. A grant that no member can answer fails there, and so does a member the
lowering cannot build a value for — which is how `char` and `f80` prove they are
covered.

## 4. Reading it: two words and one token of lookahead

`Name::Name` is a **`QualifiedExpr`**: a new syntax kind holding the two
`Identifier` tokens. It is not a `PathExpr`, and the difference is the whole of
§ 4 — a `PathExpr` carries one interned name and *resolution* answers it, while
this node names a **type** first and resolves nothing at that stage.

The grammar already had `::`, for the turbofish of an explicit instantiation
(`f::<i32>(x)`, read as a postfix). One token tells them apart:

| written | read as | why |
|---|---|---|
| `f::<i32>(x)` | a call with a type argument list | after `::` comes `<` |
| `i32::MAX` | a qualified name | after `::` comes a word |

Nothing else is needed — no symbol table, no declaration order, and no spelling
that changes meaning when a name is added. That is deliberate and it is the same
property `casts.md` decision 4 relies on for `(T)x`.

## 5. Where the value comes from

Two stages and one rule each, and the rule is the vocabulary's rather than the
stage's:

- **`sema`** reads the first word with `readTypeSpec` — the *same* reader every
  type position uses, with the same names in scope — so a reserved type word, a
  name the unit gave a type, and a **binder** are all just types. It then asks
  `TypeStore::hasConstant` (a concrete type) or
  `support::constraintGrantsConstant` (a binder's class).
- **`src/ir`** builds the value at the type's own width: `APInt` from
  `support::integerConstantValue` for an integer, and `APFloat` for a float —
  `getLargest`, `getInf`, `getNaN`, and for `EPSILON` one step toward `+inf` and
  back, which is exact in the type's own semantics.

Three consequences, and each is a decision:

1. **The integer rule lives in `support`, not in either stage**, so the checker's
   fold and the lowering's constant cannot disagree: `integerConstantValue(bits,
   isSigned, constant)` is asked by both. It answers for a width up to 128, which
   is what makes `i128::MAX` the exact value — a `ConstInt` (64 bits, `#if`'s own
   width) could not hold it.
2. **The float rule lives in `ir` alone, and never goes through a `double`.**
   `ConstantFP::get(Type*, double)` rounds, and an `f80` exists precisely so that
   a value has a width a `double` does not. So `f64::MAX` and `f80::EPSILON` are
   built from the semantics, and the checker deliberately folds **no** float
   constant — the same rule a float literal already follows (`sema.md`).
3. **A constant folds like a literal where the value fits the core.** `i32::MAX`
   is `ConstInt`-foldable, so it is a constant expression everywhere a literal is
   (`i64::MAX % 1000` folds to `807`, and `const M: i32 = i32::MAX;` is a
   file-scope table's element). A value wider than 64 bits is left *unfolded*
   rather than truncated: `hasIntValue` false is the same answer the language
   already gives a literal too large to fold, and the lowering still emits the
   exact one.

## 6. What the change turned up — the comparison hole

Writing the value tests found a **crash**, and it was not in this feature:

```
$ mincc run crash.mx
mincc: llvm/IR/Instructions.h:1182: void llvm::ICmpInst::AssertOK(): Assertion
  `getOperand(0)->getType() == getOperand(1)->getType()` failed.
```

`1 == 0.0` (and `1 < 0.0`, and the mirror `1.0 == 0`) reached the backend as an
`icmp` between an `i32` and an `f64`. The checker accepted it and said nothing.

The root cause was a **missing check, not a missing rule**: the comparison branch
of `checkBinary` asked `usualArithmetic` for the operands' common type and used
its answer without asking whether it *had* one. For a pair like `u8 == i32` the
answer is the promoted type and everything is fine; for two numbers of different
classes — and `isArithmetic` is true of a *literal* whatever its class — the
answer is an error type, and the node was typed `bool` over two operands that
nothing converted. The IR then compared two different types, which LLVM treats as
a compiler bug and reports by *asserting*.

The fix is at the root: both comparison branches (`==`/`!=`, and the ordering
four) now refuse an operand pair with no common type, with the sentence the
arithmetic path already gives for the same pair — *"an integer and a float are
different classes of number and do not convert into each other"*. The example
that found it was left as the example **and** the mistake in it was corrected, in
that order: fixing the example would have hidden a crash that any reader of
`x == 0.0` with an integer `x` would hit.

The general rule this comes from is the language's own:

> **If the checker lets it pass, it must run.** A `bool` over two operands with no
> common type is not a program the compiler understood; it is a decision this
> stage did not make, and every stage below inherits the hole.

## 7. What is deliberately not here

- **No user-defined constants** (`T::MY_CONST`). That is an associated item, and
  it arrives with declared interfaces — the same stage that brings methods and
  associated types (`generics.md`, § 6).
- **No `sizeof`-style constants** (`T::SIZE`). A constant of the *language* rather
  than of the type, and it belongs with `sizeof`/`alignof`, which are their own
  builtins and their own record.
- **No constants in a type position** (`[T::N]i32`). That is a *value* used as a
  count — const generics — and it needs the count to be able to be symbolic
  (`arrays.md` keeps the count a literal today). The spelling is already the one
  that will be used for it, which is part of why it was chosen.
- **No `bool` constants.** See § 2: `true` and `false` are already values.

## 8. Where it lives

| piece | file | what |
|---|---|---|
| the vocabulary | `support/constraint/constraint.h/.cc` | `TypeConstant`, the name table, `constraintGrantsConstant`, `constraintForConstant`, `integerConstantValue` |
| the shape | `parse/syntax_kind.h`, `parse/expression.cc` | `QualifiedExpr`, read in `parsePrimary` with one token of lookahead |
| the question | `sema/type_store.h/.cc` | `hasConstant` — the members predicate, beside the five class predicates |
| the read | `sema/check_expr.cc` | `checkQualified`: the type, the constant, the class (for a binder), the fold |
| the sentences | `sema/sema_error.h/.cc` | `sema-type-constant-unknown`, `sema-type-constant-not-granted` — each lists the set the type or the class *does* have |
| the value | `ir/expr.cc` | `lowerTypeConstant`: `APInt` at the type's width, `APFloat` in the type's semantics |
| the comparison fix | `sema/check_expr.cc` | § 6, in both comparison branches |

Tests: `constraint_test.cc` (the grants against the member sets, per instantiation),
`errors_test.cc` (both codes reachable), `generics_test.cc` (the two uses of `::`),
`build_command_test.cc` (every constant **run**, and a generic body instantiated at
two types), `examples/024_type_constants.mx` (the whole vocabulary, and the file is
written so that running it is the assertion: it returns `1` when every value holds).
