# minc+

[![ci](https://github.com/CarlosDlw/mincplus/actions/workflows/ci.yml/badge.svg)](https://github.com/CarlosDlw/mincplus/actions/workflows/ci.yml)
[![docs](https://img.shields.io/badge/docs-carlosdlw.github.io-blue.svg)](https://carlosdlw.github.io/mincplus/)
[![license: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![platforms: Linux | macOS | Windows](https://img.shields.io/badge/platforms-Linux%20%7C%20macOS%20%7C%20Windows-blue.svg)
![standard: C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![backend: LLVM](https://img.shields.io/badge/backend-LLVM-blue.svg)

**Minimal C with extras and full C interoperability.**

minc+ is a small systems programming language and the compiler that implements
it. C is the baseline, not a stripped-down imitation of it: a translation unit,
functions, raw pointers, `extern` declarations and a purely textual preprocessor
look deliberate, because a C programmer is the reader it is written for. What it
changes is the small set of things C leaves to the programmer's memory or to the
optimizer's imagination:

- **Types say what they are.** `i32` is 32 bits and `isize` is the pointer width;
  the C spellings (`int`, `long`, …) are first-class too, and their widths come
  from the **target's** ABI, never from the machine running the compiler.
- **No undefined behaviour the programmer cannot see.** Signed overflow wraps,
  operands and arguments evaluate left to right, division by zero is defined to
  trap, and the compiler hands the optimizer a *closed list* of assumptions —
  never one the program did not state.
- **A pointer is an address with a provenance.** Not an integer that is secretly
  an address, and with no type-based aliasing rule to trip over.
- **An array is an object and a slice is a view.** `[4]i32` *holds* its elements
  and copies them; `[]i32` names somebody else's and copies two words. Neither
  decays into the other, so `sizeof` cannot start lying about a parameter.
- **Mistakes are errors, not warnings.** Reading a binding that was never
  assigned, and a non-`void` function that can reach its end without returning,
  both fail to compile.

## What it looks like

```minc
// hello.mx -- the preprocessor, a declaration of something defined in the C
// library, a raw pointer, and a function that cannot come back.

#define STEP 1

extern fn i32 puts(s: str);
extern fn !   exit(code: i32);

fn void bump(p: *i32)
{
  *p = *p + STEP;
}

fn ! fail(why: str)
{
  puts(why);
  exit(1);
}

fn i32 main()
{
  let answer: i32 = 41;
  bump(&answer);

  if answer > 41
  {
    puts("bumped past 41\n");
    return answer;
  }

  fail("unreachable\n");
}
```

```console
$ mincc run hello.mx
bumped past 41
$ echo $?
42
```

Four things are load-bearing in those twenty lines. `#define` is the
preprocessor's, and `STEP` is expanded before the parser sees the file.
`extern fn` reaches a function that lives in the C library — the linker resolves
it, because the ABI is the target's. `*i32` is a raw pointer and `&answer` is the
address of a modifiable place, so `*p = *p + 1` is a store through it. And
`fn !` is the bottom type: `fail` never returns to its caller, so `main` needs no
`return` after it, and the compiler *proves* it rather than taking the word —
the promise is derived from the return type and handed to LLVM as `noreturn`.

## Status

**v0.1.0 — pre-release. The pipeline is complete end to end; what is missing is
language surface.**

A file goes in and an executable that runs comes out:

```
source -> lex -> preprocess -> parse -> lower -> validate -> resolve -> sema
       -> ir -> codegen -> link
```

`mincc check hello.mx` prints nothing and exits 0 when a file is correct, which
is what makes it usable in a script. Each stage takes one artifact and returns
one, reports nothing, and leaves every error as a value with a code and a span;
only the reporting libraries and the driver turn those into text and an exit
code. The stage order is stated once, in
[`docs/architecture.md`](docs/architecture.md#the-pipeline).

What is not there yet is the *surface*: `struct`/`union`/`enum`, casts, `sizeof`,
`switch`, and C interoperability beyond declarations — plus `len(x)`, the one
thing a slice still lacks. Generics and function pointers are open questions
rather than planned work.

- **[Feature checklist](website/docs/language/features.md)** — the whole
  language in one page: what is **decided**, what is **planned**, what is still
  **open**. This is the authoritative list.
- **[Roadmap](docs/roadmap.md)** — the order the compiler is built in, and what
  is left in each stage.

Read the second one for a timeline and the first one to know whether something
exists.

## Build

Requires CMake 3.28+, Ninja, a C++20 compiler (Clang, GCC, or MSVC), the **LLVM
development files** (the IR and the backend are LLVM's; 22.1.8 here), and GTest.
GTest is found through the CMake package config, then `pkg-config`, then a pinned
`FetchContent` download; `-DMINC_FETCH_GTEST=OFF` forbids the download. ccache is
used when present. The CMake presets are the source of truth for every flag; the
`Makefile` is a shortcut over them and nothing else.

LLVM's *build configuration* is part of its ABI, so a mismatch is something the
configure step reports rather than a link that half-works: this compiler is built
with RTTI on, exceptions off, and `LLVM_ENABLE_ASSERTIONS` off (which
`LLVM_ENABLE_ABI_BREAKING_CHECKS` follows). A release LLVM — `llvm-dev`,
Homebrew's `llvm`, the official Windows installer — has that configuration.

If LLVM is somewhere CMake does not look by default (a versioned package under
`/usr/lib/llvm-22`, Homebrew's `llvm`, the Windows installer), every preset reads
one variable, so it is said once:

```sh
MINC_LLVM_ROOT=/usr/lib/llvm-22 cmake --preset dev
```

```sh
git clone https://github.com/CarlosDlw/mincplus
cd mincplus
cmake --preset dev && cmake --build --preset dev
ctest --preset dev
```

Presets are `dev` (Debug) · `release` · `ci` (warnings as errors) · `sanitize`
(ASan + UBSan, Linux). There are two speeds afterwards, and picking the right one
is most of the difference:

```sh
make quick              # build, test, format check -- the inner loop
make gates              # everything CI runs, before a commit
```

[`CONTRIBUTING.md`](CONTRIBUTING.md) has the gates, the house rules, the setup
notes that save the most time, and the order to do things in when the change is
a language feature.

## Documentation

Three kinds of writing live in this repository, and each has one reader:

| Where | What | Who reads it |
| --- | --- | --- |
| [`website/`](website) | The **language reference**: what a `.mx` file means, and what each tool does. Published at <https://carlosdlw.github.io/mincplus/>; `make docs` builds it, `make docs-serve` serves it. | Someone writing a program |
| [`README.md`](README.md) | What the project is, what exists today, and how to build it. | Someone deciding whether to use it |
| [`docs/`](docs) | The **design records**: why each stage is shaped the way it is, what the alternatives were, what the market does. [`docs/architecture.md`](docs/architecture.md) is the map. | Someone changing the compiler |

The split is deliberate. A design record is read while reading the code and goes
stale with it; a reference has to be true of the *language*, not of this week's
implementation. Anything not implemented is marked as such on the page that would
otherwise describe it.

```sh
make docs               # the language reference, which fails on a broken link
```

If you are about to change a stage, its record is next to the pipeline it
describes:

[lexer](docs/architectures/lexer.md) ·
[preprocessor](docs/architectures/preprocessor.md) ·
[parser](docs/architectures/parser.md) ·
[resolve](docs/architectures/resolve.md) ·
[sema](docs/architectures/sema.md) ·
[type constants](docs/architectures/type_constants.md) ·
[memory model](docs/architectures/memory.md) ·
[the bottom type](docs/architectures/never.md) ·
[IR](docs/architectures/ir.md) ·
[codegen](docs/architectures/codegen.md) ·
[CLI](docs/architectures/cli.md) ·
[builtins](docs/architectures/builtins.md) ·
[extern](docs/architectures/extern.md)

## Platforms

The compiler is **cross-platform**. It builds and runs on **Linux, macOS, and
Windows**, with **Clang, GCC, or MSVC**, and CI runs the full build, test,
format, and static-analysis suite on all three operating systems.

Windows is built and tested with **MSYS2's MinGW-w64** toolchain, and that is not
an arbitrary pick: LLVM's own prebuilt Windows packages are a *toolchain*
package — binaries with no `LLVMConfig.cmake` and no libraries to link against —
so there is nothing there for a compiler that embeds LLVM to build against. MSVC
is supported (the build sets `/utf-8`, `/Zc:__cplusplus` and `/W4 /permissive-`
for it) and is not exercised in CI yet, which is a gap rather than a claim.

C interoperability is a *separate axis* from host support:

| Axis | Support today |
| --- | --- |
| Hosts (where `mincc` runs) | Linux, macOS, Windows |
| Toolchains | Clang, GCC, MSVC (C++20) |
| Interop target (emitted code) | named by **LLVM triple**; System V AMD64 and Windows x64 first, `.o`/`.a` linked via `cc`/`ld` (or `link.exe`) |

Those are the targets the backend is exercised against, not a closed set: the IR
is LLVM's, so a target is a triple and a data layout rather than a code path
written by hand, and the front end does not change when one is added. The
**default target is the host**, written by CMake at configure time, so
`mincc build hello.mx` links and runs where it is sitting, `long` has that
machine's width, and `--target` is how a cross build is spelled — in any of the
spellings LLVM itself recognizes (`arm64` and `aarch64` are the same machine).

Because the same sources build everywhere, the support layer is written to behave
identically on every platform:

- **Line endings.** LF, CRLF, and lone CR are all recognized; terminators never
  leak into the text a diagnostic renders, so carets stay on their token.
- **Encoding.** Sources must be UTF-8. A BOM is stripped, UTF-16/UTF-32 is
  rejected with a message naming the encoding, and embedded NUL bytes and
  malformed UTF-8 are rejected at the source boundary — so every later stage can
  treat `SourceFile` as trusted text.
- **Paths.** File access goes through `std::filesystem` and treats arguments as
  UTF-8, so non-ASCII names resolve on Windows too; tests never hard-code `/tmp`.
- **Console.** CLI help is ASCII-only, and color is enabled only for a real
  terminal, honors `NO_COLOR` and `TERM=dumb`, and on Windows turns on
  virtual-terminal processing first so an older console gets plain text instead
  of escape soup.
- **Standard input.** `mincc lex -` reads stdin in binary mode on Windows, so a
  piped file is byte-identical to opening it.

## Commands

Eight commands, eight views, one pipeline — each names the stage it shows:

| Command | Shows |
| --- | --- |
| `mincc lex <files...>` | one file's raw tokens, no preprocessing — a directive's `#` is an ordinary token there |
| `mincc pp <files...>` | the token stream of the translation unit: macros expanded, includes resolved |
| `mincc parse <files...>` | the syntax tree over that stream |
| `mincc resolve <files...>` | the lowered tree, the scopes, and each name with the declaration it denotes |
| `mincc check <files...>` | the verdict, and nothing else on success — `--stats`, `--types`, `--ast` add detail |
| `mincc ir <files...>` | the LLVM module of the translation unit — `--target TRIPLE` picks the ABI |
| `mincc build <files...>` | a file on disk: an executable, an object, or an assembly listing |
| `mincc run <files...> [-- args...]` | the program's own output and exit status; everything after `--` is passed through |

`-D`, `-U`, `-I` and `--target` are front-end options, so every command that
preprocesses accepts them. The command line as a whole is finished: all six
spellings of help work, each command has its own grouped and width-aware page,
the parser and that page come from **one table** so they cannot disagree, a
misspelled command gets a `did you mean`, `@file` reads a command line out of a
file, and `-vV` prints the block a bug report needs. Every command is documented
in [`website/docs/tools/cli.md`](website/docs/tools/cli.md), and the design is in
[`docs/architectures/cli.md`](docs/architectures/cli.md).

`mincc check` is the one to reach for first: it is the whole front end through
type checking, with no codegen, and it prints nothing when the file is correct.

```console
$ printf 'fn i32 main() {\n  let x: uintt = 1;\n  return 0;\n}\n' | mincc check -
<stdin>:2:10: error[sema-unknown-type]: `uintt` is not a type
    let x: uintt = 1;
           ^^^^^
<stdin>:2:10: note: did you mean `uint`?
$ echo $?
1
```

## Repository layout

Three things about the tree are worth knowing before you read it. `src/support/`
is **LLVM-free by contract** and the only place platform-specific code lives, so
no other module contains a `#if`. `src/ir/` and `src/backend/` are the only two
stages that may include `llvm/*`, and a test greps the tree so the boundary fails
in CI rather than in review. And `examples/` is not a folder of samples: every
file there is lexed, parsed, resolved, type-checked and lowered by the test
suite, so an example cannot drift into syntax the front end does not accept.

- `src/support/` — spans, sources, diagnostics, arena, expected, interning, and
  the terminal.
- `src/lex/` · `src/pp/` — the raw lexer and the preprocessor that owns `#`,
  inclusion and macro expansion, with provenance that survives expansion and
  always-on resource budgets.
- `src/parse/` · `src/syntax/` — a parser that emits events, and the lossless,
  hash-consed tree built from them, plus a typed view over it.
- `src/ast/` · `src/resolve/` — lowering into a compact arena for analysis, and
  name resolution in two phases, so a name used above its declaration still has
  an answer.
- `src/sema/` — the type checker, which also publishes the two facts the IR is
  not allowed to re-derive.
- `src/ir/` · `src/backend/` — the typed tree into an `llvm::Module`, then an
  object or an assembly listing.
- `src/driver/` — `mincc` itself: the command line *as data*, response files,
  the one error format, and the subcommands.
- `src/cinterop/` — after the C type model, for the reasons in
  [`docs/architecture.md`](docs/architecture.md#the-pipeline).

The module graph, ownership, and the contract of each library — including where a
new file belongs — are in [`docs/architecture.md`](docs/architecture.md).

## Getting help

Ask in [Discussions](https://github.com/CarlosDlw/mincplus/discussions), or open
an issue using the template that matches. The forms ask for the two things that
make a compiler bug quick to fix: the smallest file that shows it, and the exact
command. Anything about a build or a wrong answer should start with the output of
`mincc -vV`, which is the version, the host, the default target and the LLVM
version in one block.

[`SUPPORT.md`](SUPPORT.md) says which question goes where, and where the answer
probably already is.

## Contributing

Contributions are welcome, and the ones that help most are not always code: a bug
report that reproduces, an example in `examples/`, a diagnostic that could say
something clearer. [`CONTRIBUTING.md`](CONTRIBUTING.md) has the setup, the gates,
the house rules, and the order to do things in when the change is a language
feature.

Every participant is expected to follow the
[Code of Conduct](CODE_OF_CONDUCT.md).

## License

MIT. See [`LICENSE`](LICENSE). Contributions are accepted under the same terms;
there is no CLA.

Security bugs are handled privately — see [`SECURITY.md`](SECURITY.md), which
includes what counts as a security bug in a compiler (wrong code first, crashes
second).
