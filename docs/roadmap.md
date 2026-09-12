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
- [x] ASan + UBSan preset (`cmake --preset sanitize`) as one build-wide switch,
      so no module can be left uninstrumented by accident
- [x] Architecture contract map in [`architecture.md`](architecture.md), lexer
      design in [`architectures/lexer.md`](architectures/lexer.md), parser and
      syntax-tree design in [`architectures/parser.md`](architectures/parser.md)
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
- [x] Exhaustive short-input coverage: every 1-byte and 2-byte input, and every
      3-byte combination of the bytes that change scanning
- [x] A deterministic byte-soup property test asserting losslessness over
      arbitrary input
- [x] AddressSanitizer + UndefinedBehaviorSanitizer preset, run in CI
- [ ] Fuzz target wired to a fuzzing engine (libFuzzer/AFL) in CI

## 3. Parser — `src/parse` · syntax tree + AST — `src/syntax`

Design decided in [`architectures/parser.md`](architectures/parser.md): a
hand-written recursive-descent parser that emits **events**, consumed by a
separate builder into a **lossless, untyped green tree**, with a cursor and a
typed AST view on top.

- [x] `TokenSource` over the significant-token index: the parser is trivia-blind
- [x] Event stream (`Start`/`Finish`/`Token`) plus a side list of error values;
      the parser allocates no tree and links no diagnostics
- [x] `TreeBuilder` consuming events and the full token stream, attaching trivia
      into the node under construction (the only component that sees trivia)
- [x] Arena-backed green nodes/tokens: position-free and parent-free, with a
      hash-consing node cache for structural sharing (the tree is a DAG)
- [x] Cursor layer (absolute offset, range, traversal); identity is
      `(FileId, range)`, never a pointer. A parent pointer is deliberately
      absent -- a shared green node has no single parent
- [x] Unified `SyntaxKind` (u16) covering tokens and nodes, pinned to
      `TokenKind` by a `static_assert`
- [x] Typed AST accessors over the untyped tree, every field optional so
      half-written code is representable
- [x] Expressions: precedence climbing over one operator table, C precedence
      and associativity; left-associative chains via `precede`/`forward_parent`
- [x] Type positions (`fn` return type, `: T`) parsed by position, so no token
      kind for a type name and no lexer feedback
- [x] Error recovery: missing zero-width tokens, `Error` nodes, synchronization
      sets, and a bail-out cap
- [x] Stack-safety guard on every recursive entry (`kMaxNestingDepth`), so deep
      nesting is a diagnostic rather than a crash
- [x] Tree dump for inspection (`mincc parse <files...>`, `--no-trivia`)
- [x] Lossless-reconstruction check over every example
- [x] Declarations: functions and the `let`/`const` forms
- [x] Statements and blocks
- [ ] `SyntaxTreeStore` in `Session`, keyed `(FileId, revision)` — the tree
      carries its revision today; the keyed store lands with the editor path
- [ ] Golden-file tests (`tests/parse/data/*.mx` with expected tree and errors)
- [ ] `if`/`else`, loops, `break`/`continue`, `switch` — when their syntax is
      decided
- [ ] `struct`/`union`/`enum`, typedefs, and the full C declarator grammar
- [ ] Initializers, `sizeof`/`alignof`, casts, and the C-compatible `fn` forms
- [ ] Generated typed AST layer, once the node count justifies the generator
- [ ] Reserved syntax kinds for macro calls, token trees, and attributes
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

- [x] Unit tests per shipped module (support, lexer, parser, syntax tree,
      driver); preprocessor, sema, and IR suites land with those stages
- [x] Negative tests: the lexer's and parser's diagnostic paths are covered by
      a test that triggers them
- [ ] Snapshot tests for AST dumps and rendered diagnostics (golden files)
- [ ] End-to-end tests: every `examples/*.mx` compiles, links, runs, and its
      output is asserted
- [ ] Fuzzing for lexer, preprocessor, parser, and UTF-8, with a seeded corpus
- [x] Sanitizer builds (ASan/UBSan) in CI
- [ ] ThreadSanitizer run for the driver
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
