---
sidebar_position: 3
---

# A tour of the language

The whole surface in one page. Every section links to the page that goes into
detail.

## Bindings

```minc
fn i32 main()
{
  // A binding is written `name : type = value`.
  let count: i32 = 0;        // mutable
  let inferred = 10;         // the type is the initializer's: i32
  const limit: i32 = 100;    // not assignable
  const alsoInferred = 42;

  count += 1;                // a place on the left, a value on the right
  count = limit - count;

  return count;
}
```

`let` is mutable, `const` is not, and both have the same shape: an optional
annotation and an initializer. There is no `mut`, no `var`, and no separate
syntax for a constant. A binding with no initializer is legal and holds no value
until an assignment reaches the read on every path — reading it earlier is an
error, not a surprise. See [Variables](/language/variables).

## Types

A primitive type states its width. Both sets below are first-class types.

```minc
let a: i8  = -8;      let b: i16 = -16;    let c: i32 = -32;
let d: i64 = -64;     let e: i128 = -128;  let f: isize = -1;

let g: u8  = 8;       let h: u16 = 16;     let i: u32 = 32;
let j: u64 = 64;      let k: u128 = 128;   let l: usize = 1;

let m: f32 = 0.0;     let n: f64 = 0.0;    let o: f80 = 0.0;

let p: bool = true;
let q: char = 'x';    // an 8-bit byte type, always unsigned
let r: str  = "hello";  // NUL-terminated, C-like
```

The C spellings name the same types, with the widths the target ABI gives them:

```minc
let a: int = 0;                     let b: uint = 0;
let c: long = 0;                    let d: unsigned = 0;
let e: short int = 1;               let f: long int = 2;
let g: long long int = 3;           let h: unsigned long long int = 6;
let i: signed char = -1;            let j: unsigned char = 255;
let k: float = 0.0;                 let l: double = 0.0;
```

`void` is a return type and never a value type: no object has it. `!` is the
bottom type and a return type only, and means "this never comes back". See
[Types](/language/types) and [The bottom type](/language/never).

## Literals

```minc
let decimal: i32 = 1234567;
let leadingZeroIsDecimal: i32 = 010;   // no implicit octal in `.mx`
let hex: i32 = 0xBEEF;
let binary: i32 = 0b10101010;
let octal: i32 = 0o755;

let fraction: f64 = 3.14159;
let exponent: f64 = 1e9;
let negativeExponent: f64 = 2.5e-3;
let hexFloat: f64 = 0x1.8p3;

let newline: char = '\n';
let unicode: char = '\u00e9';
let text: str = "olá, mundo";
```

## Operators

Arithmetic, bitwise, comparison, logical, the conditional operator, and every
assignment form — with C's precedence:

```minc
let sum: i32 = 1 + 2 * 3 - 4 / 2 % 3;
let grouped: i32 = (1 + 2) * (3 - 4);
let bits: i32 = (0x0F & 0x3C) | (1 << 8) ^ ~0x0F;
let both: bool = less && greater;
let chosen: i32 = flag ? ifTrue : ifFalse;

value += 1;   value <<= 2;   counter++;
```

The full table, including precedence and associativity, is
[the operator reference](/reference/operators).

## Statements

```minc
fn i32 classify(value: i32)
{
  // Parentheses around a condition are optional, everywhere one appears.
  if value < 0
  {
    return 0 - 1;
  }
  else if value == 0
  {
    return 0;
  }
  else
  {
    return 1;
  }
}

fn i32 factorial(n: i32)
{
  let result: i32 = 1;
  let i: i32 = 2;

  while i <= n
  {
    result *= i;
    i += 1;
  }

  return result;
}

fn i32 sumSkippingMultiplesOfThree(limit: i32)
{
  let total: i32 = 0;

  for let i: i32 = 0; i < limit; i += 1
  {
    if i % 3 == 0
    {
      continue;
    }

    total += i;
  }

  return total;
}
```

The body of `if`, `while` and `for` is always a block. Any clause of a `for` may
be left out. `break` and `continue` apply to the innermost loop. See
[Statements](/language/statements).

## Functions

```minc
// A parameter is a binding, so it is written the way every binding is written.
fn i32 add(left: i32, right: i32)
{
  return left + right;
}

// No parameters: an empty list, not a `void` one.
fn i32 zero()
{
  return 0;
}

// A `void` function may `return;` or fall off the end.
fn void touch(value: i32)
{
  if value < 0
  {
    return;
  }
}

// Defined elsewhere: another unit, a library, the C runtime.
extern fn i32 puts(s: str);

// A declaration that takes a variable number of arguments after `fmt`.
extern fn i32 printf(fmt: str, ...);
```

A function may be called before it is written: file-scope names do not depend on
order. See [Functions](/language/functions).

## Pointers

```minc
fn void bump(p: *i32)
{
  *p = *p + 1;
}

fn i32 main()
{
  let value: i32 = 41;

  let p: *i32 = &value;    // the address of a modifiable place
  *p = *p + 1;             // the place `p` names
  bump(p);                 // a pointer parameter shares the pointee

  let first: i32 = p[0];   // `p[i]` is `*(p + i)`

  let empty: *i32 = null;  // `null` is `*void`, and converts to any pointer
  let erased: *void = p;   // the untyped pointer
  let typedAgain: *i32 = erased;

  return first + (p == &value ? 1 : 0) + (typedAgain == empty ? 1 : 0);
}
```

A `*T` is an address, not an owner. `*void` converts to and from every pointer
type and cannot be dereferenced or stepped. See [Pointers](/language/pointers).

## The bottom type

```minc
extern fn ! exit(code: i32);

fn ! die(msg: str)
{
  exit(1);
}

fn i32 pick(c: bool)
{
  // `die` never produces a value, so this conditional is an `i32`.
  let chosen: i32 = c ? 1 : die("the impossible branch\n");
  return chosen;
}
```

A call to a function returning `!` is itself `!`, the code after it is
unreachable, and `!` converts into every other type because an expression that
never produces a value cannot be the wrong type. See
[The bottom type](/language/never).

## The preprocessor

Purely textual, and it runs before the parser:

```minc
#define MAX_ITEMS 100
#define PER_PAGE 25
#define PAGE_COUNT ((MAX_ITEMS + PER_PAGE - 1) / PER_PAGE)

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define STRINGIFY(x) #x
#define CONCAT(a, b) a ## b

#if PER_PAGE > 0
#define PAGED 1
#else
#define PAGED 0
#endif
```

Macro bodies are substituted as written — the parentheses are the author's job,
and they are not cosmetic. See [The preprocessor](/language/preprocessor).

## What is not in the language yet

The pipeline is complete end to end; what is missing is *surface*. Nothing on
this list has a decided syntax that is unimplemented — these are features that
are not there at all:

- arrays, `struct`, `union`, `enum`, and member access
- casts, `sizeof`, `alignof`, `offsetof`
- `switch`, `do`/`while`, `goto`, labels on `break`/`continue`
- `static`, thread-local storage, and `volatile`
- file-scope bindings (today every item is a function declaration)
- defining a variadic function (`va_start` does not exist yet)
- `#include` of C headers — `#include` itself works, on `.mx` headers
- the standard library, the checked build, and its runtime guards

The `README.md` in the repository carries the authoritative checklist, marked
*decided* / *planned* / *open*, and `docs/roadmap.md` tracks what is next.
