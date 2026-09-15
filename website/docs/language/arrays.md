# Arrays

An array is a fixed number of elements stored one after another, and the number
is part of the **type**. `[4]i32` and `[8]i32` are two different types, and
neither converts to the other — so a function that takes four elements cannot be
handed eight.

```minc
let a: [4]i32;          // four `i32`s, uninitialized
let b: [2][3]i32;       // two arrays of three — the outer one first
let p: *[4]i32;         // a pointer to an array of four
let q: [4]*i32;         // an array of four pointers
```

The spelling is a **prefix**: `[N]` in front of the element type, and it composes
with `*T` left to right. That is the one place where minc+ deliberately differs
from C: `int a[4]` and `int (*p)[4]` are almost the same text with different
meanings, and `[4]i32` against `*[4]i32` is not.

## Values

A value is written out in full. There are two spellings of the same value, and
both are complete:

```minc
// 1. The typed form: the count and the element type are in the literal, so it
//    needs no annotation and can be used anywhere an expression can.
let a = [4]i32{1, 2, 3, 4};
let g = [2][3]i32{[1, 2, 3], [4, 5, 6]};

// 2. The context form: the consumer says the type once, and the elements say it
//    nowhere.
let b: [4]i32 = [1, 2, 3, 4];
let c: [2][3]i32 = [[1, 2, 3], [4, 5, 6]];

// `_` takes the count from the elements that are written, which is how a table
// is written once instead of twice. Only the outermost count may be `_`.
let digits = [_]u8{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};

// The fill: one value and the count, and the two numbers have to agree.
let zeros = [64]u8{0; 64};
```

Five rules, and each one exists to kill a mistake:

- **The length is exact.** `[4]i32{1, 2, 3}` is an error, not a zero-filled array
  of four. C's "fewer initializers means zero-fill" is how an array ends up half
  written with no diagnostic at all; writing zeroes has a spelling, and it is the
  fill.
- **The count comes from one place.** `[_]` counts the elements for you, and only
  in the outermost position: with an inner `_`, every row of a two-dimensional
  array could have its own length, and rows of different lengths are not a type.
- **No ragged input.** `[[1, 2], [3]]` is an error: a row is an initializer of the
  row's type, so it is checked exactly like a one-dimensional one.
- **Nothing converts inside.** `[1, 2.0]` is an error, exactly as `1 + 2.0` is:
  the two classes of number do not convert implicitly, and an element is not an
  exception. An element too large for its type is the same story.
- **A string is not an array.** `let b: [4]u8 = "abc";` is refused. `"abc"` is a
  `str`, and a `str` is not bytes; that one implicit step is where C's
  `char[]`-versus-`char*` confusion comes from.

## Access

```minc
let a = [4]i32{1, 2, 3, 4};

a[0] = 42;            // an element of the object itself
let first: *i32 = &a[0];   // the element's address
let whole: *[4]i32 = &a;   // the whole array's address
```

There is **no decay**. An array does not become a pointer to its first element:
not in an initializer, not in an argument, not in an arithmetic expression. The
two pointers above are different types and are written by name, which is the
trade this language makes against C — a parameter can never lie about its size,
and `a[7]` on a `[4]i32` is a compile-time error.

**A constant index is checked.** The count is in the type and the index is in the
literal, so `a[4]` on a `[4]i32` is a diagnostic rather than a trip into whatever
follows the object. A runtime index is not checked at compile time: the element's
type is still known, and the access carries a *statically known extent* that a
checked build bounds-checks.

**Arrays are values.** Assigning one copies it, passing one copies it (the caller
makes the copy), and returning one copies it — the object is what moves, and
there is no sharing behind the name. The one thing that does not copy is a
pointer, because that is what a pointer is for.

## `const`

`const` protects the **name**, and it reaches the elements:

```minc
const table: [3]i32 = [3]i32{10, 20, 30};

let x = table[0];   // reading is fine
table[0] = 1;       // error: `table` is a `const` binding
let p: *i32 = &table[0];   // error: the same rule, one level down
```

## What is not here yet

- **A file-scope array initializer** (`const TABLE = [_]i32{...};`) is not
  implemented yet: it needs the aggregate value record the initializer-constant-
  expression walk reads, and it is the next step of the design
  (`docs/architectures/arrays.md`, step 8).
- **Slices** (`[]T`, a pointer and a length together) are **reserved**: the
  spelling parses and is refused with a sentence, so no program can mean anything
  else by it in the meantime.
- **`sizeof`/`alignof`** are not in the grammar yet. The layout is defined and
  testable; the operator that exposes it arrives with them.
- **Braces on a scalar or a `struct` name** (`i32{1}`, `Point{...}`) are not a
  typed initializer: today a typed initializer is recognized by the `[N]` its type
  starts with. The general `T{...}` lands with the first non-array aggregate.
