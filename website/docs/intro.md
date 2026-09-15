---
slug: /
title: Introduction
---

# minc+

**minc+** is a systems programming language: a *minimal C* dialect with a small,
documented set of extras, and full C interoperability as a goal.

C is the baseline, not a stripped-down imitation of it. If you know C, the shape
of a `minc+` program is familiar on purpose — a translation unit, functions,
raw pointers, `extern` declarations, a preprocessor that is purely textual. What
the language changes is the small set of things C left to the programmer's
memory or to the optimizer's imagination:

- **Names and types are explicit.** A type is written where it is needed:
  `let count: i32 = 0;`, `fn i32 add(left: i32, right: i32)`. There is no second
  spelling for a parameter, because `fn i32 f(i32 a)` cannot say which word was
  the name.
- **Primitive type names state their width.** `i32` is 32 bits; `isize` is the
  pointer width. The C spellings (`int`, `long`, …) are also first-class types,
  and their widths come from the target's ABI.
- **No undefined behavior the programmer cannot see.** Signed overflow wraps,
  evaluation order is left to right, division by zero is defined to trap, and the
  compiler never hands the optimizer an assumption the program did not state.
- **A pointer is an address plus a provenance.** There is no type-based aliasing
  rule to trip over and no integer that is secretly an address.
- **Fewer silent mistakes.** Reading a binding that was never assigned is an
  error rather than a warning; a name used before it exists is caught before
  code is generated; a macro expansion that produced nonsense is reported at the
  site that wrote it.

## What it looks like

```minc
extern fn i32 puts(s: str);

fn i32 add(left: i32, right: i32)
{
  return left + right;
}

fn i32 main()
{
  const greeting: str = "hello, world";
  let total: i32 = add(20, 22);
  puts(greeting);
  return total;
}
```

```console
$ mincc run hello.mx
hello, world
$ echo $?
42
```

## Where to go next

- **[Installation](/getting-started/install)** — build the compiler from source.
- **[Hello, world](/getting-started/hello-world)** — the pipeline, end to end.
- **[A tour of the language](/getting-started/tour)** — the whole surface in one
  page.
- **[Types](/language/types)** — the reference proper starts here.
- **[Feature checklist](/language/features)** — what is decided, what is
  planned, and what is still an open question, in one page.

## What is not here

This site documents the **language** — what a `.mx` file means. It does not
document the compiler's internals: why a pipeline stage is shaped the way it is,
what the alternatives were, and what the references do. Those records live in the
repository, next to the code they describe, starting at `docs/architecture.md`
and `docs/architectures/`. [The architecture page](/tools/architecture) has the
map.

The language is under active development. Every page that describes something
not yet implemented says so in a **Not implemented yet** note, so nothing on this
site is a promise the compiler does not keep.
