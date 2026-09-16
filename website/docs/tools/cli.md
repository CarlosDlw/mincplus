---
sidebar_position: 1
---

# The command line

`mincc` is the compiler. Every spelling of help a CLI of this kind is expected to
answer works:

```console
$ mincc                 # the overview
$ mincc -h
$ mincc --help
$ mincc help run        # one command's page
$ mincc run --help      # the same page
```

Each command has its own grouped page, generated from the same table the parser
reads — so a page and its parser cannot disagree about a flag's name.

## The commands

| Command | What it does |
| --- | --- |
| `mincc build <files...>` | the whole pipeline: compile, emit, and link |
| `mincc run <files...> [-- args...]` | `build`, then run the program |
| `mincc check <files...>` | type-check every unit; nothing is emitted |
| `mincc lex <files...>` | one file's raw tokens, without preprocessing |
| `mincc pp <files...>` | the translation unit's token stream |
| `mincc parse <files...>` | the syntax tree of that stream |
| `mincc resolve <files...>` | the lowered tree, the scopes, and every name's declaration |
| `mincc ir <files...>` | the LLVM module of the translation unit |
| `mincc builtins` | the names the language binds, and what each one is |

The commands between `lex` and `ir` are **views of one pipeline**, each printing
the state at its stage and stopping there — nothing is emitted, and no two of
them run the same stage twice:

```
source → lex → preprocess → parse → lower → validate → resolve → check → ir → object → link
   lex     pp           parse                    resolve            check    ir            build
```

`builtins` reads no file: it is the compiler's own table, so the list and the
`/language/builtins` page cannot describe different things.

`-` names a file on standard input, read in binary:

```console
$ printf 'fn i32 main() { return 0; }\n' | mincc check -
```

## Global options

These are accepted by every command.

| Option | Effect |
| --- | --- |
| `-h`, `--help` | the overview, or one command's page with `mincc help <command>` |
| `-V`, `--version` | print the version; with `-v`, the block a bug report needs |
| `--color WHEN` | `auto` (default), `always`, `never`. `always` beats `NO_COLOR` and `never` beats everything |
| `-ferror-limit N` | show at most `N` errors and stop; `0` means all of them |

## Input options

| Option | Effect |
| --- | --- |
| `-D NAME[=BODY]` | define a macro before the file is read; may be repeated, and the order is kept |
| `-U NAME` | undefine a macro; every `-U` is applied after every `-D` |
| `-I DIR` | add a directory to the include search list, in the order written |
| `-isystem DIR` | like `-I`, but searched after every `-I`, and a file found there is a *system* header — warnings in it are suppressed, errors are not |
| `--target TRIPLE` | the target the C type spellings and the layout are read against |

`--target` takes an LLVM triple in `arch-vendor-os[-env]` form, and the spellings
LLVM itself recognizes: `arm64` and `aarch64` are the same machine. A triple this
compiler does not state is refused with a reason rather than guessed at, and the
**default is the host**.

## Diagnostic options

| Option | Effect |
| --- | --- |
| `-Wunused` | warn about a declaration nothing refers to |
| `-Wshadow` | warn about a declaration that hides another one |
| `-Wconversion` | warn about an implicit conversion that may lose information |

See [Diagnostics](/tools/diagnostics) for what each one reports.

## `mincc lex`

The raw lexer over one file. No directive is interpreted, so a `#` is an ordinary
token and `#include` is not expanded — that is `pp`'s work. Every byte of the
file belongs to exactly one token, whitespace and comments included.

```console
$ mincc lex examples/002_variables.mx
```

## `mincc pp`

The translation unit: includes resolved, macros expanded, conditionals decided.
The default output is the token stream; the flags below ask for the record the
preprocessor kept instead.

| Option | Effect |
| --- | --- |
| `--defines` | print the macros defined at the end of preprocessing |
| `--includes` | print the include graph, one edge per line |
| `--deps` | print the files the unit read, as `make` dependencies |
| `--at POS` | answer about one position — `line`, optionally prefixed by the file it belongs to |

## `mincc parse`

| Option | Effect |
| --- | --- |
| `--no-trivia` | leave whitespace and comments out of the dump |

The tree is lossless — every byte, trivia included — so `--no-trivia` is a
display filter and not a different parse.

## `mincc resolve`

The first command that looks at *meaning* rather than at shape. It does not look
at types — that is `check` — so a name that resolves is reported as resolving
whatever its type turns out to be.

| Option | Effect |
| --- | --- |
| `--ast` | print the lowered tree instead of the tables |
| `--refs` | print every name use with the declaration it resolved to |
| `--unresolved` | print only the uses with no target, and the reason each one has |
| `--at POS` | answer about one position: `line:col`, optionally prefixed by a file |

On success it prints one summary line per file:

```console
$ mincc resolve examples/002_variables.mx
# examples/002_variables.mx  (scopes 2, defs 6, refs 1, 0 error(s), 0 warning(s))
```

`--ast`, `--refs` and `--unresolved` print the whole record rather than the
summary; the declarations the language binds itself are marked with the name
they are, so a `null` in a dump is never mistaken for a `true`:

```console
$ mincc resolve examples/002_variables.mx

  defs
    #0  file#0  ordinary  const  false  refs 0  [predefined false]
    #1  file#0  ordinary  const  true   refs 0  [predefined true]
    #2  file#0  ordinary  const  null   refs 0  [predefined null]
    #3  file#0  ordinary  fn     main   refs 0  examples/002_variables.mx:2:8
    #4  function#1  ordinary  let  x   refs 1  examples/002_variables.mx:4:7
    #5  function#1  ordinary  let  y   refs 0  examples/002_variables.mx:5:7
```

