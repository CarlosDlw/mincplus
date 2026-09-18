---
sidebar_position: 13
---

# Type constants

A **type constant** is a value a *type* has, written as the type, `::`, and the
name: `i32::MAX`, `f64::EPSILON`, `char::MAX`. Nothing is computed and nothing is
looked up at run time — the value belongs to the type, so `u8::MAX` is 255
wherever it is written, and the checker folds it into a constant like any
literal.

```minc
let limit = i32::MAX;             // 2147483647
let gap = f64::EPSILON;           // the gap above 1.0, at f64's width
type Meters = f64;
let zero = Meters::ZERO;          // an alias is transparent: this *is* f64::ZERO
```

They exist for the one case a program cannot write any other way: **a value inside
a generic body, where the type is not known until the instantiation.**

```minc
fn T nonNegative<T: Ordered>(x: T) {
  return x < T::ZERO ? T::ZERO : x;    // the zero of whatever `T` turns out to be
}

fn i32 main() {
  return nonNegative(-5) + nonNegative(-2.5) == 0.0 ? 1 : 2;   // exits 1
}
```

`T::ZERO` is `0` in the `i32` instance and `0.0` in the `f64` one, from one body
that is checked once. Without it, a generic body can compare two binders but
cannot name a constant, because `0` is a literal and a literal needs a type the
body does not have.

## The names

Seven words, and the set is small on purpose:

| Name | What it is | Who has it |
| --- | --- | --- |
| `ZERO` | the additive identity | every integer, every float, `char` |
| `ONE` | the multiplicative identity | every integer, every float, `char` |
| `MIN` | the **smallest value** of the type | every integer, every float, `char` |
| `MAX` | the **largest value** of the type | every integer, every float, `char` |
| `EPSILON` | the gap above `1` | a float |
| `INFINITY` | the value past every finite one | a float |
| `NAN` | not a number | a float |

- **`MIN` is the smallest value, not the smallest positive one.** `MIN` < `MAX`
  holds for every type, always. There is no name for "the smallest positive
  value": that is a different question, and one name answering two questions is
  how a reader comes to be wrong about which one they asked.
- **No `TRUE`/`FALSE` and no `NULL`.** `true`, `false` and `null` are already
  values of the language with their own spelling, and `bool` is not a class that
  could grant one.
- **The names are the ones a class promises**, not a list kept by hand — see
  below.

```minc
fn i32 main() {
  let ok = (i8::MIN == -128) && (i8::MAX == 127) && (u8::MAX == 255) &&
           (i32::MAX == 2147483647) && (u32::MAX == 4294967295) &&
           (i64::MIN < 0) && (i128::MAX > 0) && (u128::MAX > 0) && (isize::MAX > 0) &&
           (char::MIN == 0) && (char::MAX == 255) &&
           (f64::EPSILON > 0.0) && (f64::EPSILON < 1.0) &&
           (f64::MAX > 1.0e308) && (f64::MIN < -1.0e308) &&
           (f64::INFINITY > f64::MAX) && ((f64::NAN == f64::NAN) == false);
  return ok ? 1 : 2;                                    // exits 1
}
```

Every one of those values is exact for its type, including the two that a 64-bit
intermediate would lose: `i128::MAX` is `170141183460469231731687303715884105727`,
and the width of a type constant is the *type's* width, not the compiler's.

## What a constraint promises

A constant is granted by a [constraint class](/language/generics) under the same
rule that grants an operation:

> **A class grants every constant all of its members have.**

| Class | Grants | Why |
| --- | --- | --- |
| `Integer` | `ZERO`, `ONE`, `MIN`, `MAX` | every member is a bounded machine integer |
| `Float` | those four, plus `EPSILON`, `INFINITY`, `NAN` | every member is a float |
| `Number`, `Ordered` | those four | their members are the same arithmetic types |
| `Any` | nothing | `Any` admits products and stores too |
| `Eq` | nothing | its members are `bool`, `str` and pointers — **none of which has a zero** |
| `Pointer` | nothing | an address has no bounds a constant could name |

That is why a body under `<T: Float>` can write `T::EPSILON` and a body under
`<T: Integer>` cannot, and why `<T: Eq>` gets nothing.

```minc
fn T half<T: Float>(x: T) { return x * (T::ONE / (T::ONE + T::ONE)); }
fn T oneOf<T: Integer>(x: T) { return x < T::MAX ? T::ONE : T::ZERO; }
```

## Read from the type, resolved by nobody

`Name::Name` is a qualified name: the first word is read as a **type** — the same
reader every other type position uses, with the same names in scope, so a
primitive, an alias, a C spelling such as `int` and a generic binder are all just
types — and the second is a constant of it.

Nothing is resolved at that point, and no symbol table is consulted to decide
which of the two forms `a::b` is. One token tells them apart:

| Written | Read as | Why |
| --- | --- | --- |
| `f::<i32>(x)` | a call with an explicit type argument list | after `::` comes `<` |
| `i32::MAX` | a qualified name | after `::` comes a word |

There is no context in which one of them could be the other, so a program's
meaning never depends on which declarations exist.

## Refusals

```
$ mincc check p.mx
p.mx:3:14: error[sema-type-constant-unknown]: `BANANA` is not a constant of `i32`: its constants are `ZERO`, `ONE`, `MIN`, `MAX`
    let x = i32::BANANA;
             ^^^^^^
```

Everything a refusal needs is in the sentence, and both sentences name the whole
set the reader has to choose from:

| Situation | Code |
| --- | --- |
| the name is not one of the seven | `sema-type-constant-unknown` |
| the type has no such constant (`str::ZERO`, `bool::MAX`) | `sema-type-constant-not-granted` |
| the binder's class does not grant it (`T::ZERO` under `<T: Eq>`) | `sema-type-constant-not-granted` |
| the first word is not a type at all | `sema-unknown-type` |

A constant is a **value**, so it composes with everything a value does: it is an
operand, an argument, an index, a member of a tuple, a file-scope `const`
initializer, and the arm of a `?:`. It is not, and will not be, arithmetic the
preprocessor can see — `#if i32::MAX` is refused by `pp-expression-syntax`, since
`#if` runs before any type exists and the preprocessor's arithmetic is 64-bit
signed and untagged.

## Not implemented yet

- **Constants of your own** (`T::MY_CONST`). That is an associated item, and it
  arrives with declared interfaces — the same step that brings methods.
- **`T::SIZE` and `T::ALIGN`.** A fact about the *layout* rather than about the
  value, and it belongs with `sizeof`/`alignof`.
- **A constant where a count is written** (`[T::N]i32`). A count is a literal
  today ([Arrays](/language/arrays)); the spelling above is the one that will be
  used when a count can be symbolic.
- **A constant in a type position.** `let x: i32::MAX = 3;` is not a type, and the
  parser says so — though the recovery after it is noisier than the mistake
  deserves.
