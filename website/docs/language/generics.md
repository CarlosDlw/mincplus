---
sidebar_position: 11
---

# Generics and constraints

A **binder** is a type the declaration does not name, written in `<...>` after the
name being declared. One declaration with binders is a **template**: checked once,
abstractly. Each distinct argument list it is used with is an **instance**: a real
function, with a real symbol.

```minc
fn T identity<T>(value: T) {
  return value;
}

type Pair<T, K> = (T, K);

fn i32 main() {
  let a: i32 = identity(1);          // instance `identity<i32>`
  let b: f64 = identity::<f64>(1.0); // instance `identity<f64>`, written out
  let p: Pair<i32, bool> = (1, true); // `Pair<i32, bool>` *is* `(i32, bool)`
  return 0;
}
```

Two spellings, one list. A **type position** writes `<...>`; a **call** writes
`::<...>`:

```minc
fn i32 take(x: Vec<i32>) { return 0; }   // a type position
let v = identity::<i32>(1);              // a call site
```

`f < T > (x)` after an expression is a *comparison* — that is why the call form
has the colons, and why Rust spells it the same way. The reader also splits a
`>>` or `>>=` that closes two lists, so `Pair<Pair<i32, i32>, i32>` needs no
spaces.

## One body, many functions

The body of a template is checked **once**, against the binder — not once per
type argument:

```minc
fn T pick<T>(first: T, second: T, choose: bool) {
  return choose ? first : second;
}
```

Everything that follows comes from that decision. Because the body is checked
once, the compiler must know *which operations it may perform on the hole* before
any argument exists — and that is what a constraint is.

## Constraints: `T: Class`

A constraint says what may be done with the hole. The classes are the language's
own, and a class is two facts:

* its **members** — which type arguments may be used;
* its **grants** — which operations the body may perform.

| class | members | grants |
|---|---|---|
| `Any` | every object | nothing beyond storing, copying, passing and returning |
| `Eq` | arithmetic, `bool`, `str`, pointer | `==` `!=` |
| `Ordered` | arithmetic | `Eq` and `<` `<=` `>` `>=` |
| `Number` | arithmetic | `Ordered` and `+` `-` `*` `/`, unary `-`, `++` `--` |
| `Integer` | the integers and `char` | `Number` and `%` `&` `\|` `^` `~` `<<` `>>` |
| `Float` | `f32`, `f64`, `f80` | `Number`, over fewer types |
| `Pointer` | `*T` | the comparisons, and nothing else |

```minc
fn T twice<T: Number>(x: T) {
  return x + x;
}

fn bool less<T: Ordered>(a: T, b: T) {
  return a < b;
}
```

The two facts are **independent on purpose**. `Ordered` and `Number` admit
exactly the same types and promise different operations, so `fn T max<T:
Ordered>` says "all I do with it is compare" — a smaller promise than `Number`
and therefore a better one. What the table must satisfy is that *every grant is
legal for every member*, so a class can never promise an operation some member of
it would be refused.

`Any` is what a binder with no written constraint gets, and it is a class rather
than the absence of one: storing, copying, passing and returning still work.

```minc
fn T identity<T>(value: T)    // `<T>` and `<T: Any>` are the same declaration
{
  return value;
}
```

### What an unconstrained binder cannot do

```minc
fn T twice<T>(x: T) {
  return x + x;
}
```

```
error[sema-generic-operation]: `+` on a binder needs a constraint: `T` is a type
parameter, and what may be done with one is what its class allows. Constrain it, as
in `fn T f<T: Number>(...)`, and `Number` is the weakest class that admits `+`
```

The class named is always the **least powerful** one that grants the operation: a
body that only compares is told `Ordered`, because writing `Number` there would
promise arithmetic the body never uses.

```minc
fn T rest<T: Number>(a: T, b: T) { return a % b; }
// `%` on a binder: `T` is constrained to `Number`, which does not allow it.
// Widen the class to `Integer`
```

### `bool` has no class

`!`, `&&`, `||` and the condition of an `if`/`while`/`for` are `bool`-only. No
class grants them *on purpose* — a class whose members are one type is not a
constraint, it is the type — so a binder in one of those positions is told to
write `bool`:

