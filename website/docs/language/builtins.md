---
sidebar_position: 12
---

# Builtins

A builtin is an operation the **compiler** provides because the language cannot
express it. That is the whole test: if a function in a runtime could do it, it is
a function — not a keyword, not a special name. `exit`, `abs`, `sqrt`, `memcpy`,
`printf` are all ordinary declarations of ordinary symbols, and they go through
the linker like anything else.

What is left is arithmetic the language defines on a value's bits, and the
primitive a runtime's `assert` is built on. You can see the whole list — the same
data the compiler itself reads — with:

```console
$ mincc builtins
```

## The two spellings, and who owns the name

| Kind | Owned by | Can you declare it? | Promised? |
| --- | --- | --- | --- |
| `clz`, `ctz`, `popcount`, `bswap`, `rotl`, `rotr` | the language | yes — a local shadows it; the file scope cannot redeclare it | yes: every input has a defined answer |
| `__builtin_*` | the compiler | no — a declaration, a parameter, a local and a `#define` are all refused | no: stable is earned one row at a time |

The plain names are bound in the file scope before your unit is read, exactly as
`true` and `null` are. They are *not* keywords, and nothing in the compiler
matches a function by name:

```minc
fn u32 leading(x: u32) {
  return clz(x);            // the language's own operation
}

fn i32 clz(a: i32) {        // error: the file scope already binds `clz`
  return a;
}

fn i32 main() {
  let clz: i32 = 3;         // fine: a local shadows it (-Wshadow says so)
  return clz;
}
```

The prefix cannot be taken at all, and the refusal happens where the name is
*taken* rather than where it is used — so a `#define __builtin_trap ...` in some
header is a diagnostic, and so is a parameter called `__builtin_x`. C reserves
the same prefix and diagnoses neither.

## The bit operations

| Builtin | Signature | Answer |
| --- | --- | --- |
| `clz(x)` | any integer → same type | leading zero bits; `clz(0)` is the **width** |
| `ctz(x)` | any integer → same type | trailing zero bits; `ctz(0)` is the **width** |
| `popcount(x)` | any integer → same type | bits set |
| `bswap(x)` | 16/32/64/128 bits → same type | the bytes in the opposite order |
| `rotl(x, n)` | any integer, any integer count → the value's type | `x` rotated left by `n mod width` |
| `rotr(x, n)` | any integer, any integer count → the value's type | `x` rotated right by `n mod width` |

Three properties are deliberate, and they are what makes these safe enough to be
plain names:

**The width is the argument's.** No conversion, no promotion: `clz` of a `u8` is a
`u8` operation with an eight-bit answer, `clz` of an `i128` is `i128`. The
operation is on the bit pattern, so signed and unsigned are the same operation —
`clz(-1)` on an `i32` is `clz(0xFFFFFFFF)`.

```minc
fn u8 narrow(x: u8) {
  return clz(x) + ctz(x);   // 8-bit operations, 8-bit answers
}
```

**Every input has a defined answer.** This is not a detail: the machine
instruction is undefined for some of these, and a language that adopted it as-is
would have adopted the undefined behaviour. `clz(0)` is the width (not "undefined"),
and a rotate's count is taken modulo the width, so `rotl(1, 32)` is `1` and
`rotl(1, 33)` is `2`. Both are checked by the test suite *by running them*.

**A literal with no context takes its default.** `clz(1)` is an `i32` operation
because `1` is an `i32` — the language's ordinary rule for a literal nothing
decides. If you meant a 64-bit rotation, hold the value in a `u64` first:

```minc
fn u64 spin() {
  let x: u64 = 1;
  return rotl(x, 3);        // a 64-bit rotate
}
```

The count is deliberately *its own* integer type and not the value's: the language
converts no integer implicitly, so requiring `rotl(x: u8, n: u8)` would make a
count written as a literal, or held in a `usize`, an error for no gain.

### Why `bswap` cares about bits

`bswap` exists for a whole number of bytes, at least two. `bswap` of a `u8` is
refused **at the call site**, with a sentence, rather than at the machine:
LLVM's verifier rejects a module that contains a one-byte `bswap`, so a checker
that let it through would be a program whose module cannot be built. A refusal
you can read beats a failure you cannot repair.

## `__builtin_trap`

`__builtin_trap()` stops the program where it stands, in a way a debugger sees. It
is the primitive `assert` and `panic` will be written on, and it is the reason `!`
is a type rather than an annotation:

```minc
fn i32 abort_unless(b: bool) {
  if b {
    return 0;
  }
  __builtin_trap();         // the call is `!`: no `return` needed, and none missing
}
```

A call that never comes back has the type `!` (the [bottom type](/language/never)),
so the flow analysis knows control stops there — which is what lets a function
whose body ends in a trap keep its promise without a trailing `return`.

## What is not a builtin

- **Anything a library can do.** `exit`, `abort`, `malloc`, `strlen`, `pow` are
  symbols in a runtime, declared with `extern fn`.
- **A cast in disguise.** `ptr_to_int` and friends are casts; they will be spelled
  as casts, not as functions.
- **Compiler-internal operations.** The `memcpy` behind an array copy and the
  `trap` behind a checked division are things the *compiler* emits for a
  construct. They have no name in the language at all, so there is nothing to
  mistype and no promise about their semantics to keep.
- **`expect`, `assume`, `unreachable`.** The first two are metadata, and adopting
  one is a decision about what the optimizer may assume rather than a gap in a
  list. `unreachable` is undefined behaviour by construction: it arrives when a
  checked build can hold it, not before.

## What is coming

`sizeof` / `alignof` / `static_assert` are front-end operators rather than
builtins — they take a *type* rather than a value, so they are grammar.
`__builtin_{add,sub,mul}_overflow` is the checked-arithmetic family, and
`offsetof` waits for `struct`. All of them land in the same table, and `mincc
builtins` is how you will see them the day they do.
