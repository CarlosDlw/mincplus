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
literal that does not fit is an error rather than a truncation.

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
