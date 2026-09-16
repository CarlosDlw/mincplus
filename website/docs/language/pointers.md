---
sidebar_position: 7
---

# Pointers

A `*T` is an **address**, not an owner. The language has no borrow checker and no
lifetime in the type, so the obligation is the programmer's: the pointer must name
an object that is still alive, and the access must stay inside it. What the
compiler does promise is that nothing is undefined *by accident* — there is no
aliasing rule to trip over, no `char*` special case, and no integer that is
secretly an address.

The whole surface is four shapes, and each has one meaning:

| Written | Means |
| --- | --- |
| `*T` | a pointer to a `T` |
| `&x` | the address of a modifiable place |
| `*p` | the place `p` names |
| `p[i]` | the i-th `T` from `p` |

```minc
fn void bump(p: *i32)
{
  *p = *p + 1;
}

fn i32 main()
{
  let value: i32 = 41;

  let p: *i32 = &value;   // the address of the place `value`
  *p = *p + 1;            // a store through it writes `value`

  bump(p);                // the callee gets a copy of the address

  let first: i32 = p[0];  // `p[i]` is `*(p + i)` — the definition, not a second
                          // operation, so the index scales by the pointee's size

  return first + value;
}
```

A pointer *parameter* receives a copy of the address, so the pointee is shared
and the pointer itself is not: assigning to `p` inside `bump` would not change the
caller's `p`, while `*p = …` does change the caller's `value`.

## Taking an address

`&x` needs a place: a `let` binding, a dereference of a pointer, an index. A
literal, a `const`, a function name and `true` are not places, and each is refused
with the word that was written.

```minc
let value: i32 = 1;
let p: *i32 = &value;         // fine

const limit: i32 = 10;
// let q: *i32 = &limit;      // error: a `const` has no modifiable place
```

`&value` is a `*i32`; `&p` is a `**i32`. There is no `*const T` yet, so `const`
applies to the binding and not to the pointee: a `const p: *i32` is a pointer that
cannot be pointed elsewhere, and `*p = 1` still writes through it.

## Dereferencing

```minc
let value: i32 = 1;
let p: *i32 = &value;

*p = 2;              // a store
let read: i32 = *p;  // a load
(*p) += 1;           // the place, then a compound assignment
```

`*p` is a place, so it can be assigned, read, or passed on. `void` has no size and
`!` has no values, so neither can be a pointee.

## `*void`, the untyped pointer

```minc
fn bool isEmpty(p: *void)
{
  return p == null;
}

fn i32 main()
{
  let value: i32 = 1;
  let typed: *i32 = &value;

  let erased: *void = typed;     // implicit, and the only crossing point
  let again: *i32 = erased;      // implicit, back the other way

  return again == typed ? 0 : 1;
}
```

`*void` converts to and from every pointer type, and that is the *only* implicit
pointer conversion. It is also the only pointer that cannot be dereferenced or
stepped — `void` has no size, so there is no load and no stride:

```console
$ printf 'fn i32 f(p: *void) { return *p; }\n' | mincc check -
<stdin>:1:29: error[sema-pointer-void-access]: `*void` cannot be dereferenced: `void` has no size, so there is nothing to access
  fn i32 f(p: *void) { return *p; }
                              ^^
```

## `null`

`null` is the empty pointer: a `*void` that converts to any pointer type.

```minc
let empty: *i32 = null;
let alsoEmpty: *void = null;

if p == null
{
  return 0;
}
```

There is no integer zero that means an address. `p == 0` is refused, and the
refusal explains that a pointer is not an integer — see below.

## Arithmetic and indexing

```minc
let p: *i32 = &value;

let moved: *i32 = p + 1;   // one `i32` further along
moved -= 1;                // and back
let back: bool = moved == p;

let zero: i32 = p[0];      // the same as `*p`
let two: i32 = p[2];       // `*(p + 2)`
```

`p + n` and `p - n` step by whole pointees, and `p[i]` is defined as `*(p + i)`.
The offset is converted to the pointer's index width (`isize`/`usize`) and that
conversion is recorded, so `p + n` and `p[n]` are guaranteed to compute the same
address. The pointer keeps its type: `p + 1` is a `*i32`, not an integer.

Two pointers to the same object compare equal; comparison reads the address.

:::warning[Stepping outside the object]
`p + n` itself is defined arithmetic — the model describes it as offset
computation. What is *not* defined is the **access** through a pointer that has
left its object: dereferencing there violates the access rule. A checked build
reports it at the site; a release build is outside the model. See
[the memory model](/language/memory-model).
:::

## A pointer is not an integer

```console
$ printf 'fn i32 f(p: *i32) { return p == 0; }\n' | mincc check -
<stdin>:1:28: error[sema-pointer-mismatch]: `*i32` and `<integer literal>` cannot be compared: a pointer is compared with a pointer (use `null` for the empty one)
  fn i32 f(p: *i32) { return p == 0; }
                             ^^^^^^
```

and in an assignment or an initializer the conversion itself is refused, with the
model's own words:

```console
$ printf 'fn i32 f(n: i32) { let p: *i32 = n; return 0; }\n' | mincc check -
<stdin>:1:34: error[sema-pointer-integer]: `i32` does not convert to `*i32` in this initializer: a pointer is not an integer, and the language has no implicit conversion between the two
  fn i32 f(n: i32) { let p: *i32 = n; return 0; }
                                   ^
```

Neither direction converts implicitly: an integer never becomes a pointer on its
own. The two operations that *do* join them are named in the model — `expose`
(pointer to integer) and `with_exposed_provenance` (integer to pointer) — and
they are the only place provenance is lost or regained. **A cast is that name**:

```minc
let addr: usize = p as usize;   // expose(p)
let p2: *u8 = addr as *u8;      // with_exposed_provenance(addr)
let p3: *u8 = (*u8)p;           // the same two lines, spelled C's way
```

Both are counted by `-Wprovenance`, whose sentence is the model's own: the
address is defined, and an access through it is defined only for an allocation
whose provenance has been exposed. A **reinterpretation** between two pointer
types (`p as *u8`, where the pointee changes and the address does not) is not in
the provenance pair at all: with opaque pointers it is a type change with no
instruction behind it. See [Casts](/language/expressions#casts).

## Provenance, in one paragraph

A pointer carries more than an address: it carries the permission to access a
particular allocation for a particular time, and that permission is called
*provenance*. It is what makes "is this access inside a live object" an answerable
question, and it is why the language needs no anti-aliasing rule and no `inbounds`
on arithmetic it cannot prove. The full statement — objects, access, alignment,
lifetime, and exactly what happens when an obligation is violated — is
[the memory model](/language/memory-model).

:::note[Not implemented yet]
`restrict`, address spaces, `*const T`, and the checked build's access guards are
not implemented. Today a pointer is one address, one pointee type, and the shapes
above — including arrays and a pointer to one (`&a[0]` is a `*i32`, `&a` is a
`*[4]i32`), which is why there is no decay to lose the bound.
:::
