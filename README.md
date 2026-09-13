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

Scaffold v0.2: the `src/support` foundation, the lexer (`src/lex`), the
preprocessor (`src/pp`), the parser and syntax tree (`src/parse`, `src/syntax`),
the lowered AST and the type checker (`src/ast`, `src/resolve`, `src/sema`), and
the `mincc` driver. The front end is wired end to end: `mincc check` runs
`source -> lex -> preprocess -> parse -> lower -> validate -> resolve -> check`,
so a file that starts with `#define` is type-checked as a translation unit.
`build` and `run` parse correctly but report that they are not implemented;
the backend is what they are waiting for. `--help` and `--version` are
functional.

The stage order is fixed and written down once, in
[`docs/architecture.md#the-pipeline`](docs/architecture.md#the-pipeline):
`lex` and `preprocess` (phases 3 and 4 of translation), then
`parse -> lower -> validate -> resolve -> sema -> ir -> codegen -> link`.
The whole front end is shipped — through **lower**, **validate**, **resolve**
and **sema**, which are stages of their own rather than one pass: a C-like
grammar lets a call name a function defined further down, so name resolution has
to finish before any body can be type-checked, and the green tree is built for
fidelity rather than for analysis. **`ir` is next.** Each stage takes one
artifact and returns one, reports nothing, and leaves every error as a value
with a code and a span; only the `*_report` libraries and the driver turn those
into text and an exit code.

Five commands, five views, one pipeline — each names the stage it shows:

| Command | Shows |
| --- | --- |
| `mincc lex <files...>` | one file's raw tokens, no preprocessing — a directive's `#` is an ordinary `Hash` token there |
| `mincc pp <files...>` | the token stream of the translation unit: macros expanded, includes resolved |
| `mincc parse <files...>` | the syntax tree over that stream |
| `mincc resolve <files...>` | the lowered tree, the scopes, and each name with the declaration it denotes |
| `mincc check <files...>` | the verdict, and nothing else on success — `--stats` adds one summary line per file, `--types` the type table, `--ast` every node with the type it was given |

`-D name[=body]`, `-U name` and `-I dir` are front-end options, so every command
that preprocesses accepts them, and a `-D` is a real source file
(`<command line>`) so a caret on a command-line token points at something a
reader can find. `--target systemv-amd64|windows-x64` (`check`) picks the ABI the
C type spellings mean against, so `long` is 64 bits on the first and 32 on the
second — the width is read from a table, never from the machine running the
compiler.

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

```console
$ mincc parse examples/001_main_func.mx
== examples/001_main_func.mx  (30 bytes, 19 tokens, 8 nodes, 0 errors, depth 5)

File@0..30
  FnDecl@0..29
    KwFn@0..2 "fn"
    Type@2..6
      Whitespace@2..3 " "
      Identifier@3..6 "i32"
    Name@6..11
      Whitespace@6..7 " "
      Identifier@7..11 "main"
    LParen@11..12 "("
    ParamList@12..12
    RParen@12..13 ")"
    Block@13..29
      Newline@13..14 "\n"
      LBrace@14..15 "{"
      ReturnStmt@15..27
        Newline@15..16 "\n"
        Whitespace@16..18 "  "
        KwReturn@18..24 "return"
        LiteralExpr@24..26
          Whitespace@24..25 " "
          IntegerLiteral@25..26 "0"
        Semicolon@26..27 ";"
      Newline@27..28 "\n"
      RBrace@28..29 "}"
  Newline@29..30 "\n"
  EndOfFile@30..30
```

The tree is **lossless**: the leaves tile the file byte for byte, so its text is
the source and a formatter or a refactor needs no second representation. The
parser reports every syntax error it finds (not just the first), recovers from
it and keeps going, and the tree still covers every byte of even a malformed
file. The design behind that is in
[`docs/architectures/parser.md`](docs/architectures/parser.md).

Lowering compacts that tree for analysis, validation checks the structural rules
before the expensive passes, and resolution ties every name to the declaration it
denotes — in two phases, so a name used above its declaration still has an
answer. `mincc resolve` shows the result:

```console
$ mincc resolve examples/002_variables.mx
# examples/002_variables.mx  (scopes 2, defs 5, refs 1, 0 error(s), 0 warning(s))

  scopes
    #0  file  22..98  (root)
    #1  function  36..97  parent #0

  defs
    #0  file#0  ordinary  const  true  refs 0  [predefined]
    #1  file#0  ordinary  const  false  refs 0  [predefined]
    #2  file#0  ordinary  fn  main  refs 0  examples/002_variables.mx:2:8
    #3  function#1  ordinary  let  x  refs 1  examples/002_variables.mx:4:7
    #4  function#1  ordinary  let  y  refs 0  examples/002_variables.mx:5:7
```

The design behind lowering and resolution — the item tree, the scope model, the
language decisions they depend on — is in
[`docs/architectures/resolve.md`](docs/architectures/resolve.md).

Type checking is the last stage, and `mincc check` is its view: it types every
expression, decides the width of every C spelling against the *target* rather
than the host, and reports the verdict without emitting anything. Like a
compiler, it prints **nothing on success** — the exit code is the answer — and
everything else is opt-in.

```console
$ mincc check examples/002_variables.mx && echo ok
ok

$ mincc check --stats examples/002_variables.mx
# examples/002_variables.mx  (scopes 2, defs 5, refs 1, functions 1)  0 error(s), 0 warning(s)

$ mincc check --types examples/002_variables.mx
# types 21  target systemv-amd64  long=64  pointer=64
  #0  <error>  error  size=0  align=0
  #1  void  void  size=0  align=0
  ...
  #9  i32  int  size=4  align=4
  ...
  #20  fn i32()  function  size=0  align=0  params=0

$ printf 'fn i32 main() {\n  let x: uintt = 1;\n  return 0;\n}\n' | mincc check -
<stdin>:2:10: error[sema-unknown-type]: `uintt` is not a type
    let x: uintt = 1;
           ^^^^^
<stdin>:2:10: note: did you mean `uint`?
$ echo $?
1
```

`--ast` prints the same tree `resolve --ast` does, plus the type each node was
given, so the two dumps differ by exactly what this stage added; `--target
windows-x64` reads the C spellings against LLP64 (where `long` is 32 bits), and
`-Wconversion` reports the implicit narrowing that C allows silently. The design
— the type model, the conversions, and which stage owns which error — is in
[`docs/architectures/sema.md`](docs/architectures/sema.md).

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
- [x] `str` — NUL-terminated, C-like; a scalar type, not `char*` yet
- [x] `void` — a return type and (later) `void*`; never a value type: no
      object has it and no arithmetic is defined on it
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

#### Conversions and literal typing

The design record is
[`architectures/sema.md`](docs/architectures/sema.md#conversions), and these are
the language decisions it makes. They are *semantics*, so they are recorded
here as well.

- [x] A type is an **identity, not a spelling**: `i32`, `int` and `signed int`
      are one type, so the C spellings are interchangeable rather than merely
      accepted
- [x] **Narrowing is implicit at assignment** (initializer, assignment,
      argument, `return`), as in C — there are no casts yet, and a documented
      example must compile. A `-Wconversion` lint is designed for, off by
      default
- [x] **Integer and float literals are context-typed**: `let x: u8 = 255;` is a
      `u8` with no conversion, and `let x = 7;` / `let y = 1.5;` default to
      `i32` / `f64`. A literal that does not fit the type its context gives it is
      an **error**, not a silent truncation (unlike C)
- [x] `char` literals are `char` and string literals are `str` — never `i32`
      or a byte array
- [x] Integer promotions follow C (smaller than `i32` widens to `i32`) and the
      usual arithmetic conversions follow C17 6.3.1.8, with ranks by width
- [x] **A condition must be `bool`** — an arithmetic value does not implicitly
      convert (`x != 0` is the explicit form). C's "any scalar is a condition"
      is rejected
- [x] `bool` is **not arithmetic**: `!`, `&&`, `||`, `==`, `!=` are defined; a
      promotion to `int` as in C is not
- [x] `str` is **not arithmetic** and `==`/`!=` on it are **refused**, because
      C's `s1 == s2` compares addresses — content comparison is a library call
- [x] A `const` name is an lvalue but **not modifiable**; assigning to it or
      applying `++`/`--` is an error
- [x] A non-`void` function that can reach its end without returning a value is
      an error, not C's undefined behavior with a warning
- [x] `main`, when declared, must be `fn i32 main()`

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
- [x] Function parameters, written `name: type` — the same shape as `let`, and
      only that: the C order `type name` is rejected rather than guessed at
- [ ] Global variables, constants
- [ ] `extern` declarations bound to C symbols
- [ ] Visibility (`pub` / `private`) and namespaces
- [ ] Module system and imports `[?]`
- [ ] Variadic functions, including calling C variadics
- [ ] Default arguments or named arguments `[?]`

### Scopes and names

How a name is tied to the declaration it means. These are *semantic* decisions,
so they are recorded here as well as in
[`docs/architectures/resolve.md`](docs/architectures/resolve.md#decisions-the-language-owns),
which is where the algorithm that depends on them lives.

- [x] A file-scope name is visible **independently of order** (as in Go's
      package block, not C's point of declaration): a call may name a function
      defined further down, and mutual recursion needs no prototype
- [x] A `let`/`const` initializer sees the **outer** binding, not the one being
      declared (`let x = x + 1;` shadows; `let x = x;` is the outer `x`, never
      the uninitialized new one)
- [x] Scopes are lexical and block-based; a block is a scope, and a function's
      parameters form one with its body's outermost block
- [x] Declaration-before-use is **not** required within a scope's *body*, but a
      `let` is in scope from the statement after it, as in C
- [x] `fn` **cannot** be declared inside a `fn` — the grammar has no nesting
- [x] Shadowing is **allowed**; diagnosed only under `-Wshadow`
- [x] Unused declarations are diagnosed under `-Wunused`
- [ ] `goto` and labels — the `Label` name space is reserved, the feature is
      not
- [ ] Visibility (`pub`/`private`) filters lookup rather than nesting scopes

### Statements and control flow

- [x] Blocks and `return`
- [x] `if` / `else` / `else if` — the condition takes no parentheses (and accepts
      them), and each arm is a block
- [x] `while`, and C-style `for` (`for init; cond; step`), parentheses optional
- [ ] `do`/`while`
- [ ] Range/`for`-in iteration `[?]`
- [ ] C `switch` and/or pattern `match` `[?]`
- [x] `break` / `continue`
- [ ] Labels on `break` / `continue` `[?]`
- [ ] `goto` and labels (C compatibility)
- [ ] `defer` `[?]`
- [ ] Assertions and checked runtime conditions

### Expressions and operators

- [x] Arithmetic, bitwise, comparison, and logical operators with C precedence
- [x] Short-circuit `&&` / `||` — parsed and typed (`bool` operands, `bool`
      result); the short-circuit *evaluation* is the IR's
- [x] **No undefined behaviour in integer arithmetic**: signed and unsigned
      overflow wrap (two's complement), and the lowering must not emit `nsw`
- [x] **Integer edges are diagnosed where they are constant and defined where
      they are not**: a constant that does not fit its type, a division or
      remainder by zero, `INT_MIN / -1`, and a shift count that is negative or
      at or past the width are all errors at compile time and traps at runtime
      (`INT_MIN % -1` is `0`) — never a value the optimizer may invent
- [x] **Evaluation order is specified**: operands and argument lists evaluate
      strictly left to right, and `&&`/`||`/`?:` evaluate only the side taken
      (C leaves the order unspecified)
- [x] Assignment and compound assignment, with the left side checked to be a
      modifiable place
- [x] Conditional expression `?:`, with both branches unified to one type
- [ ] Casts
- [ ] `sizeof`, `alignof`
- [ ] Address-of and dereference (full set in *Pointers and raw memory*)
- [ ] Member access and indexing
- [ ] Slicing syntax `[?]`
- [x] Literals: integers (bases, suffixes), floats, chars, strings — read by
      one shared reader, so the type checker and the preprocessor cannot
      disagree about what `0x10` or `0755` means
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
- [x] **Definite assignment**: a `let` with no initializer holds no value until
      an assignment reaches the read on *every* path, and reading it is
      `sema-use-before-assignment` — an error, as in Java, C# and Swift, not C's
      warning-if-you-are-lucky; the accepted side of the rule, including the
      `while true` plus `break` idiom, is
      `examples/008_definite_assignment.mx`
- [ ] Overflow checks in debug builds `[?]`
- [ ] Null safety `[?]`
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
  version output), `error_report` (the one error format), `input_source` (the
  one way an input path or `-` is loaded), `lex_command`, `parse_command` (the
  subcommands), `exit_code`. The version header is generated from
  `cmake/version.h.in`; the source tree holds no second copy.
- `src/parse/` — the parser: a recursive-descent grammar over a token *source*,
  emitting events and error values. It links no diagnostics and no tree, so a
  grammar change is testable without a `Session`; `minc_parse_report` is the
  separate library that turns its errors into diagnostics.
- `src/syntax/` — the tree: a lossless, hash-consed, arena-backed green tree of
  that event stream, a parentless cursor over it, a typed AST view, the
  `TreeStore` that keeps trees keyed by `(FileId, revision)` and shares one node
  cache across them, and the `mincc parse` dump. Design in
  [`docs/architectures/parser.md`](docs/architectures/parser.md).
- `src/pp/` — the preprocessor. A client of the lexer that owns `#`, file
  inclusion and macro expansion, with always-on resource budgets and provenance
  that survives expansion. Its output is the preprocessed text and the tokens
  that tile it, so the parser and the tree builder read the same bytes, and each
  token still carries the source span it was written at. `minc_pp_parse` is the
  adapter that lets the parser read it, kept a separate target so the
  preprocessor never links the grammar. Design in
  [`docs/architectures/preprocessor.md`](docs/architectures/preprocessor.md).
- `src/ast/` — lowering the lossless green tree into a compact, arena-backed
  AST, plus the structural validation the parser could not do. Not the typed
  view of section 3: that makes the green tree pleasant to traverse and still
  carries trivia and error nodes, while this is a separate arena built for
  analysis, where every node keeps the `(FileId, range)` it came from. Design in
  [`docs/architectures/resolve.md`](docs/architectures/resolve.md).
- `src/resolve/` — name resolution in two phases, deliberately: **collect**
  every declaration into its scope first, then **resolve** each use, so a name
  used above its declaration still has an answer. It owns the scopes, one
  interned symbol per name, the typo suggestions, and the source→definition map
  that go-to-definition is built on. `mincc resolve` is its view. Neither
  `src/ast` nor `src/resolve` is a bullet inside `src/sema`, and both keep the
  stage contract: errors are values with a code and a span, never text.
- `src/sema/` — the type checker. It consumes the resolved tree — so nothing in
  it searches a scope — and produces the typed AST the IR needs: every
  expression with a type, the type store interned, C's conversions implemented
  once, and a failed expression typed as a poison rather than as a missing
  value. The typing is produced by a compilation-wide `Context` that owns the
  type store and answers the same question for the same revision with the same
  artifact, so the IR builder, a lint and the language server can all ask again
  for free. `mincc check` is its view. Design record, including the decisions
  above and the list of which stage owns which error:
  [`docs/architectures/sema.md`](docs/architectures/sema.md).
- `src/ir/`, `src/backend/`, `src/cinterop/` — **next**, in that order and for
  the reasons in
  [`docs/architecture.md#the-pipeline`](docs/architecture.md#the-pipeline).
- `tests/unit/` — gtest suites, one per module.
- `examples/` — `.mx` samples, and a regression suite: every file is lexed by
  `tests/unit/lex/examples_test.cc`, parsed by
  `tests/unit/parse/examples_parse_test.cc`, and carried through lowering,
  validation, resolution and type checking by
  `tests/unit/sema/examples_test.cc`, so an example cannot drift into syntax the
  front end does not accept, names it cannot resolve, or types that do not
  check.
  - `001_main_func.mx` — the smallest program: one function and a `return`
  - `002_variables.mx` — `let` with an annotation and with inference
  - `003_types.mx` — the primitive type names and the C-compatible spellings,
    declared with `let` and `const`
  - `004_operators.mx` — arithmetic, bitwise, comparison, logical, the
    conditional operator, and every assignment form
  - `005_literals.mx` — integers in four bases, decimal/hex floats, character
    and string escapes, and both comment styles
  - `pp/` — the **preprocessor corpus**. A different contract from the five
    above: these files contain directives, and a directive's `#` is an ordinary
    `Hash` token, so they lex cleanly too -- `mincc lex` shows the `#` and the
    directive name as tokens, without pretending to know what they mean, while
    `mincc parse` preprocesses them first. Each file documents the expansion it
    produces, and every file is preprocessed with one flag,
    `-I examples/pp/include`, which is the corpus's entire configuration.
    - `pp/001_object_macros.mx` — object-like macros, `#undef` and redefinition
    - `pp/002_function_macros.mx` — parameters, nested invocation, statement and
      block macros
    - `pp/003_stringify_and_paste.mx` — `#`, `##`, and the two-level trick that
      distinguishes a spelling from a value
    - `pp/004_conditionals.mx` — `#if`/`#elif`/`#else`, `defined`,
      `#elifdef`, and nesting
    - `pp/005_includes.mx` — both include spellings, and a guarded header
      included three times
    - `pp/006_variadic_macros.mx` — `...`, `__VA_ARGS__`, the one-empty-argument
      rule, and `__VA_OPT__`
    - `pp/007_builtins.mx` — `__FILE__`, `__LINE__`, `__COUNTER__`,
      `__has_include`
    - `pp/008_header_names.mx` — header-names as the language defines them
      (`//` and `/*` are ordinary characters inside `<...>`, escapes do not
      exist inside `"..."`), and `__has_include` asked about the same search
      list the guarded `#include` then uses
    - `pp/include/minc_limits.h`, `pp/include/minc_config.h` — the guarded
      headers `pp/005` pulls in, one of which includes the other
    - `pp/include/minc_pragma.h` — a header with no `#ifndef` guard at all,
      protected by `_Pragma("once")`, and included twice to prove it

Module contracts, ownership, and the dependency graph are documented in
[`docs/architecture.md`](docs/architecture.md); the implementation plan is in
[`docs/roadmap.md`](docs/roadmap.md); the lexer, parser, preprocessor,
lowering/name-resolution and type-checking designs -- with their research
references -- are in
[`docs/architectures/lexer.md`](docs/architectures/lexer.md),
[`docs/architectures/parser.md`](docs/architectures/parser.md),
[`docs/architectures/preprocessor.md`](docs/architectures/preprocessor.md),
[`docs/architectures/resolve.md`](docs/architectures/resolve.md), and
[`docs/architectures/sema.md`](docs/architectures/sema.md).

## Build

Requires CMake 3.28+, Ninja, a C++20 compiler (Clang, GCC, or MSVC), and
GTest. GTest is found through the CMake package config, then `pkg-config`,
then a pinned `FetchContent` download; pass `-DMINC_FETCH_GTEST=OFF` to forbid
the download. ccache is used when present. The CMake presets are the source of
truth for every flag; the `Makefile` is a shortcut over them and nothing else.

```sh
cmake --preset dev      # dev (Debug) | release | ci | sanitize
cmake --build --preset dev
ctest --preset dev
```

There are two speeds, and picking the right one is most of the difference:

```sh
make quick              # build, test, format check -- the inner loop
make gates              # everything CI runs, before a commit
```

`make quick` is one preset, so it never pays for the sanitizer build or the
static analysis. `make gates` is `ci`, `sanitize`, `format-check` and `tidy` in
the order CI runs them, cheapest first.

### Keeping builds fast

Two things dominate the wall clock, and both are the tool's configuration
rather than the code's:

- **ccache.** The same clean build of the tree takes ~106 s with an empty cache
  and ~4 s with a warm one. The 5 GiB default is small for four presets, so
  entries get evicted and paid for twice: `make ccache` reports the hit rate and
  `make ccache-tune` raises the limit (`CCACHE_SIZE=20G make ccache-tune`).
- **clang-tidy is the `tidy` gate.** Its path-sensitive checks cost seconds per
  file and one `clang-tidy` invocation walks them one at a time. `make tidy`
  therefore uses `run-clang-tidy -j` when it is installed -- same checks, same
  `--warnings-as-errors`, same non-zero exit on a finding -- and falls back to
  the sequential invocation when it is not. On the reference machine that is
  ~6m20s against ~1m50s for the same result.

Note that `dev` and `ci` produce identical objects apart from `-Werror`, which
ccache does key on, so they do not share cache entries. Iterating with `dev` and
running `make gates` before a commit costs one build of each; running `make dev`
*and* `make ci` pays for the same compile twice.

## Checks

```sh
# Formatting (matches the CI job)
make format-check

# Static analysis (needs a configured build for compile_commands.json)
make tidy
```

Both are wrapped by `make gates`; the underlying commands are
`clang-format --dry-run --Werror` over `include src tests` and
`run-clang-tidy -p build/tidy` over `src`.

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
- **Standard input.** `mincc lex -` and `mincc parse -` read stdin in binary
  mode on Windows, so a piped file is byte-identical to opening it.