```minc
fn i32 odd<T>(x: T) { if x { return 1; } return 0; }
// `if` on a binder: `T` is a type parameter and no class allows this, because it is
// defined for `bool` and nothing else. A value that must be a `bool` is written
// `bool`, not a binder
```

### A constraint on a generic `type`

An alias's target is a type expression, so it performs no operation — what the
class on it promises is which types may fill the hole, and a use is where a hole
gets filled:

```minc
type Vec<T: Number> = [4]T;

fn i32 sum(v: Vec<i32>) { return v[0]; }   // fine

sum::<i32>(0);
let bad: Vec<bool> = [4]bool{true, false, true, false};
// `bool` cannot be the `T` of `Vec`: the declaration says that binder is `Number`,
// and a type argument has to be one of the types that class admits
```

### Type arguments are checked where the instance is made

An argument outside the class is refused at the call, and no function is emitted
for it:

```minc
fn T add<T: Number>(a: T, b: T) { return a + b; }
let x = add::<bool>(true, true);
// `bool` does not satisfy the constraint on `T`: this declaration says that binder
// is `Number`, and a type argument has to be one of the types that class admits --
// otherwise the body, which is checked once against the class, would mean something
// the instance cannot do
```

### Literals, and the class that decides them

`1` is an integer literal and `1.0` a float one, and this language does not
convert one into the other by itself: `let x: f64 = 1;` is an error, and you
write `1.0`. In a binder's position the literal therefore has to be decided by the
**class** — and only a class whose members are *all* integers can take `1`:

```minc
fn T zero<T: Integer>() { return 1; }     // one body, the right `1` in every instance
fn T one<T: Float>()   { return 1.0; }

fn T halve<T: Float>(x: T) { return x / 2.0; }   // a literal beside a binder
```

```minc
fn T zero<T: Number>() { return 1; }
// this integer literal cannot be stored in `T`: `T` is constrained to `Number`, and
// that class does not say which kind of number a literal is: `1` would mean `1i32`
// for one instantiation and `1.0` for another, and the body is checked **once**
```

When the class admits it, the literal **is the binder** — not its own default —
which is what makes one body give the right value per instance: `zero::<u8>()` is
a `u8` `1`.

A `Number` body therefore cannot write a constant at all. That seam is named:
what it needs is the *type's own* constant (`T::ZERO`), which arrives with
user-declared constraints.

## What is not here yet

* **User-declared constraints** (`interface`, a concept). The syntax has one slot
  for one, and the classes above are the compiler's own.
* **Composing constraints** — `<T: A + B>`. Every pair of the seven classes is
  either redundant or contradictory, so nothing needs it today; a user-declared
  class is what will.
* **A constraint over the pointee** — `<T: Pointer>` grants `p < q` and not `*p`
  or `p[i]`, because those are the pointee's type and an abstract pointer does not
  name one. Related types arrive with `interface`.
* **Callable binders** — `fn i32 apply<F>(f: F, x: i32) { return f(x); }` needs
  both a constraint that says "callable with this signature" and a way to write a
  function type in a type position.

## Where the instances go

Each instance is one function with the symbol `__M8_identityi32`, emitted once
however many calls reach it, and one `DW_TAG_subprogram` whose `DW_AT_name` is
`identity<i32>` — so `break identity` in a debugger stops in every instance.

The set of instances is found by a worklist keyed on `(declaration, arguments)`,
seeded by the concrete calls and expanded per instance of an enclosing template:
a call to a template *inside* a template is written in terms of the enclosing
binders, so no single walk of the program enumerates them. That is also what makes
a recursive generic terminate — a repeated pair is never expanded twice.

`mincc check --ast` prints the instance table, which is the one fact the syntax
tree cannot show: every call says `identity`, and it is the *set* of functions
that changes.

## The design record

[`docs/architectures/generics.md`](https://github.com/carlosdlw/mincplus/blob/main/docs/architectures/generics.md)
— the market evidence, the layout rule, the refusals, and each decision with what
it was measured against.
