# minc+

Minimal C with extras and full C interoperability.

## Platforms

The compiler is **cross-platform**. It builds and runs on **Linux, macOS, and
Windows**, with **Clang, GCC, or MSVC**, and CI runs the full build, test,
format, and static-analysis suite on all three operating systems.

C interoperability is a *separate axis* from host support:

| Axis | Support today |
| --- | --- |
| Hosts (where `mincc` runs) | Linux, macOS, Windows |
| Toolchains | Clang, GCC, MSVC (C++20) |
| Interop target (emitted code) | System V AMD64 ABI, `.o`/`.a` linked via `cc`/`ld` |

Other interop targets are planned, not supported yet. Because the same sources
build everywhere, the support layer is written to behave identically on every
platform; the concrete guarantees are in
[Cross-platform notes](#cross-platform-notes).

## Status

Scaffold v0.1: the `src/support` foundation, the lexer (`src/lex`), and the
`mincc` driver. `mincc lex <files...>` works and prints the token stream;
`build`, `run`, and `check` parse correctly but report that they are not
implemented, and there is no parser, semantic analysis, or backend yet.
`--help` and `--version` are functional.

```console
$ mincc lex examples/002_variables.mx
== examples/002_variables.mx  (98 bytes, 46 tokens: 23 significant, 23 trivia)

   pos  offset  len  kind            flags  spelling
  ----  ------  ---  --------------  -----  ----------------------------------------
   1:1       0   21  LineComment     -      // variables examples
  1:22      21    1  Newline         -      \n
   2:1      22    2  KwFn            -      fn
   2:3      24    1  Whitespace      -       
   2:4      25    3  Identifier      -      i32
  ...
   8:1      98    0  EndOfFile       -      
```

Every byte of the file appears in exactly one token, including whitespace and
comments, and lexical errors are reported as `file:line:col: error[code]` with
a caret, without stopping at the first one. The design behind that is in
[`docs/architectures/lexer.md`](docs/architectures/lexer.md).

## Language features

What `.mx` is and will be. This is the language checklist, not the build order
— the implementation plan is in [`docs/roadmap.md`](docs/roadmap.md).

Legend: `[x]` **decided** — the design is fixed · `[ ]` **planned** · `[?]`
**open** — needs a decision. "Decided" is about the *language design*, not about
whether the compiler implements it yet; implementation status lives in
[`docs/roadmap.md`](docs/roadmap.md).

Start from the project's identity: a *minimal C* dialect — C is the baseline,
not a stripped-down imitation of it — plus a small, documented set of extras,
and full C interoperability in both directions.

### Syntax and files

- [x] `.mx` source files compiled by `mincc`
- [x] `fn` function declaration form (`fn i32 main() { ... }`), as in
      `examples/001_main_func.mx`
- [x] Block statements and `return`
- [x] `//` line comments
- [x] `let` bindings are **mutable**, with a colon type annotation or
      inference (`let x: i32 = 0;` / `let y = 10;`), as in
      `examples/002_variables.mx`
- [x] `const` bindings are **immutable** and have the same shape as `let`:
      `const x: i32 = 0;` or `const y = 10;` — no separate `mut`/`var`
- [ ] C-style function and declaration syntax alongside `fn`
- [ ] Doc comments attached to declarations
- [ ] Attributes/annotations on declarations `[?]`

### Types

Primitive names state their width instead of inheriting C's
implementation-defined ones. Both this set and the C spellings below are
first-class types; the examples use the primitive names.

- [x] Signed integers: `i8`, `i16`, `i32`, `i64`, `i128`, `isize`
- [x] Unsigned integers: `u8`, `u16`, `u32`, `u64`, `u128`, `usize`
- [x] Floats: `f32`, `f64`, `f80`
- [x] `bool`
- [x] `char` — a distinct 8-bit byte type represented as `u8`, always
      unsigned (see *C-compatible type names*)
- [x] `str` — NUL-terminated, C-like
- [ ] `void`
- [ ] Pointers — deliberately complete and C-level, see
      [Pointers and raw memory](#pointers-and-raw-memory)
- [ ] Fixed-size arrays
- [ ] Slices (pointer + length) `[?]`
- [ ] `struct`
- [ ] `union`
- [ ] `enum` constants and tagged unions `[?]`
- [ ] Function types and function pointers
- [ ] Type aliases
- [x] `const` bindings (see *Syntax and files*); immutability is a binding
      property, not a type qualifier yet
- [ ] Optional/nullable types and null safety `[?]`
- [ ] Tuples `[?]`
- [ ] Generics / parametric types `[?]`

`f80` is the x87 80-bit extended format. It is in the set because that is what
C's `long double` is on System V AMD64; codegen for it lands later.

#### C-compatible type names

The C spellings are a second, interchangeable way to name the same types, so
`.mx` code can be written in C style and C headers map onto it cleanly.

- [x] `int`, `uint`, `short`, `long`, `signed`, `unsigned`, `float`, `double`
- [x] Multi-word forms: `long int`, `long long int`, `unsigned long long int`,
      `short int`, `signed char`, `unsigned int`, ...
- [x] Widths follow the **target ABI**, not a fixed table, so linking against C
      behaves as the ABI requires
- [x] Interchangeable with the primitive names in declarations
- [ ] A portability lint (off by default) for ABI-dependent C spellings in code
      built for more than one target

Mapping on the current interop target (System V AMD64, LP64):

| C spelling | `.mx` type | Note |
| --- | --- | --- |
| `char` | `char` (`u8`) | raw byte 0..255; signedness is not inherited |
| `signed char` | `i8` | the signed byte escape hatch |
| `unsigned char` | `u8` | |
| `short` | `i16` | |
| `int` | `i32` | |
| `long`, `long long` | `i64` | both 64-bit on LP64 |
| `unsigned int`, `unsigned long` | `u32`, `u64` | |
| `float`, `double` | `f32`, `f64` | |
| `long double` | `f80` | x87 extended, 80-bit |
| `size_t` / `ssize_t`, `ptrdiff_t` | `usize` / `isize` | pointer-sized |
| `__int128` / `unsigned __int128` | `i128` / `u128` | compiler extension in C |

**`char` is unsigned — decided.** `.mx` `char` is a distinct 8-bit type whose
representation is `u8`; it never inherits C's implementation-defined
signedness (signed on x86-64, unsigned on ARM). The reason is that `str` is a
NUL-terminated byte string and UTF-8 code units are 0..255, so sign-extending
a byte is a bug generator rather than a feature. Consequences:

- C `char` maps to `.mx` `char`: the value crossing the boundary is the raw
  byte, so `str` indexing agrees with C bytes in both directions.
- C `signed char` maps to `i8`. Code that depends on C's signed-`char`
  behavior must say `i8` explicitly; the sign is never implicit.
- `char` stays **distinct** from `i8`/`u8`, mirroring C, so `char*` and `u8*`
  are not silently interchangeable and conversions between them are explicit.

**`long`, `unsigned long`, and `long double` are ABI-dependent — accepted.**
That is inherent to C's names, not a `.mx` quirk: `long` is 64-bit on LP64
(System V AMD64) and 32-bit on LLP64 (Windows x64), and `long double` is
80-bit on SysV but 64-bit under MSVC. The resolution is to keep the C
spellings for C-facing declarations and to treat them as non-portable by
definition; code that must compile unchanged for more than one target uses the
fixed-width primitives (`i32`, `i64`, `isize`) instead. Note the contrast with
`isize`/`usize`: they are pointer-sized too, but they are *named* for that
intent, which is exactly what makes them portable where `long` is not.

### Pointers and raw memory

Pointer control is complete and unchecked by design. `minc+` has to express
allocators, buffers, device registers, and anything else that sits on top of
the C ABI, so raw pointers are a first-class language feature and not a
hidden escape hatch.

**Pointer types**

- [ ] Pointers to any object type, at any depth (`**T`, `***T`)
- [ ] Function pointers, including calling through them
- [ ] `void*` (untyped) and pointers to incomplete/opaque types
- [ ] Qualifiers on the pointee, if `const`/`volatile` land `[?]`
- [ ] Pointer syntax: C-style `*T` or a `.mx` spelling `[?]`

**Addressing and access**

- [ ] Address-of `&` and dereference `*`
- [ ] Member access through a pointer
- [ ] Array-to-pointer decay, `&a[0]`
- [ ] Raw loads and stores, including type punning through a pointer

**Arithmetic and comparison**

- [ ] `p + n`, `p - n`, `p1 - p2`, `++p` / `--p`, `p[i]`
- [ ] Element-based scaling by `sizeof(*p)`, not byte stepping
- [ ] Pointer comparison and ordering
- [ ] `isize` as the pointer-difference type

**Conversions and casts**

- [ ] Pointer to integer and integer to pointer, sized by `usize`/`isize`
- [ ] Explicit reinterpret cast between pointer types
- [ ] `void*` to and from any object pointer `[?]` (implicit, C style, or cast)
- [ ] Function pointer to and from `void*` `[?]`
- [ ] `str` to and from `char*`, plus byte views (`u8*`/`i8*`) over any object

**Aliasing, alignment, and optimization**

- [ ] Every object is byte-addressable; `u8*` and `char*` may alias anything
- [ ] `restrict` / noalias annotation `[?]`
- [ ] Volatile accesses for memory-mapped I/O `[?]`
- [ ] Aligned vs unaligned access guarantees `[?]`
- [ ] The pointer provenance/aliasing rules the optimizer may assume `[?]`

**Safety model**

- [ ] Raw pointers are unchecked and need no keyword — plain C semantics `[?]`
- [ ] Null dereference: undefined behavior like C, or a debug-build trap `[?]`
- [ ] Optional non-null pointer type `[?]`
- [ ] Bounds are the programmer's responsibility: `*T` carries no length

### Declarations and modules

- [x] Top-level functions, including `main`
- [ ] Global variables, constants
- [ ] `extern` declarations bound to C symbols
- [ ] Visibility (`pub` / `private`) and namespaces
- [ ] Module system and imports `[?]`
- [ ] Variadic functions, including calling C variadics
- [ ] Default arguments or named arguments `[?]`

### Statements and control flow

- [x] Blocks and `return`
- [ ] `if` / `else`
- [ ] `while`, `for`, `do`/`while`
- [ ] Range/`for`-in iteration `[?]`
- [ ] C `switch` and/or pattern `match` `[?]`
- [ ] `break` / `continue`, with labels `[?]`
- [ ] `goto` and labels (C compatibility)
- [ ] `defer` `[?]`
- [ ] Assertions and checked runtime conditions

### Expressions and operators

- [ ] Arithmetic, bitwise, comparison, and logical operators with C precedence
- [ ] Short-circuit `&&` / `||`
- [ ] Assignment and compound assignment
- [ ] Conditional expression
- [ ] Casts
- [ ] `sizeof`, `alignof`
- [ ] Address-of and dereference (full set in *Pointers and raw memory*)
- [ ] Member access and indexing
- [ ] Slicing syntax `[?]`
- [ ] Literals: integers (bases, suffixes), floats, chars, strings
- [ ] Escape sequences, raw and multiline strings `[?]`
- [ ] String interpolation/formatting `[?]`

### Memory and lifetime

- [ ] Manual allocation interoperating with C (`malloc` / `free`)
- [ ] Allocators/arenas exposed to the language `[?]`
- [ ] Deterministic cleanup (`defer` or destructors) `[?]`
- [ ] Move semantics `[?]`
- [ ] Ownership/borrow checking `[?]`

### Error handling

- [ ] C-style error codes and `errno`
- [ ] `Option` / `Result` types `[?]`
- [ ] Error propagation (`?` / `try`) `[?]`
- [ ] Panics vs. recoverable errors `[?]`

### C interoperability (language surface)

- [x] C calling convention and ABI (System V AMD64)
- [ ] Calling C functions from `.mx`
- [ ] Exporting `.mx` symbols that C can call
- [ ] Struct layout compatibility, passing and returning aggregates by value
- [ ] Function pointers interoperating with C callbacks
- [ ] `char*` / `void*` / C string interop
- [ ] Opaque C types and forward declarations
- [ ] Importing C headers `[?]`
- [ ] Declaring links to libraries from source `[?]`
- [ ] Variadic C functions
- [ ] Bitfields `[?]`

### Standard library surface

- [ ] Core types: string, slice, optional/result
- [ ] I/O (print, files)
- [ ] Collections (list, map)
- [ ] Math and string utilities
- [ ] Formatting

### Safety

- [ ] Bounds-checked indexing, with an opt-out `[?]`
- [ ] Overflow checks in debug builds `[?]`
- [ ] Null safety `[?]`
- [ ] Uninitialized-variable diagnostics
- [ ] Type safety at C-interop boundaries `[?]`

### Tooling exposed in the language

- [ ] In-language tests (`test` blocks) `[?]`
- [ ] Doc comments feeding generated documentation
- [ ] Deprecation and stability attributes

## Layout

- `src/support/` — spans, sources, diagnostics, arena, expected, interning.
  LLVM-free by contract; everything else builds on it.
  - `span/` half-open byte ranges and merge/extend helpers
  - `line/` byte offset to 1-based (line, column) mapping
  - `source/` `SourceManager`/`SourceFile`; the boundary that turns bytes into
    trusted text
  - `diag/` `DiagBag` collection and pure `DiagRenderer` formatting
  - `mem/` bump `Arena` for AST/IR nodes
  - `intern/` deduplicated symbols
  - `utf8/` strict decoding, validation, and BOM detection
  - `expected/` `Expected`/`Unexpected` and the `Fallible<T>` alias
  - `session/` central per-compilation state: sources, symbols, diagnostics,
    and the node arena, with per-file revisions for editor use
  - `term/` `ColorMode` and tty detection; the only module containing
    platform-specific code, so nothing else has to
- `src/lex/` — the raw lexer: a pure `lexOne`, the lossless `TokenStream`, the
  token dump, and the flag-to-diagnostic reporting split into a separate
  library (`minc_lex_report`) so the lexer itself links no diagnostics.
- `src/driver/` — `mincc` entry point: `cli` (parsing), `help_text` (help and
  version output), `error_report` (the one error format), `lex_command` (the
  `lex` subcommand), `exit_code`. The version header is generated from
  `cmake/version.h.in`; the source tree holds no second copy.
- `src/parse/` and `src/syntax/` — planned: the parser (events over a token
  source) and the lossless syntax tree with its cursor and typed AST view.
  Design in [`docs/architectures/parser.md`](docs/architectures/parser.md).
- `src/lex/pp|sema|ir|backend|cinterop/` — planned.
- `tests/unit/` — gtest suites, one per support module.
- `examples/` — `.mx` samples, and a regression suite: every file is lexed by
  `tests/unit/lex/examples_test.cc`, so an example cannot drift into syntax the
  lexer does not accept.
  - `001_main_func.mx` — the smallest program: one function and a `return`
  - `002_variables.mx` — `let` with an annotation and with inference
  - `003_types.mx` — the primitive type names and the C-compatible spellings,
    declared with `let` and `const`
  - `004_operators.mx` — arithmetic, bitwise, comparison, logical, the
    conditional operator, and every assignment form
  - `005_literals.mx` — integers in four bases, decimal/hex floats, character
    and string escapes, and both comment styles

Module contracts, ownership, and the dependency graph are documented in
[`docs/architecture.md`](docs/architecture.md); the implementation plan is in
[`docs/roadmap.md`](docs/roadmap.md); the lexer design and its research
references are in
[`docs/architectures/lexer.md`](docs/architectures/lexer.md).

## Build

Requires CMake 3.28+, Ninja, a C++20 compiler (Clang, GCC, or MSVC), and
GTest. GTest is found through the CMake package config, then `pkg-config`,
then a pinned `FetchContent` download; pass `-DMINC_FETCH_GTEST=OFF` to forbid
the download. ccache is used when present.

```sh
cmake --preset dev      # dev (Debug) | release | ci | sanitize
cmake --build --preset dev
ctest --preset dev
```

## Checks

```sh
# Formatting (matches the CI job)
find include src tests \( -name '*.h' -o -name '*.cc' \) -print0 \
  | xargs -0 clang-format --dry-run --Werror

# Static analysis (needs a configured build for compile_commands.json)
clang-tidy -p build/dev $(find src -name '*.cc')
```

```sh
# AddressSanitizer + UndefinedBehaviorSanitizer over the whole project
cmake --preset sanitize
cmake --build --preset sanitize
ctest --preset sanitize
```

`cmake --preset ci` and `ctest --preset ci` (warnings as errors) are what CI runs
on every platform, and `--preset sanitize` is a Linux job there too. The
sanitizer switch is build-wide rather than per-target, so a new module cannot be
added to the tree and quietly left uninstrumented.

## Conventions

- Code, comments, and docs in English; identifiers are meaningful English words.
- No exceptions in utility code. Recoverable failures use `Expected<T, E>`
  (alias `Fallible<T>`); `Arena` reports exhaustion with `nullptr`.
- `[[nodiscard]]` on anything whose result must be observed.
- Every limit lives in `support/limits.h`, with `static_assert`s keeping the
  numeric and human-readable forms in sync. Nothing re-derives them.
- One responsibility per file; split before a file grows past a few hundred
  lines. No god-files.

## Cross-platform notes

These are contracts, not aspirations — the test suite enforces the first three.

- **Line endings.** LF, CRLF, and lone CR are all recognized; terminators never
  leak into the text a diagnostic renders, so carets stay on their token.
- **Encoding.** Sources must be UTF-8. A UTF-8 BOM is stripped; UTF-16/UTF-32
  sources are rejected with a message naming the encoding. Embedded NUL bytes
  and malformed UTF-8 are rejected at the source boundary, so every later stage
  can treat `SourceFile` as trusted text.
- **Paths.** File access goes through `std::filesystem` and treats arguments as
  UTF-8, so non-ASCII names resolve on Windows too. Tests never hard-code
  `/tmp`, `.gitattributes` normalizes the tree to LF on checkout, and MSVC is
  told `/utf-8` because the sources contain UTF-8 text in comments.
- **Compilers.** GCC's `-Wshadow` is stricter than Clang's, so both are run
  before a change is considered done; MSVC uses `/W4 /permissive-`.
- **Console.** CLI help is ASCII-only and diagnostic color is opt-in, so
  redirected output and non-UTF-8 consoles behave the same everywhere. Color is
  enabled only for a real terminal, honors `NO_COLOR` and `TERM=dumb`, and on
  Windows turns on virtual-terminal processing first so an older console gets
  plain text instead of escape soup.
- **Standard input.** `mincc lex -` reads stdin in binary mode on Windows, so a
  piped file is byte-identical to opening it.
