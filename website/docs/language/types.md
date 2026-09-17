---
sidebar_position: 2
---

# Types

A type is written where a value needs one: after the `:` of a binding, after the
`fn` of a return type, after the `:` of a parameter. It is a run of words, and a
`*` in front of it makes it a pointer.

```minc
let count: i32 = 0;
fn i32 add(left: i32, right: i32) { return left + right; }
let p: *i32 = &count;
```

## Two ways to name a type

**The primitive names state their width.** `i32` is 32 bits on every target; a
declaration does not depend on a lookup table, and a reader does not have to
remember what the ABI says.

| Width | Signed | Unsigned |
| --- | --- | --- |
| 8 bits | `i8` | `u8` |
| 16 bits | `i16` | `u16` |
| 32 bits | `i32` | `u32` |
| 64 bits | `i64` | `u64` |
| 128 bits | `i128` | `u128` |
| pointer | `isize` | `usize` |

| Floating point | |
| --- | --- |
| `f32` | IEEE 754 binary32 |
| `f64` | IEEE 754 binary64 |
| `f80` | x87 extended precision — see the note below |

**The C spellings name the same types**, with the widths the target ABI gives
them. They exist so that code shared with C means the same thing on both sides:

| Spelling | Is |
| --- | --- |
| `int`, `signed`, `signed int` | the ABI's `int` — 32 bits on every supported target |
| `unsigned`, `unsigned int` | the same width, unsigned |
| `short`, `short int`, `signed short` | 16 bits |
| `long`, `long int` | 32 bits on Windows (LLP64), 64 on Linux and macOS (LP64) |
| `long long`, `long long int` | 64 bits everywhere |
| `unsigned char`, `unsigned short`, `unsigned long`, `unsigned long long` | the unsigned widths above |
| `float`, `double` | `f32`, `f64` |
| `signed char` | a signed 8-bit integer |
| `char` | see below |

A multi-word spelling is one type, not a sum: `unsigned long long int` names one
type, and `long long long` is not it.

:::note[`long` is the one width that depends on the target]
`long` is 64 bits under LP64 (Linux, macOS) and 32 under LLP64 (Windows), which
is C's rule and not this language's — the point of the C spellings is to mean
what C means. If you want a fixed width, write `i64`. `mincc check --types`
prints the target and the width it gives `long`.
:::

:::note[`f80` — x86 only]
`f80` names the **x87 80-bit format**, which is x86's arithmetic: `x86_64` and
`i386` have it, AArch64 and RISC-V do not. On a target with no x87 the checker
refuses the word and says what to write instead, rather than widening quietly to
`f64`:

```console
$ mincc check --target aarch64-unknown-linux-gnu main.mx
main.mx:2:10: error[sema-malformed-type]: `f80` is the x87 80-bit format, which `aarch64-unknown-linux-gnu` has no ABI for; use `f64`, or `long double` for this target's extended format
    let x: f80 = 1.5;
           ^^^
```

`long double` is the **portable** spelling of "the widest float this machine
has": it resolves to the format the target states — `f80` on x86, IEEE binary128
on AArch64 Linux, a plain `f64` under MSVC. Where the exact precision matters
across platforms, `f64` is the choice that means the same thing everywhere.
:::

## Type aliases

`type Name = T;` gives a type a second name:

```minc
// The file scope: order does not matter, because a name for a type is not storage.
type Pair = *Word;
type Word = u32;
type Table = [4]Word;
type Meters = f64;

fn Word add(a: Word, b: Word) {
  // The name is visible in a block too, and there it is read top to bottom.
  type Sum = Word;
  let total: Sum = a + b;
  return total;
}
```

**An alias is a name and not a type.** `Meters` and `f64` are the same type with
two spellings, so no rule anywhere knows the difference: assignment, argument
passing, `return`, a cast, an array element, a pointee, a slice — all of them
accept either. There is nothing to convert and nothing to unwrap, and `mincc` never
introduces a type of its own to represent the name.

**Two scopes, two orders.**

- **File scope is order-independent.** `type Pair = *Word;` may come before the
  line that declares `Word`, exactly as file-scope constants may refer to a later
  one. Nothing needs to be declared before it is named.
- **A block is read top to bottom.** A `type` among statements is visible from its
declaration to the end of the block, and gone after it:

```minc
fn i32 main() {
  type S = i32;
  let a: S = 1;          // i32
  {
    type S = f64;        // hides the outer S while this block is read
    let b: S = 1.5;      // f64
  }
  let c: S = 3;          // i32 again
  return a + c;
}
```

**A name may not be defined in terms of itself.** An alias is an abbreviation, and
an abbreviation that contains itself has no expansion:

