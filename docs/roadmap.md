# Implementation roadmap

The build order for the compiler itself. The **language** feature checklist —
what `.mx` will actually have — lives in the [README](../README.md#language-features).

Legend: `[x]` done · `[ ]` planned · `[?]` open decision that changes scope.

Design targets: a compiler that runs on Linux, macOS, and Windows (Clang, GCC,
MSVC); an LLVM-based backend kept isolated so the front end never depends on
it; emitted objects following the System V AMD64 ABI and linking with `cc`/`ld`.

## 0. Foundation (current)

- [x] `support/` — spans, lines, UTF-8 (decoding, validation, BOM), sources,
      diagnostics, arena, interning, `Expected`
- [x] `source` as the single validation boundary (size, BOM, NUL, UTF-8)
- [x] `driver`: `cli`, `help_text`, `error_report`, `exit_code`,
      `--help`/`--version`, and the `lex` subcommand
- [x] `term` — `ColorMode` and tty detection, the only platform-specific module
- [x] Build system, presets, cross-platform CI, format and tidy gates
- [x] Architecture contract map in [`architecture.md`](architecture.md), lexer
      design in [`architectures/lexer.md`](architectures/lexer.md)
- [x] `Session` — per-compilation state container with per-file revisions, so
      editor edits keep a stable `FileId` while the contents change

## 1. Preprocessor — `src/lex/pp`

- [ ] `#include` with an include-path search (`-I`), `<>` vs `""`
- [ ] `#define` / `#undef`, object- and function-like macros
- [ ] Macro expansion with correct rescanning, recursion blocking, `##` and `#`
- [ ] Variadic macros (`...`, `__VA_ARGS__`) and `__VA_OPT__` `[?]`
- [ ] Conditionals: `#if`/`#ifdef`/`#ifndef`/`#elif`/`#else`/`#endif`, `defined`
- [ ] Constant-expression evaluation for `#if` (integer arithmetic, `&&`, `||`)
- [ ] `#error`, `#warning`, `#pragma`, `#line`, `__FILE__`/`__LINE__`
- [ ] Predefined macros (`__STDC__`, target/ABI macros, version)
- [ ] Directive-scoped diagnostics with correct spans inside macro expansions
- [ ] Expansion-limit guard and a clear "macro expansion too deep" diagnostic

## 2. Lexer — `src/lex`

Design decided in [`architectures/lexer.md`](architectures/lexer.md): a pure raw
lexer producing `{kind, length}` tokens with error flags, recorded into a
lossless per-file token buffer.

- [x] Raw lexer `lexOne(text, offset)`: pure, allocation-free, total, and
      resumable from any token boundary with no state parameter
- [x] Token kinds, `Span` derivation, and a contiguous token buffer covering
      every byte, with the lossless invariant audited on every lex
- [x] Keywords classified during lexing from one shared table (`fn`, `let`,
      `const`, `return`)
- [x] Integer literals: decimal, `0x`, `0b`, `0o`; base prefix with no digits
      flagged
- [x] Floating literals: decimal and hex floats, `e`/`p` exponents only when
      digits follow
- [x] Character and string literals with escape scanning; `''`, unknown
      escapes, and unterminated literals flagged
- [x] Operators and punctuation with longest match
- [x] Comments `//` and `/* */` (non-nesting), retained as trivia
- [x] Trivia (whitespace, LF/CRLF/CR, comments) retained as ordinary tokens,
      with a significant-token index for the parser
- [x] Lexical errors reported as diagnostics with `file:line:col`, code, and
      caret, one per problem, in a single pass
- [x] Token dump for inspection (`mincc lex <files...>`)
- [ ] Identifiers interned through `Interner` (`SymId`) — happens where symbols
      are wanted, not in the token
- [ ] Literal suffixes (`10u`, `1.0f`) and digit separators `[?]`
- [ ] String prefixes (`L`, `u8`, `u`, `U`) and raw/multiline strings `[?]`
- [x] A deterministic byte-soup property test asserting losslessness over
      arbitrary input
- [ ] Fuzz target wired to a fuzzing engine (libFuzzer/AFL) in CI

## 3. Parser — `src/parse` · AST — `src/ast`

- [ ] AST node model on `Arena`: kind, children, `Span`, no owning pointers
- [ ] AST dump/pretty-printer for snapshot tests
- [ ] Declarations: variables, functions, typedefs, `struct`/`union`/`enum`
- [ ] Declarators: pointers, arrays, function pointers, the full C
      "declaration mirrors use" grammar
- [ ] Statements: compound, `if`/`else`, `switch`, all loops, `goto`/labels,
      `return`, `break`, `continue`
- [ ] Expressions with C precedence and associativity, including comma and
      conditional operators
- [ ] Initializers, including designated initializers
- [ ] Type names and `sizeof`/`alignof`/casts
- [ ] The `fn` extension syntax alongside the C-compatible forms
- [ ] Error recovery that produces multiple diagnostics per run
- [ ] Grammar documented next to the code it implements

## 4. Semantic analysis — `src/sema`

- [ ] Scopes and name resolution, with shadowing rules
- [ ] Type system: the decided primitive set (`i8`..`i128`, `u8`..`u128`,
      `f32`/`f64`/`f80`, `bool`, `char`, `str`) plus the C-compatible spellings
      (`int`, `long`, `long long int`, ...) mapped per target ABI, with `char`
      fixed unsigned rather than inheriting C's sign, plus qualifiers, arrays,
      functions, pointers
- [ ] Usual arithmetic conversions, integer promotions, implicit conversions
- [ ] Constant expressions and folding (shared with `#if` evaluation)
- [ ] Type checking for every expression and statement form
- [ ] Lvalue/rvalue rules, address-of requirements, assignment compatibility
- [ ] Pointer semantics: element-scaled arithmetic, casts, byte-aliasing rules,
      and the provenance model the optimizer may rely on
- [ ] Control-flow checks: `break`/`continue` context, `goto` targets,
      missing `return`
- [ ] Storage classes and linkage: `static`, `extern`, tentative definitions
- [ ] Symbol table exported for the backend and C interop
- [ ] Warning set: unused entities, unreachable code, sign/conversion issues,
      shadowing (each with a code and a test)

## 5. IR — `src/ir`

- [ ] IR design and textual form `[?]` typed SSA vs. simple three-address
- [ ] Module/type/function/block/value model with an arena-backed builder
- [ ] AST → IR lowering (including `switch`, short-circuit, aggregate copies)
- [ ] IR verifier (types, terminator presence, dominance)
- [ ] Pass manager plus core passes: DCE, mem2reg, constant propagation,
      CFG simplification
- [ ] Debug-info hooks so source locations survive into the backend

## 6. Backend — `src/backend/llvm` (isolated)

- [ ] LLVM target initialization for AMD64; target machine and data layout
- [ ] IR → LLVM IR translation
- [ ] Object emission (`.o`) and assembly output (`--emit=asm`)
- [ ] Optimization pipelines for `-O0`..`-O3` and size
- [ ] Symbol visibility, sections, and relocations matching the ABI
- [ ] `[?]` Whether a hand-written AMD64 codegen is in scope at all
- [ ] `[?]` Whether non-AMD64 targets are ever planned

## 7. C interoperability — `src/cinterop`

- [ ] System V AMD64 argument classification (INTEGER/SSE/MEMORY) and returns
- [ ] Aggregates by value: struct passing/returning, unions, alignments
- [ ] Variadic calls (`va_list` conventions); bitfields `[?]`
- [ ] Calling into C: `extern` declarations resolved against real libc
- [ ] Being called from C: exported symbols with C linkage
- [ ] Emit/consume `.o` and `.a`; drive `cc`/`ld` for the final link
- [ ] `[?]` Whether to parse C headers directly or require declaration blocks
- [ ] Interop test suite that links against libc in both directions

## 8. Driver — `src/driver`

- [ ] `check`: run lex/parse/sema, render `DiagBag`, no codegen
- [ ] `build`: full pipeline → `.o` → link; `-o`, multiple inputs
- [ ] `run`: build then execute, forwarding program arguments after `--`
- [ ] Common flags: `-I`, `-D`, `-O`, `--emit`, `--target`
- [ ] Response files (`@file`) for long command lines
- [ ] `--color` with tty detection and `NO_COLOR`; wire `DiagRenderer` colors
- [ ] `-ferror-limit` and a "too many errors" path using `kMaxDiagnostics`
- [ ] `--version` printing version, host, and target triple
- [ ] `[?]` Incremental compilation / on-disk cache

## 9. Language extras

- [ ] `[?]` Fix the extension list with the language checklist
- [ ] Specify each extra: syntax, semantics, and C-interop interaction
- [ ] Reject extensions cleanly when a C-compatible mode is requested
- [ ] Language reference documenting every extension with rationale

## 10. Runtime and standard library

- [ ] `[?]` Reuse libc only, or ship a small bundled runtime/stdlib
- [ ] Entry-point handling and startup (`main`, CRT assumptions)
- [ ] Headers for interop and for the extras that need library support
- [ ] Decide what "minimal C" guarantees beyond the C standard subset

## 11. Quality

- [ ] Unit tests per module (lexer, preprocessor, parser, sema, IR)
- [ ] Snapshot tests for AST dumps and rendered diagnostics
- [ ] End-to-end tests: every `examples/*.mx` compiles, links, runs, and its
      output is asserted
- [ ] Negative tests: every diagnostic code has a test that triggers it
- [ ] Fuzzing for lexer, preprocessor, parser, and UTF-8, with a seeded corpus
- [ ] Sanitizer builds (ASan/UBSan) in CI, plus a TSAN run for the driver
- [ ] Coverage reporting and compile-time/memory benchmarks
- [ ] Cross-platform CI extended from `support` to the whole pipeline

## 12. Release and maintenance

- [ ] Versioning policy and `CHANGELOG.md`
- [ ] Packaging: install layout, `mincc` on `PATH`, tarballs/installers
- [ ] Reproducible builds
- [ ] `LICENSE`, `CONTRIBUTING.md`, `SECURITY.md`
- [ ] Release automation: tag → CI artifacts
- [ ] Docs: language reference, CLI reference, C-interop guide

## Definition of done

The compiler is "complete" for this project when:

- every example compiles, links, and runs on Linux, macOS, and Windows;
- interop tests both call into libc and are called *from* C, on all three
  hosts;
- every diagnostic has a code, a test, and a documented meaning;
- the pipeline runs clean under ASan/UBSan in CI and under `-Werror` with
  GCC, Clang, and MSVC;
- the language reference, CLI reference, and interop guide are published.
