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

## File scope

A `let` or a `const` may also be written at the **top of a file**, beside a
function, with the same shape it has in a block:

```minc
const maxUsers: i32 = 4096;      // this constant is in the file's image
let   requests: u64 = 0;         // a mutable object, for the whole run
```

They are *definitions*: the object is in the file's image, and it exists for the
whole run of the program. There is no declaration form for a binding —
`extern let x: i32;` names no object this compiler can put in the image, and it
is refused by name rather than left to mean something subtle. `extern fn` is the
declaration form, and it is about functions.

### The initializer must be a constant

A file-scope initializer has to be **computable before the program exists** —
a literal, arithmetic over constants, or a reference to another file-scope
constant:

```minc
const mask:  u32 = (1 << 8) - 1;   // 255, folded at compile time
const scale: f64 = 2.5;
const limit: i32 = maxUsers / 2;   // names another constant, above or below
let   count: u64 = 0;
let   root:  *i32 = null;
```

A float initializer is a **literal**, with a sign and nothing else. `const half:
f64 = 1.0 / 2.0;` is refused, and this is the one place the rule is stricter
than the compiler could be: a file-scope object's *bytes* are written here, and
float arithmetic is rounded by the machine the program is about to run on. The
spelling is the one place a value is allowed to come from, so the value in the
image is the value the reader wrote, on every target.

A value that has to be *computed* is not one:

```minc
// error: this is work, and file scope is not a time at which work happens
let table: *T = alloc(1024);
```

This is a deliberate limit, and it buys the guarantee that **nothing runs before
`main`**. There is no startup order to get wrong, no constructor, and no static
initialization order fiasco — not because the language avoids it, but because
there is no dynamic initialization for an order to exist over. A global that
needs the environment, or an allocation, is initialized at the top of `main`.

Referencing a constant defined further down is fine — file-scope names are
visible independently of order — and the values are computed in dependency order,
so a cycle is an error rather than a surprise:

```minc
const a = b + 1;   // 42: `b` is below, and that is allowed
const b = 41;

// const loop = loop + 1;   // error: a cycle between constants
```

A `let` with no initializer is **zero-initialized** (that is the C ABI's `.bss`,
and it is what makes an `extern` global's value unambiguous); a `const` with no
initializer is a constant with no value, which is an error.

### Linkage, and who may see a name

A file-scope binding has **external** linkage whether it is a `let` or a `const`,
because it is a member of the unit's namespace — a constant is a value a module
*exports*, exactly as a function is, and minc+ will have a module system, so
"what other files can import" is that system's question and not a linkage
default's. Visibility and linkage are two answers to two different questions.

`static` is the word that makes a binding **internal to this unit**:

```minc
static const scratchSize: usize = 4096;  // no symbol for the linker to see
```

That is also the answer to a shared `.mx` file included by two units: two
external definitions of one name is a link-time error, and `static` is how a
name says it is one unit's business only.

`static` is the same word in front of a function, and it means the same thing
there: `static fn i32 helper()` is a symbol the linker will not resolve another
unit's reference to.

`const` protects the **name**, not the memory: a `const p: *T` cannot be pointed
somewhere else, and `*p = 1` still writes through it. Do not read a `const`
global as read-only bytes — the compiler is not allowed to infer that.

:::note[Not implemented yet]
A binding's initializer may be a literal, arithmetic over constants, a read of
another file-scope `const`, `null`, or `?:` over those. What is still missing is
whatever needs another feature first: aggregations (`struct`, arrays) to build,
the address of an object (`&x`) whose value the linker decides, thread-local
storage, and `extern` on a binding. File-scope bindings themselves, and `static`,
are implemented and recorded in
[`docs/architectures/globals.md`](https://github.com/carlosdlw/mincplus/blob/main/docs/architectures/globals.md).
:::
