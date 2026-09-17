---
sidebar_position: 6
---

# Functions

```minc
fn i32 add(left: i32, right: i32)
{
  return left + right;
}
```

The return type comes right after `fn`, the parameters are in parentheses, and the
body is a block.

## Parameters

A parameter is written the way *every* binding is written — `name: type`, the same
shape as `let name: type = value`:

```minc
fn i32 multiply(left: i32, right: i32)
{
  return left * right;
}
```

There is no `fn i32 f(i32 a)` form, and not because it is unfamiliar: a type is a
run of identifiers, so in `f(i32 a)` the parser cannot say which word was the name
and which the type. C resolves that by knowing its type names; this language
declines to have two spellings and picks the one that is never ambiguous.

- **A parameter is mutable**, like a `let`. It is a local binding.
- **A parameter's scope is the body**, which *is* the function's scope — so a
  parameter and a `let` at the top of the body cannot share a name.
- An empty list is `()`, not `(void)`.

```minc
fn i32 zero()
{
  return 0;
}
```

## Return types

```minc
fn i32 length()  { return 1; }     // produces a value
fn void touch(x: i32) { }          // produces none; may `return;`
fn ! die() { /* never returns */ } // produces no value and never comes back
```

`void` is the absence of a value. `!` is the impossibility of arriving —
[its own page](/language/never). A function that returns neither of those must not be able
to reach the end of its body.

## Order does not matter

A function may be called before it is written:

```minc
fn i32 square(value: i32)
{
  return multiply(value, value);   // `multiply` is declared below
}

fn i32 multiply(left: i32, right: i32)
{
  return left * right;
}
```

Every file-scope declaration's type is established before any body is checked, so
a forward reference is not "resolved later" — it is impossible.

## `extern`: defined elsewhere

```minc
extern fn i32 puts(s: str);
extern fn i32 printf(fmt: str, ...);
extern fn void srand(seed: u32);
```

An `extern fn` is a **declaration**: it says the definition lives outside this
unit — another object file, a library, the C runtime — and the linker is what
resolves it. It has no body, and that is what tells the two forms apart:

| Written | Means |
| --- | --- |
| `extern fn i32 puts(s: str);` | defined elsewhere |
| `fn i32 twice(n: i32) { … }` | defined here |

A body-less `fn` is a function nobody defined; an `extern` with a body
contradicts its own word. Both are reported.

A declaration and a definition of one name are **one function**: the declaration
is what calls are checked against, the definition is what runs, and their
signatures have to agree.

```minc
extern fn i32 twice(n: i32);

fn i32 twice(n: i32)   // one function, one symbol
{
  return n * 2;
}
```

## Variadic declarations

`...` after the last parameter makes a declaration variadic:

```minc
extern fn i32 printf(fmt: str, ...);

fn i32 main()
{
  printf("%d %d\n", 1, 2);
  return 0;
}
```

Three rules, and each is a grammar rule with its own message:

- **Only a declaration may be variadic.** A definition would need `va_start`,
  which the language does not have yet, so `fn i32 f(x: i32, ...) { }` is
  refused rather than mis-compiled.
- **`...` needs at least one parameter before it.** With no fixed parameter there
  would be nothing to check the arguments against.
- **`...` is last.** A parameter after it cannot be reached.

The marker is part of the *type*: `f(i32)` and `f(i32, ...)` are different
functions, and declaring one and defining the other is a signature mismatch. It
is also visible in the IR and in the debug information, so a debugger reads the
signature the source wrote.

The arguments past the fixed ones are promoted the way the C ABI requires —
`bool`, `char`, `i8`, `i16`, `u8`, `u16` to `i32`, and `f32` to `f64` — and that
promotion is recorded like any other conversion, so the call emits the
instructions the ABI asks for. It is also the one place a promotion happens
silently in either direction: `printf("%d", x)` with an `i8` `x` will print
whatever the promoted value happens to be, exactly as it does in C.

## `main`

```minc
fn i32 main()
{
  return 0;
}
```

`main` is the entry point. It takes no parameters and returns `i32`, and a
different signature is reported:

```console
$ printf 'fn f64 main() { return 0.0; }\n' | mincc check -
<stdin>:1:8: error[sema-main-signature]: `main` must be declared `fn i32 main()`; this one returns `f64`
  fn f64 main() { return 0.0; }
         ^^^^
```

Its return value is the process's exit status.

## Recursion

Recursion needs nothing special:

```minc
fn i32 fib(n: i32)
{
  if n < 2
  {
    return n;
  }

  return fib(n - 1) + fib(n - 2);
}
```

## What a function is in the emitted code

One function in the source is one `llvm::Function`: one symbol, one type, one
declaration. That is why `extern fn i32 twice(n: i32);` above
`fn i32 twice(n: i32) { … }` is one symbol and not two — two symbols for one name
would leave one declared and never defined, and LLVM would rename the other.

An `extern` declaration the unit never defines in C reaches C's calling
convention and C's name, which is what makes `puts` and `printf` work with no
declaration file at all.

`static` before `fn` gives a function **internal linkage**, exactly as it does in
front of a `let`: `static fn i32 helper()` is a symbol the linker will not
resolve another unit's reference to. It is the same word with the same meaning,
and it is what a shared `.mx` header included by two units needs.

:::note[Not implemented yet]
Function pointers, `inline`, and any form of overloading are not implemented. A
name denotes one function, and calling is always a call to a name.
:::
