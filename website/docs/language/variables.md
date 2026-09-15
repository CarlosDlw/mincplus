---
sidebar_position: 3
---

# Variables

A *binding* gives a value a name. Two forms, one shape:

```minc
let count: i32 = 0;      // mutable, annotated
let inferred = 10;       // mutable, the type is the initializer's
const limit: i32 = 100;  // not assignable
const alsoInferred = 42;
```

`let` is mutable. `const` is not. Both take an optional `: type` annotation and
an optional initializer, and there is no `mut`, no `var`, and no second syntax
for a constant — the difference between them is one word.

## Type inference

With no annotation, the type is the initializer's:

| Initializer | Type of the binding |
| --- | --- |
| `10` | `i32` — an integer literal defaults to `i32` |
| `10.0` | `f64` |
| `'x'` | `char` |
| `"text"` | `str` |
| `true` | `bool` |
| a call, a name, an expression | whatever that expression's type is |

```minc
let count = 10;          // i32
let half = 10.0;         // f64
let letter = 'x';        // char
let text = "hello";      // str
let flag = count > 0;    // bool
```

An annotation turns the literal into the annotated type, so `let small: u8 = 10`
is a `u8` and `let wide: i64 = 10` is an `i64`. A constant that does not fit is
an error (`sema-literal-out-of-range`), not a truncation.

Inference is *local*: it never makes a function's type depend on its callers, and
a function's return type is always written.

## Without an initializer

A binding may be declared and assigned later:

```minc
fn i32 pick(chooseFirst: bool, a: i32, b: i32)
{
  let result: i32;        // no value yet

  if chooseFirst
  {
    result = a;
  }
  else
  {
    result = b;
  }

  return result;          // every path assigned it, so this is fine
}
```

Reading it before an assignment has reached the read on **every** path is an
error:

```console
$ mincc check main.mx
main.mx:4:10: error[sema-use-before-assignment]: `result` is read before it is assigned: not every path from its declaration gives it a value
    return result;
           ^^^^^^
```

This is the rule Java, C# and Swift have, and it is an error rather than C's
warning-you-might-not-get: a program that reads an unassigned value has a
mistake in it, and the compiler can see it.

The rule is exact rather than conservative where the language allows:

```minc
fn i32 loop()
{
  let seen: i32;

  while true
  {
    seen = 1;   // assigned on the only path that leaves
    break;
  }

  return seen;  // fine
}
```

`examples/008_definite_assignment.mx` in the repository is the accepted side of
the rule, case by case.

A `const` with no initializer is a constant with no value, which is not a thing
worth having; give it one.

## Assignment

The left side of an assignment has to be a **modifiable place**: a `let`
binding, a dereference of a pointer, an indexed place, or a parenthesised
expression that is one. A `const`, a function name, a literal and `true` are
none of those, and the message says which one was written.

```minc
let value: i32 = 1;
value = 2;          // fine
value += 3;         // compound assignment, every arithmetic and bitwise operator
value++;

const limit: i32 = 10;
// limit = 11;      // error: `limit` is a `const`
```

The compound forms are `+=` `-=` `*=` `/=` `%=` `&=` `|=` `^=` `<<=` `>>=`, and
`++`/`--` exist as both prefix and postfix. All of them assign, so their left
side follows the same rule.

Assignment is an expression and its value is the value stored, so `a = b = 0`
works and `let x = (value += 1);` is a value.

## Scope

A binding lives from its declaration to the end of the block that contains it,
and a function's body *is* a block:

```minc
fn i32 scopes()
{
  let outer: i32 = 1;

  {
    let inner: i32 = 2;
    outer = inner;      // the inner block sees the outer binding
  }

  // inner is gone here
  return outer;
}
```

A declaration may shadow an outer one, and `-Wshadow` reports it, naming both
places:

```console
$ mincc check -Wshadow main.mx
main.mx:4:9: warning[resolve-shadowed-name]: declaration of 'x' shadows an earlier declaration
      let x: i32 = 2;
          ^
```

Two declarations of one name in one scope is a redeclaration, and an error.

## The bottom line on `const`

`const` is about the *binding*, not about the bytes: a `const p: *i32` is a
pointer that cannot be pointed somewhere else, and `*p = 1` still writes through
it. There is no `const`-correctness in the type today — no `*const T` — so
"this pointer's pointee will not be written" is not a promise the language can
make yet.

:::note[Not implemented yet]
File-scope bindings do not exist: every item in a unit is a function
declaration. Global constants, `static`, and thread-local storage arrive with
linkage control.
:::
