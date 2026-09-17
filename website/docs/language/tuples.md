---
sidebar_position: 11
---

# Tuples

A tuple is a **product** of two or more types, written in parentheses: a value of
several parts at once, with the parts in a fixed order and no names. It is what a
function returns when one answer is not enough, and what a parameter takes when
one argument is not enough.

```minc
fn (i32, i32) divmod(a: i32, b: i32)
{
  return (a / b, a % b);
}
```

The names `divmod` and `a / b` are ordinary; the new thing is that the return
type is a *list* and the `return` produces one value of it.

## Writing the type, and writing a value

A product type is a parenthesised list of two or more types, and it nests:

```minc
let pair: (i32, bool) = (1, true);
let nested: (i32, (i32, bool)) = (1, (2, true));
let table: [2](i32, i32) = [(1, 2), (3, 4)];   // an array of pairs
let box: *(i32, bool) = &pair;                 // a pointer to one
```

A **value** is a parenthesised list of expressions, and its type is
`(T, U, ...)` — there is nothing else to say at a use, because a product carries
its members' types with it. The two spellings are the same parentheses the
language already had, and they are decided by what is inside them: `(a + b)` is a
parenthesised expression and `(a, b)` is a value. A one-element product does not
exist — `(x)` is `x` — so the comma is what makes the type, exactly as it does in
the type position.

There is no `struct` and no named fields here. A product is the *anonymous*
case: what a function returns, what a `let` takes apart, and nothing more. When
the language has `struct`, a named record is that feature's job; the two are not
spelled alike and neither is waiting on the other.

## Taking one apart

Two ways, and both are compile-time.

**By position.** `.` followed by a member's index — a number, not a name:

```minc
let pair: (i32, bool) = (1, true);

let n: i32 = pair.0;     // the first member
let b: bool = pair.1;    // the second
```

A chain of two reads is written with a space — `pair.1 .0`, or `(pair.1).0` —
because the scanner reads the longest number it can: `pair.1.0` is `pair.1.0`
with a *float* `1.0` in it, and the parser says so rather than guessing:

```console
$ mincc check main.mx
main.mx:2:39: error[parse-expected-name]: `1.0` is one number, so this is not two member reads: write the second member apart from the first, as `t.0 .1` or `(t.0).1`
```

**By destructuring.** A `let` or a `const` may name a product's members directly,
and the members become ordinary bindings:

```minc
let (q, r) = divmod(7, 2);      // `q` is 3, `r` is 1
let (_, rest) = divmod(9, 2);   // `_`: the first member is not wanted
const (half, _) = divmod(5, 2);
```

Three rules, and each is a diagnostic:

- **The arity must match.** Two names for three members is
  `sema-destructuring-arity` — there is no partial pattern and no gather.
- **`_` skips one member**, and it is the only thing that may be written twice:
  `let (_, _) = p;` is legal, and two `_` are not a redeclaration.
- **A pattern is not an expression.** `(a, b)` on the right of `=` is a value and
  on the left is a pattern; there is no third reading, so `let (a, b) = (a, b)`
  is the outer `a` and `b`, never itself.

## Passing and returning

A product is a value like any other scalar of its size: it is copied on
assignment, on return and on the way into a call, and a copy is a copy of every
member.

```minc
fn i32 first(p: (i32, bool))
{
  return p.0;
}

fn i32 main()
{
  let pair: (i32, bool) = (7, false);
  let same = pair;          // every member copied
  return first(same);       // 7
}
```

**A product is refused across the C boundary.** Its layout and the convention for
passing it are this compiler's own, so an `extern` declaration may not claim them:

```console
$ mincc check main.mx
main.mx:1:24: error[sema-extern-aggregate]: `extern` says this function is defined somewhere this compiler is not looking, and a product `(i32, i32)` is passed here as a copy the caller makes: pass a pointer instead, `*(i32, i32)`, and the address crosses
  extern fn i32 takes(p: (i32, i32));
                         ^^^^^^^^^^
```

A pointer to a product crosses freely, because an address is something every ABI
agrees about.

## Generics come after it

A binder list is itself a sequence of pairs — a name and a class — so the product
had to exist before `<T, K>` could be read as more than one word. For the same
reason a *type* alias may name a product and a function may return one:

```minc
type Pair<T, K> = (T, K);

fn Pair<T, K> swap<T, K>(left: T, right: K)
{
  return (right, left);
}
```

`Pair<i32, bool>` *is* `(i32, bool)`, with no second type behind the name, and the
generics page takes it from there — the binder rules, constraints, and the
instantiation of a body per argument list.

## What a product does not have

- **No equality and no ordering.** `a == b` on two products is refused, and the
  sentence names the members to compare — `a.0 == b.0 && a.1 == b.1`. Equality
  would have to mean "every member is equal", which is a rule the language gives
  per type through a declared interface rather than a built-in meaning for `==`.
- **No literal type, no `len`, no printing.** A product has no name to print and
  no member count an expression can read; the members are compile-time.
- **No `struct`-style member names**, and no first-class product *type* value:
  `(i32, i32)` names a type where a type belongs, and the checker reads it there.
- **No member read through a pointer, implicitly.** `p.0` where `p` is a
  `*(i32, bool)` is refused — a pointer is not a product — and the two ways to
  say it are `(*p).0` and `p[0].0`. There is no auto-dereference, because `.`
  reaching through a pointer would be a rule about pointers hidden in an operator
  about members.

## In the emitted code

Every member has its own type, and one value of the product is one object whose
members lie in **C's field order**. A reordering is not permitted for packing,
because the layout is also what `-g` states:

```console
$ mincc build -g -o prog main.mx
$ gdb -batch -ex 'break main.mx:7' -ex run -ex 'ptype p' ./prog
type = struct {
    i32 __0;
    bool __1;
}
$1 = {__0 = 7, __1 = true}
```

The record is anonymous — the language has no name to give it — and its members
are the positions, in order, with no padding the source did not ask for. A
debugger therefore reads a product the same way it reads any other record, and
`p p` prints the members rather than one word.

The design record is
[`docs/architectures/tuples.md`](https://github.com/CarlosDlw/mincplus/blob/main/docs/architectures/tuples.md).