## `mincc check`

The front end through type checking, and nothing after it. **On success there is
no output and the status is 0**, which is what makes it usable in a script
unchanged.

| Option | Effect |
| --- | --- |
| `--stats` | one summary line per input |
| `--types` | the table of types, and nothing else |
| `--ast` | the typed tree, where every node carries its type |
| `-Wunused` | warn about a declaration nothing refers to |
| `-Wshadow` | warn about a declaration that hides another one |
| `-Wconversion` | warn about an **implicit** conversion that may lose information |
| `-Wcast` | warn about a **cast** that may lose information (`sema-cast-loses`) |
| `-Wprovenance` | name every cast between a pointer and an integer (`expose`, `with_exposed_provenance`), since an access through the result is defined only where provenance was exposed |
| `--target TRIPLE` | the target the C spellings and the layout are read against; a triple the compiler does not state is refused with a reason, never guessed |
| `-D` / `-U` / `-I` / `-isystem` | the preprocessor's defines, undefines and search list, in the order written |

`-Wconversion` and `-Wcast` are two questions and not one: the first is about a
conversion the language inserted, the second about a cast the program wrote. A
cast that merely names what the language would do anyway is reported by neither.

```console
$ mincc check --stats examples/002_variables.mx
# examples/002_variables.mx  (scopes 2, defs 6, refs 1, functions 1)  0 error(s), 0 warning(s)
$ mincc check --types examples/003_types.mx
# types 23  target x86_64-unknown-linux-gnu  long=64  pointer=64
  #6  str  str  size=8  align=8
  #9  i32  int  size=4  align=4
  #20  !  never  size=0  align=0
```

The table is what the checker decided, printed in one form: the type, its kind,
its size and its alignment on this target. `long=64` is the ABI's answer for
`long`, so a reader can see the one C spelling whose width is not the same
everywhere.

## `mincc ir`

The typed tree lowered into an `llvm::Module`, printed as text. The CFG, the
optimizers and the cross-platform target are LLVM's; this stage materialises what
the type checker recorded.

| Option | Effect |
| --- | --- |
| `-g` | emit debug information into the module |
| `-fcheck` | emit the checked build's guards (on by default, because this command prints the module a `-O0` build emits) |
| `-fno-check` | print the module without them |

The guards are the memory model's diagnostic half: every access through a pointer
is tested against null, against the alignment its type requires, and — where the
record has an extent — against an index outside it. `mincc ir -fcheck` is how a
trap you saw in a running program is read back as the blocks and the message that
produced it ([the checked build](/language/memory-model#what-a-checked-build-reports)).

## `mincc build`

The whole pipeline, to a file: the front end, the LLVM lowering, the object, and a
link. **This compiler does not link**: a C driver (`clang`, `cc` or `gcc`) is
invoked with an `argv` array and does it, because the startup objects, the C
runtime and the platform's linker flags are knowledge a C toolchain already has.

| Option | Effect |
| --- | --- |
| `-o PATH` | where the output goes. `a.out` by default (`a.exe` for a Windows target), or the input with its extension replaced for `--emit obj`/`asm`. `-o -` writes the object or listing to standard output |
| `-O [LEVEL]` | `-O0`..`-O3`, `-Os`, `-Oz`. A bare `-O` is `-O1`, and the level is never taken from the next argument |
| `--emit KIND` | `exe` (default), `obj`, or `asm` |
| `-g` | emit debug information (DWARF on ELF and Mach-O, CodeView on PE) |
| `-fcheck` | guard every access through a pointer — null, alignment, and bounds — and print the site of the one that fails. On at `-O0`, which is the build a program is developed with |
| `-fno-check` | no guards, even at `-O0`: for a program whose invariants live in a language the compiler cannot see |
| `-v` | print the commands the build runs — the first thing to look at after a link failure |
| `-L DIR` | add a directory to the linker driver's search list |
| `-l NAME` | link with a library. Order is meaning, so libraries reach the driver in the order written, after the objects |
| `--linker PATH` | the linker *driver* to use: `clang`, `cc`, `gcc`, or a path. By default the first one found on `PATH` |
| `--sysroot DIR` | the target's system root, handed to the linker driver; required with `-L`/`-l` when the target is not the host |

```console
$ mincc build -o prog main.mx
$ mincc build -O2 --emit obj main.mx
$ mincc build -o prog main.mx util.mx -L build -l math
```

The invariant scan runs before the object is written: a module that violates a
rule of the language produces a diagnostic and **no file at all**.

## `mincc run`

`build`, and then the executable is run. The program is a child process and not a
JIT in this one, so its crash is its own, its exit status is the one this command
returns, and its standard streams are the terminal's.

```console
$ mincc run main.mx -- one two
```

Everything after `--` is passed to the program and read by nobody here, so
`mincc run p.mx -- -o --emit` hands the program two ordinary arguments.

## Response files and `-vV`

A command line can be read out of a file with `@file`, which is expanded before
the parse — as a compiler driver is expected to do when an argument list gets
long.

`-vV` prints the block a bug report needs:

```console
$ mincc -vV
mincc 0.1.0
binary:         mincc
host:           x86_64-unknown-linux-gnu
default target: x86_64-unknown-linux-gnu
LLVM:           22.1.8
```

A misspelled command or option gets a `did you mean`, and the nearest names are
suggested rather than guessed.