```console
$ mincc check cycle.mx
cycle.mx:2:10: error[sema-type-alias-cycle]: the type `A` is defined in terms of itself: `A` -> `B` -> `A`. A name for a type is an abbreviation, and an abbreviation that contains itself has no expansion: recursion needs a type that names *itself*, and a name that stands for another type never does
  type B = A;
           ^
```

That includes `type P = *P;`. Recursion needs a type that names *itself* — a
`struct`, when it arrives — and a name that stands for another type never does.

**The name survives in messages and in the debugger.** A diagnostic prints what
you wrote and what it stands for, side by side:

```console
$ mincc check main.mx
main.mx:2:30: error[sema-invalid-assignment]: `i32` cannot be used as `Arr (aka `[8]u8`)` in this initializer
    let a: Arr = 1;
                 ^
```

and `mincc build -g` emits one `DW_TAG_typedef` per declaration and points every
binding that wrote the name at it:

```console
$ gdb -batch -ex 'break main' -ex run -ex 'whatis d' prog.bin
type = Meters         ← `let d: Meters`, an alias of f64
```

Two rules worth knowing, because they are the deliberate ones: **a type word may
not be renamed** (`type i32 = i64;` is refused — a name for a type is a name the
unit chooses, and `i32` belongs to the language), and **one name is one
declaration per scope** (a repeated alias is a redeclaration, as it would be for a
`let`).

## `bool`

`true` and `false` are the two values. A condition expects a `bool` — an
arithmetic value is not silently one:

```minc
let flag: bool = true;
if flag { /* … */ }        // fine
// if 1 { }                // error: the condition is `i32`, not `bool`
```

Comparisons produce a `bool`, and `&&`, `||` and `!` take and produce one.

## `char`

A `char` is a distinct **8-bit byte type**, represented as `u8` and always
unsigned. It is not an arithmetic type:

```minc
let letter: char = 'x';
let newline: char = '\n';
```

Two consequences, and both are deliberate:

- **`char` is unsigned, always.** `signed char` and `unsigned char` exist as
  the C types, and they are integers: they convert to `i32`/`u32` and take part
  in arithmetic. `char` does not, which removes C's "is `char` signed on this
  machine" question from the language entirely.
- **A `char` and a byte in a string are the same value.** A string is a sequence
  of bytes, and `char` is the element type of that sequence.

## `str`

A `str` is a **NUL-terminated, C-like** string: a pointer to bytes, ending at
the first NUL. It is a scalar type today, not `char*` and not a slice, which is
what makes passing it to a C function correct without any conversion.

```minc
let empty: str = "";
let plain: str = "hello, world";
let escaped: str = "tab:\there, quote:\"here, backslash:\\here";
```

Escape sequences in a string or character literal:

| Written | Is |
| --- | --- |
| `\n` `\r` `\t` `\v` `\f` `\b` `\a` | the control characters |
| `\e` | `0x1B`, ESC |
| `\?` `\"` `\'` `\\` | themselves |
| `\nnn` | octal, one to three digits |
| `\o{n...}` | octal, delimited |
| `\xn...` `\x{n...}` | hex — the delimited form says where the run ends |
| `\unnnn` `\Unnnnnnnn` | a code point, four or eight hex digits, encoded as UTF-8 |
| `\u{n...}` `\U{n...}` | the same, delimited |
| `\` + end of line | nothing: the literal continues on the next line |
| anything else | refused by name (`lex-unknown-escape`) |

A `str` is a sequence of **bytes**, so an escape names bytes: `\xNN` and `\o{...}`
are one byte each, and a code point is its UTF-8 encoding on every target.
[Expressions](/language/expressions#characters-strings-and-escapes) is the full
table, with what each refusal says.

:::note[Not implemented yet]
A `str` cannot be indexed, has no readable length, and does not take part in
`char*` arithmetic: a `str` is passed around, given to a function that takes one,
and converted to a pointer by a cast. Those three arrive with the library surface
rather than with the type.
:::

## `void`

`void` is a **return type** and nothing else: it means "this function produces no
value". No object has type `void`, a parameter cannot be `void`, and no
arithmetic is defined on it.

```minc
fn void touch(value: i32)
{
  if value < 0
  {
    return;   // a bare `return` is how a `void` function leaves early
  }
}             // and falling off the end is fine
```

An empty parameter list is written `()`, not `(void)`.

:::note[`void` is a return type and nothing else]
An object of type `void` does not exist, so there is nothing to declare, pass or
read: `void` appears in the return position of a function and nowhere else.
`*void` is a different thing and *is* implemented — it is the untyped pointer,
the one pointer type that converts to and from every other, and it cannot be
dereferenced or stepped. See [Pointers](/language/pointers).
:::

## `!`

The bottom type. It is a **return type only**, and a function declared `fn !`
never returns to its caller. It has [a page of its own](/language/never) because it is the
one type that changes what the rest of a function means.

## Pointers

`*T` is a pointer to a `T`:

```minc
let count: i32 = 1;
let p: *i32 = &count;    // a pointer to i32
let q: **i32 = &p;       // a pointer to a pointer to i32
```

Pointer rules, arithmetic and the untyped pointer are on
[the pointers page](/language/pointers).

## Conversions

A **conversion** is what the language does on its own, and it is silent by
default. A **[cast](/language/expressions#casts)** is what the program writes,
and it is the only way to cross a class of number or to join a pointer and an
integer. The conversion rules are short, and the type checker **records every one
it inserts** — which instruction the lowering emits is read from that record and
never decided a second time.

**Arithmetic converts to arithmetic — inside its own class.** An integer
converts to another integer and a float to another float: `let a: i64 = 1;`,
`let c: i8 = someI32;`, `let d: f32 = someF64;`. The conversion is exact when the
target can represent every value of the source, and truncating or rounding when
it cannot.

```console
$ mincc check -Wconversion main.mx
main.mx:4:20: warning[sema-implicit-conversion]: implicit conversion from `i32` to `i8` may lose information
    let narrow: i8 = wide;
                     ^^^^
