---
sidebar_position: 4
---

# Expressions

An expression produces a value. The operators are C's, with C's precedence, and
the full table is [the operator reference](/reference/operators).

## Arithmetic

```minc
let sum: i32 = 1 + 2;
let difference: i32 = 10 - 3;
let product: i32 = 4 * 5;
let quotient: i32 = 20 / 3;      // 6 — integer division truncates toward zero
let remainder: i32 = 20 % 3;     // 2
let precedence: i32 = 1 + 2 * 3 - 4 / 2 % 3;
let grouped: i32 = (1 + 2) * (3 - 4);
```

`+` `-` `*` `/` `%` take arithmetic operands. When the two sides differ, the
result is the type the usual arithmetic conversions pick — the wider of the two,
and the unsigned side when the two are the same width with different signedness.
**An integer and a float do not meet**: `1 + 2.0` is an error and not an `f64`,
because the class of a number is the class of its spelling — see
[Conversions](/language/types#conversions).

Unary `-` and `+` take one operand, and `-` on an unsigned type is defined —
it wraps, like every other unsigned operation.

## Bitwise

```minc
let shiftedLeft: i32 = 1 << 8;
let shiftedRight: i32 = 0x100 >> 4;
let anded: i32 = 0x0F & 0x3C;
let ored: i32 = 0x0F | 0x30;
let xored: i32 = 0x0F ^ 0x3C;
let inverted: i32 = ~anded;
```

`~` is a prefix operator; the rest are binary. A shift's count is the right
operand and is *not* promoted to the left operand's type — `x << 1` is an `i32`
shift even when `x` is `i64`. A count that is negative, or at or past the width,
is an error when it is constant and a trap at run time when it is not.

## Comparison

```minc
let less: bool = 1 < 2;
let lessOrEqual: bool = 2 <= 2;
let greater: bool = 3 > 2;
let greaterOrEqual: bool = 3 >= 3;
let equal: bool = 1 == 1;
let different: bool = 1 != 2;
```

Every comparison produces a `bool`. Pointers compare by address, and `p == null`
asks whether a pointer is empty.

## Logical

```minc
let both: bool = less && greater;
let either: bool = less || different;
let negated: bool = !either;
```

`&&` and `||` take `bool` operands and produce a `bool`; there is no truthiness,
so an `i32` cannot be used as a condition and `1` is not `true`. Both
short-circuit: the right operand is evaluated only if the left does not decide
the answer. `!` is prefix and takes one `bool`.

## Conditional

```minc
let chosen: i32 = flag ? ifTrue : ifFalse;
```

The condition is a `bool`, and exactly one of the two arms is evaluated. Both
arms unify to one type: `flag ? 1 : 2` is an `i32`, and `flag ? 1 : 2.0` is an
`f64`. An arm whose type is `!` does not take part in that unification, because a
branch that never produces a value cannot be the branch that decides the type:

```minc
let chosen: i32 = flag ? 1 : die("unreachable");   // the conditional is an `i32`
```

## Casts

A [conversion](/language/types#conversions) is what the language does on its
own, and it stays inside one class of number. A **cast** is what the program
asks for: it is written, it is the only way to cross from an integer to a float
or to join a pointer and an integer, and it is what makes the instruction the
lowering emits a decision the checker recorded rather than one the backend made.

There are **three spellings and one meaning**. All three consult the same matrix
in the checker, so they agree by construction:

```minc
let a: i64 = (i64)n;     // C's form
let b: i64 = n as i64;   // the operator
let c: i64 = 10i64;      // the literal's own suffix
```

### `x as T` — the operator

`as` is a postfix operator at the unary level: below calls and indexing, above
every binary operator, exactly where C puts a cast.

```
a as i64 * 2      is  (a as i64) * 2
a as u8 as i64    is  (a as u8) as i64    // chains, left to right
```

What follows `as` is a **complete type run** — the same type a binding's
annotation uses — so `p as *u8`, `x as [4]i32` and `s as []u8` all *parse*.
Whether each one is allowed is a question for the matrix, one stage down: the
grammar says what was written, and never guesses at whether it was right.

### `(T)value` — C's spelling, and why it is unambiguous here

The parser decides between a cast and a parenthesised expression with two
questions, both lexical: is the run inside the parentheses a complete type, and
does the token after `)` start an expression? Both yes is a cast; anything else
is a parenthesised expression, as before.

That is only decidable because **type names are reserved**: `let i32 = 5;` is
refused at its declaration, so `(i32) + 1` cannot be a reference to a variable
named `i32`. C needs a typedef table in its parser and inherits the "most vexing
parse" for it; here the names are not declarable and the form needs no symbol
table at all. `(i32)` with no operand is not a cast and is not a syntax error
either — it is a type in a value position, and the diagnostic says so.

### `10u8` — the literal suffix

A suffix is part of the literal *token*, and the table is **closed**: a trailing
run of identifier bytes is claimed into the number only when the run is a known
suffix. `10u8` is one token; `10z` is a number and an identifier
(`parse-invalid-literal-suffix`); `1else` is still `1` and `else`.

A suffix makes the literal **immediately typed**: `let x = 10u8;` is a `u8`, not
an `i32` that happens to fit, while `let y = 10;` stays the deferred literal its
context decides. A suffixed literal that does not fit its own type is an error,
with the same code an out-of-range annotated literal already gets.

| suffix | type |
| --- | --- |
| `i8` `i16` `i32` `i64` `i128` `isize` | that type |
| `u8` `u16` `u32` `u64` `u128` `usize` | that type |
| `u`, `U` | `unsigned int` |
| `l`, `L` | `long` — **the target's** (64-bit on the Unices and Darwin, 32-bit on Windows) |
| `ll`, `LL`, … | `long long`; with `u`/`U` in any order and case, the unsigned counterpart |
| `f`, `F` | `f32` |
| `f32`, `f64`, `f80` | that type |
| `l`, `L` on a float | `long double` — **the target's** |

Three rules make the two halves one grammar. A float suffix on an
integer-spelled literal **makes it a float** whose value is exact (`12f` is
`12.0` as an `f32`; C refuses this, minc+ does not). An integer suffix on a
float-spelled literal is refused (`1.5u`), because there is no such number. And a
suffixed literal is typed where it stands, so nothing downstream re-decides it.

A suffix is refused on a character or a string literal: there is one character
type and one string type, and `'a'u8` says nothing `'a' as u8` does not.

### What a cast does

| from → to | meaning |
| --- | --- |
| integer ↔ integer, same width | the bits are the value; no instruction, only a different name |
| integer → wider / narrower | sign- or zero-extend by the **source's** signedness / truncate (defined, and it wraps) |
| integer → float | exact when the mantissa holds it, rounded when it cannot |
| float → integer | **guarded**: out of range traps, so no poison is ever produced |
| float → float, `bool` ↔ integer | extend/truncate (`f64` → `f32` rounds) / `0` and `1` |
| pointer → pointer, `str` ↔ `*u8`, `char` ↔ `u8` | a type change with no instruction behind it |
| `*T` → integer, integer → `*T` | the model's `expose` and `with_exposed_provenance` — see below |
| `!` → anything | vacuous: the operand never produces a value, so nothing is emitted |

A pointer and an integer are joined only by a cast, and that cast *is* the
model's named operation:

```minc
let addr: usize = p as usize;   // expose(p)
let p2: *u8 = addr as *u8;      // with_exposed_provenance(addr)
```

`-Wprovenance` names every such site, because an access through the result is
defined only for an allocation whose provenance has been exposed.

### What a cast may lose

`-Wcast`, off by default, reports each cast whose result may not be the value
that went in, naming the pair and *how* it changes:

```console
main.mx:1:47: warning[sema-cast-loses]: this cast from `i32` to `u8` may lose something: bits are dropped
main.mx:1:85: warning[sema-cast-loses]: this cast from `f64` to `i32` may lose something: an out-of-range value traps and bits are dropped
main.mx:1:106: warning[sema-cast-loses]: this cast from `i32` to `*u8` may lose something: the sign changes meaning
```

A cast is not a conversion, so the flag is opt-in: the program wrote it. The
conversion lint (`-Wconversion`) covers the implicit ones.

A cast between two pointer types loses nothing, and a cast that the language
converts on its own is never reported (a cast is always allowed where a
conversion is). A **constant** that the destination cannot represent is refused
rather than folded, at the stage that folds it:

```console
$ printf 'fn i32 main() { let x: i32 = (i32)1e30; return x; }\n' | mincc ir -
<stdin>:1:30: error[ir-cast-out-of-range]: this cast converts the constant `f64` to `i32`, and no value of `i32` holds it: ...
```

### What is refused

Each refusal says what to write instead:

| written | the refusal |
| --- | --- |
| `arr as *i32` | an array is not a pointer and it does not decay: `&arr[0]` is the first element's address, and the count travels beside it |
| `arr as [4]u8` | the same sentence — an array converts element by element or not at all, and no byte-level copy exists yet |
| `s as *i32` (a slice) | a view is a pointer *and* a length: `&s[0]` is its first element's address, and the extent is the view's own business |
| `p as []i32`, `n as []u8` | `*i32` is not a `[]i32`: a view is taken from an object that has a length, not from a bare pointer, and there is no literal view |
| `f as bool` | NaN is neither true nor false, so a float has no `bool` to convert to: write `f64 != 0.0` |
| `x as void` | `void` is the absence of a value, and a cast cannot make one |
| `x as fn(...)` | expected a type after `as` — a function type is not a type this language has yet |
| `1.5u` | a float literal cannot have the integer suffix `u`: `(u32)1.5`, or `1.5 as u32` |
| `10wb` | `wb` names a C23 bit-precise integer, and this language has no such type: write `i64` (or `i128`) |

Two of those are worth reading twice. `"abc" as u8` is **not** on the list:
`str` is a pointer, and pointer → integer is the `expose` row, so the cast is
defined and `-Wcast` reports the truncation. And `p as *u8` is not on the list
either: a pointer-to-pointer cast changes the type and emits nothing, which is
why it loses nothing.

## Assignment

Assignment is an expression whose value is the value stored, so it chains:

```minc
a = b = 0;

let updated = (value += 1);
```

The left side must be a modifiable place — see [Variables](/language/variables).

## Calls

```minc
let sum: i32 = add(1, 2);
let printed: i32 = printf("%d\n", sum);
```

The arguments are evaluated strictly left to right, and the callee is a name that
resolved to a function declaration. A call to a `void` function is a statement:
it produces no value.

:::note[Not implemented yet]
Function pointers, and therefore calling through anything but a name, are not
implemented. A variadic call promotes its arguments the way the C ABI says —
see [Types](/language/types).
:::

## Literals

```minc
1234567      // decimal
010          // decimal: a leading zero has no special meaning in `.mx`
0xBEEF       // hexadecimal, either case
0b10101010   // binary
0o755        // octal, spelled `0o`

3.14159      // f64
1e9 1e+10 2.5e-3     // exponents
0x1.8p3      // hexadecimal float

'x'  '\n'  '\x41'  '\u00e9'    // char
"hello"  "olá, mundo"          // str
```

An integer literal is `i32` when nothing asks for another type; a float literal
is `f64`. The type the surrounding context asks for decides otherwise, and a
literal that does not fit is an error rather than a truncation. A literal can
also carry its own type as a [suffix](#10u8--the-literal-suffix) — `10u8`, `12f` —
and then nothing downstream decides it.

The integer readers are shared between the type checker and the preprocessor, so
`0x10` means the same value in a `#if` as it does in an expression.

## Evaluation order

Operands and argument lists evaluate strictly left to right, and `&&`, `||` and
`?:` evaluate only the side taken. C leaves the order of operands unspecified,
which is why `f() + g()` may call either first; here it is always `f` then `g`,
and a program can rely on it.

## What arithmetic is defined to do

There is no undefined behavior in the arithmetic:

- **Signed and unsigned overflow wrap**, two's complement. The lowering does not
  attach `nsw`/`nuw` to an operation the language defines to wrap.
- **Division by zero traps** rather than being undefined, and it is an error
  when the divisor is a constant zero.
- **`INT_MIN / -1` traps**; `INT_MIN % -1` is `0`.
- **A shift count that is negative or too wide** is an error when constant and a
  trap otherwise.

Each of those is a rule the checker states and records, not a promise the
optimizer is trusted to keep.
