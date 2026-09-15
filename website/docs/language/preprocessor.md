---
sidebar_position: 10
---

# The preprocessor

The preprocessor is **purely textual**, runs before the parser, and adds no
parentheses of its own. It is C's, deliberately: a `.mx` file that uses macros
the way a C file does produces the tokens a C programmer expects.

It is also the stage with the most machinery, because macro expansion is the part
of a C-family compiler that is easiest to get subtly wrong. Three things it does
that C's specification leaves open, it decides: every expansion is bounded, every
token remembers where it came from, and a nested expansion that grows too far is
reported rather than allowed to run.

## Directives

| Directive | Effect |
| --- | --- |
| `#define NAME BODY` | object-like macro |
| `#define NAME(a, b) BODY` | function-like macro |
| `#define NAME(...) BODY` | variadic macro; `__VA_ARGS__` is the tail |
| `#undef NAME` | forget a macro |
| `#include "file"` / `<file>` | read another file into the unit |
| `#include_next` | continue the include search *after* the file that wrote it |
| `#if EXPR` | conditional, over the constant-expression grammar below |
| `#ifdef` / `#ifndef` | defined / not defined |
| `#elif` / `#elifdef` / `#elifndef` | chained conditionals |
| `#else` / `#endif` | the rest of the conditional |
| `#line N ["file"]` | rewrite the line numbering for what follows |
| `#error MESSAGE` | report an error at this line |
| `#warning MESSAGE` | report a warning at this line |
| `#pragma once` | include this file once per unit |

`#embed`, `#assert`, `#unassert`, `#sccs` and `#ident` are recognized and refused
rather than silently ignored: a directive that does nothing is a directive the
reader will believe worked.

The conditionals are checked for balance: an unterminated `#if`, an `#endif` with
nothing to close, an `#else` after an `#else`, and nesting past the limit all have
their own code.

## Macros

```minc
#define MAX_ITEMS 100
#define PER_PAGE 25

#define PAGE_COUNT ((MAX_ITEMS + PER_PAGE - 1) / PER_PAGE)

#define SQUARE(x) ((x) * (x))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))

#define STRINGIFY(x) #x
#define CONCAT(a, b) a ## b
```

Four rules, and the shapes above exist to show them:

**A body is substituted as written.** `#define LOW_BITS 1 << 8 - 1` expands to
`1 << 8 - 1`, and because `-` binds tighter than `<<` that is `128`, not `255`.
The parentheses are the macro author's job, and they are not cosmetic.

**A macro is expanded where it is used**, so it picks up whatever the names in its
body are *at that point*, not what they were when it was defined.

**Arguments are substituted expanded, and the result is rescanned.** That is why
`MAX(SQUARE(4), 10)` substitutes `((4) * (4))` and then expands `MAX` over it —
two steps, which is the whole reason macro expansion is specified that way.

**`#` and `##` use their operand unexpanded.** That is the rule both operators
share, and it is why the two-level macros below are the idiom for building a name:

```minc
#define DECLARE_PAIR(b) let b: i32 = 1; let CONCAT(b, 2): i32 = 2;
```

A body is not restricted to an expression: `;` and `{` are just tokens, so a macro
whose body is a statement or a whole block works, and the user of the macro writes
neither.

## Predefined macros

| Macro | Is |
| --- | --- |
| `__minc__` | `1` |
| `__STDC__`, `__STDC_VERSION__`, `__STDC_HOSTED__` | C compatibility |
| `__FILE__` | the file being read, as a string |
| `__LINE__` | the current line, as a number |
| `__COUNTER__` | a value that increments at every use, for unique names |
| `__DATE__`, `__TIME__` | the build date and time, or a reproducible value when `SOURCE_DATE_EPOCH` is set |
| `__has_include(...)` | whether an include would be found; only inside `#if` |
| `_Pragma("...")` | the operator form of `#pragma` |

The list is deliberately short. A compiler that defines hundreds of compatibility
macros is claiming to be something it is not.

`__FILE__`, `__LINE__` and `__COUNTER__` are computed **at the invocation**, not
at the definition, so a macro body containing `__LINE__` reports the line that
used it.

## `#if` expressions

The condition is a constant expression over integers, with `defined(NAME)`,
`defined NAME`, and the macro-expanded rest of the line:

```minc
#if defined(MAX_ITEMS) && MAX_ITEMS > 10
#define PAGED 1
#else
#define PAGED 2
#endif
```

The integer reader is the *same* one the type checker uses, so `0x10` means the
same value in a `#if` as it does in an expression. An identifier that is not a
macro is `0`, which is C's rule — and the preprocessor can be asked to report
that instead of assuming it, which is the `pp-undefined-identifier` diagnostic.
Division by zero inside a `#if` is an error, and so is a malformed expression:
the preprocessor does not guess.

## Provenance

Every token knows where it was **written**, and a mistake inside a macro points at
the `#define` that wrote it rather than at the line that used it:

```minc title="main.mx"
#define BAD_TYPE uintt
fn i32 main() { let x: BAD_TYPE = 1; return 0; }
```

```console
$ mincc check main.mx
main.mx:1:18: error[sema-unknown-type]: `uintt` is not a type
  #define BAD_TYPE uintt
                   ^^^^^
main.mx:1:18: note: did you mean `uint`?
```

The caret is under the `#define` that wrote the word, not under the `let` that
used it — which is where the fix has to be made.

Macro expansion is a stack of inputs, and the record of it is kept: which macro
expanded into what, in what order, and from where. That record is what `mincc pp`
prints, and it is what a future `-E`-style view and the language server will read.

## Budgets, because a preprocessor is adversarial input

Every failure mode that a macro system can have is bounded, and reaching a bound
is a diagnostic rather than a hang:

| Bound | On |
| --- | --- |
| expansion depth | one macro expanding into another, into another |
| expansion budget | total tokens produced by expansion |
| output budget | the size of the translation unit |
| include depth | how deep `#include` may nest |
| include budget | how many files one unit may read |
| self-reference | a file including itself |
| parameter count | the parameters of one macro |
| argument count | arguments passed to a macro |

An include guard is required for a header that is read more than once
(`pp-missing-include-guard`), because "included twice by accident" is the most
common C header bug and the fix is one `#pragma once`.

:::note[Not implemented yet]
`#include` works on `.mx` headers. Including a *C* header needs the C type
grammar, `struct`, and the rest of `src/cinterop`, and it arrives with them.
:::