```

A conversion that may lose information is **silent by default** and reported
with `-Wconversion`. It is a warning and not an error because C programmers
write `let byte: u8 = value & 0xFF;` on purpose, and the masking is right there
in the source.

**An integer and a float do not convert into each other**, in either direction.
This is the one place the arithmetic departs from C, and it departs on purpose: C
turns `double d = 1;` into a silent widening and `1 + 2.0` into a `double`, both
values the reader did not write — the second with a rounding the reader cannot
see. Here the class of a number is the class of its **spelling**: `1` is an
integer, `1.0` is a float, and crossing between the two is a **cast** —
`x as f64`, `(f64)x` — never a conversion.

```console
$ printf 'fn i32 main() { let a: f64 = 1; return 0; }\n' | mincc check -
<stdin>:1:30: error[sema-invalid-assignment]: `i32` does not convert to `f64` in this initializer: an integer and a float are different classes of number and do not convert into each other -- write the value in the class you want, as in `1.0` for a float
  fn i32 main() { let a: f64 = 1; return 0; }
                               ^
```

The rule is one function in the checker (`mixedNumberPair`) that every consumer
asks — an initializer, an assignment, an argument, a `return`, and the operands
of an arithmetic operator — so `1 + 2.0` is the same refusal with the same
sentence.

**A literal has to fit.** The one case that is an error rather than a warning is
a constant whose value does not fit the type it is given:

```console
$ printf 'fn i32 main() { let x: i8 = 300; return 0; }\n' | mincc check -
<stdin>:1:29: error[sema-literal-out-of-range]: this integer literal does not fit in `i8`
  fn i32 main() { let x: i8 = 300; return 0; }
                              ^^^
```

**`bool` and `str` convert to nothing.** `bool` does not promote to an integer,
so `flag + 1` does not compile — that is the C footgun this language does not
keep. `str` is not an integer either.

**Pointers convert to the same pointee, and to and from `*void`.** Nothing else:
pointer to integer, or integer to pointer, is not a conversion. Those two are
**casts**, and they are the model's `expose` (`p as usize`) and
`with_exposed_provenance` (`addr as *u8`) — the only place provenance is lost or
regained, and the reason both are counted by `-Wprovenance`.

**`!` converts to everything.** See [the bottom type](/language/never).

**`void` converts to nothing**, and a function type has no conversion at all.

In a variadic call, `bool`, `char`, `i8`, `i16`, `u8` and `u16` promote to `i32`
and `f32` promotes to `f64`, which is what the C ABI requires of the arguments
past the fixed ones. That is the one place a promotion happens, and it is the
call's rule rather than the type's.

:::note[Not implemented yet]
`sizeof` and `alignof` are not in the grammar yet; the layout they would read is
defined anyway. Casts are implemented, in three spellings — see
[Casts](/language/expressions#casts).
:::

## What a type decides

The type of a value is what the compiler knows about it afterwards, and every
stage below the checker reads that record rather than re-deriving it. That is why
the language can say, without hedging, that division by zero traps: the checker
diagnosed the constant case and stated the rule for the rest, and the lowering
materialises what the checker recorded.
