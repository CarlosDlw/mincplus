---
sidebar_position: 2
---

# Hello, world

A `.mx` file is one translation unit. It goes in one end of `mincc` and a running
program comes out of the other.

## A first program

```minc title="hello.mx"
fn i32 main()
{
  return 0;
}
```

```console
$ mincc run hello.mx
$ echo $?
0
```

`main` is the entry point. Its return type is `i32`, and its value becomes the
program's exit status.

## Printing

There is no `print` in the language, and that is deliberate: printing is a
library concern, and the library does not exist yet. What *does* exist is the
ability to declare a function defined somewhere else, which is how any C library
function is reached:

```minc title="hello.mx"
extern fn i32 puts(s: str);

fn i32 main()
{
  puts("hello, world");
  return 0;
}
```

```console
$ mincc run hello.mx
hello, world
```

`extern fn i32 puts(s: str);` is a *declaration*: it says the definition lives
outside this unit — here, in the C library, which `mincc build` links against —
and it gives the name the type every call is checked against. It has no body, and
that is what tells it apart from a definition:

| Written | Means |
| --- | --- |
| `extern fn i32 puts(s: str);` | defined elsewhere; the linker resolves it |
| `fn i32 twice(n: i32) { … }` | defined here |

Writing one as the other is reported: a body-less `fn` is a function that was
never defined, and an `extern` with a body contradicts its own word.

## The pipeline

Every command in the tool is a view of one stage of the same pipeline, so the
easiest way to learn the language is to watch a file move through it:

```console
$ mincc lex hello.mx         # the raw tokens, no preprocessing
$ mincc pp hello.mx          # the translation unit: macros expanded
$ mincc parse hello.mx       # the syntax tree over that stream
$ mincc resolve hello.mx     # every name, and the declaration it denotes
$ mincc check hello.mx       # the verdict and nothing else
$ mincc ir hello.mx          # the LLVM module
$ mincc build hello.mx -o hello   # an executable on disk
$ mincc run hello.mx         # build, then run it
```

Each one is documented on [the command-line page](/tools/cli).

## A longer program

```minc title="fib.mx"
extern fn i32 printf(fmt: str, ...);

fn i32 fib(n: i32)
{
  if n < 2
  {
    return n;
  }

  return fib(n - 1) + fib(n - 2);
}

fn i32 main()
{
  let i: i32 = 0;

  for ; i < 10; i += 1
  {
    printf("%d\n", fib(i));
  }

  return 0;
}
```

Four things are worth noticing, because they are the language in miniature:

- **The parentheses in `if n < 2` are optional.** A condition is an expression
  and a `{` can never continue one, so the parser knows where it ends either way.
  `if (n < 2)` parses identically.
- **The body of `if`, `while` and `for` is always a block.** There is no
  single-statement body, so there is no dangling-`else` question to answer.
- **`for` has three clauses** — `for init; condition; step` — and any of them may
  be left out. The init may be a `let` binding: `for let i: i32 = 0; i < 10; i += 1`.
- **`...` makes a declaration variadic.** The arguments past the fixed ones are
  promoted the way the C ABI says they must be: `bool`, `char`, `i8`, `i16`,
  `u8` and `u16` to `i32`, and `f32` to `f64`.

## When something is wrong

`mincc check` prints nothing when a file is correct, which is what makes it
usable in a script. When it is not, the message names the problem, points at the
place it was written, and — where there is one — names the declaration involved:

```minc title="bad.mx"
fn i32 main() {
  let x: i32 = 1
  return x;
}
```

```console
$ mincc check bad.mx
bad.mx:3:3: error[parse-expected-token]: expected ';'
    return x;
    ^^^^^^
```

The message says what was expected, at the place the parser noticed — and `check`
prints nothing at all when the file is correct, which is what makes it usable in
a script.

Diagnostics do not stop at the first one; they are produced in source order, and
`-ferror-limit=N` caps how many are shown. [The diagnostics page](/tools/diagnostics)
describes the format.
