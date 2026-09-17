# Slices

A slice is a **view** of some elements of a type: a pointer and a length, two
words, naming storage somebody else owns. It is written `[]T`.

```minc
let a: [4]i32 = [1, 2, 3, 4];  // storage: the array owns its elements
let s: []i32 = a[1..3];        // a view: two words naming `a[1]` and `a[2]`
```

The difference between the two spellings is not how many elements there are — it
is *who owns them*:

| | `[N]T` | `[]T` |
| --- | --- | --- |
| what it is | the elements, here | a view of elements somewhere |
| what copying it does | copies the elements | copies two words, and aliases the data |
| what it knows | the count, in the type | the length, as a value |
| where it may come from | its own declaration or initializer | an array, another view, or a pointer |

**Neither converts to the other.** An array is not a view and a view is not an
array, so `f(a)` where `f` takes a `[]i32` is an error:

```minc
fn i32 sum(s: []i32, n: i32) {
  let total: i32 = 0;
  for let i: i32 = 0; i < n; i += 1 { total = total + s[i]; }
  return total;
}

let table: [4]i32 = [1, 2, 3, 4];
sum(table, 4);      // error: `[4]i32` cannot be used as `[]i32`
sum(table[..], 4);  // the whole array, as a view
```

## Taking a view

Four forms, and the end is **one past the last element** — the convention that
makes `a[0..4]` the whole object and `a[4..4]` the empty view at its end.

```minc
a[..]      // the whole thing
a[1..]     // from 1 to the end
a[..3]     // from the start, up to but not including 3
a[1..3]    // from 1, up to but not including 3
```

A view can be taken of three things:

```minc
let a: [4]i32 = [1, 2, 3, 4];
let one: []i32 = a[..];        // of an array: the extent is in the type
let two: []i32 = one[1..3];    // of a view: the extent is the descriptor's word
let p: *i32 = &a[0];
let three: []i32 = p[0..2];    // of a pointer: both bounds are written
```

A pointer is the one base with no length in any type, so it is the one base that
has to write **both** bounds. That is not a restriction for its own sake: it is
the form whose extent the compiler cannot check, and it is written that way.

## A view's index is its own

`0` is the first element of the view, never an index into whatever it was taken
from. A view of a view is a view of the same elements, and its bounds are relative
to the view it was taken from:

```minc
let table: [8]i32 = [1, 2, 3, 4, 5, 6, 7, 8];
let mid: []i32 = table[2..6];    // [3, 4, 5, 6]
let inner: []i32 = mid[1..3];    // [4, 5]
let first: i32 = inner[0];       // 4 — that is `mid[1]`, which is `table[3]`
```

## Writing through a view

`s[i]` is a **place**: it reads and writes the storage the view names, like `p[i]`
does through a pointer. There is no copy anywhere, in either direction.

```minc
fn void add_ten(s: []i32) {
  s[0] = s[0] + 10;
}

let table: [4]i32 = [1, 2, 3, 4];
let view: []i32 = table[1..3];
add_ten(view);
let n: i32 = table[1];   // 11 — the array itself changed
```

Passing a slice passes two words. Reading or writing through it reaches the
caller's elements, so a slice parameter is how a function works on part of a table
without copying it.

## What is checked

A bound that is a constant is checked where it is written, because the count is in
the type and the bound is a number:

```minc
let a: [4]i32 = [1, 2, 3, 4];
let s: []i32 = a[0..5];   // error: the end 5 is outside `[4]i32`
let t: []i32 = a[3..1];   // error: backwards — a beginning cannot be past its end
```

A bound that is not a constant is the **programmer's word**, exactly as a runtime
`a[i]` is: this language does not claim to have checked something it did not.

## Lifetime

A view must not outlive what it views. That is the programmer's promise today, and
the reason a slice **has no literal**: a literal would live in a temporary, and a
view of a temporary is a view of nothing:

```minc
let s: []i32 = []i32{1, 2, 3};  // error: a slice has no literal
```

Name the object first, and view it.

## Members, length, and what is not here yet

- **No members.** `.` is not a postfix operator, so `s.len` is not a name for
  anything. When a length becomes readable it is `len(s)`, which will work for an
  array (where it folds to a constant) and for a view (where it reads the word) —
  one spelling for both, because an array can never have a member.
- **No literal** (`[]T{...}`), by design and with a sentence: see above.
- **No `append`, no `push`, no capacity.** A view does not know who owns the
  storage, so growing it is not its decision to make. A growable buffer is a
  library type over an allocator.
- **`sizeof`/`alignof`** are not in the grammar yet. The view's layout is defined
  and tested — two words, the pointer's width twice, aligned like a pointer — and
  the operator that exposes it arrives with a type in an expression.
- **`str` is not `[]u8`.** A `str` is a NUL-terminated pointer and stays that
  way; converting one to a view of bytes is a length computation, and it is
  explicit and named.
- **No `..` with a step.** A strided view is a different thing (a stride and an
  extent), not a slice with step.

## At a foreign boundary

A descriptor's layout is this compiler's own, so it is refused where a function
defined elsewhere is declared — with the two words a foreign callee can read
instead:

```minc
extern fn i32 takes(s: []i32);   // error: pass a pointer and a length instead
extern fn i32 takes2(p: *i32);   // fine: an address is something every ABI agrees about
```

The design record for all of this is
[`docs/architectures/slices.md`](https://github.com/CarlosDlw/mincplus/blob/main/docs/architectures/slices.md).
