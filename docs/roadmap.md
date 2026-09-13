# Implementation roadmap

The build order for the compiler itself. The **language** feature checklist —
what `.mx` will actually have — lives in the [README](../README.md#language-features).

Legend: `[x]` done · `[ ]` planned · `[?]` open decision that changes scope.

Design targets: a compiler that runs on Linux, macOS, and Windows (Clang, GCC,
MSVC); an LLVM-based backend kept isolated so the front end never depends on
it; emitted objects following the System V AMD64 ABI and linking with `cc`/`ld`.

The stage order is not re-decided here: it is stated once, in
[`architecture.md#the-pipeline`](architecture.md#the-pipeline), as
`lex` and `preprocess` (phases 3 and 4 of translation) then `parse` -> `lower`
-> `validate` -> `resolve` -> `sema` -> `ir` -> `codegen` -> `link`. The sections
below follow that order, and a stage is added where the pipeline puts it rather
than appended to the end of this file. `lower`, `validate` and `resolve` are
their own stages because name resolution must *finish* before a body can be
type-checked, which is what section 4 is about.

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
- [x] Architecture contract map in [`architecture.md`](architecture.md),
      **including the stage order** ([`#the-pipeline`](architecture.md#the-pipeline)),
      lexer design in [`architectures/lexer.md`](architectures/lexer.md), parser
      and syntax-tree design in [`architectures/parser.md`](architectures/parser.md),
      preprocessor design in
      [`architectures/preprocessor.md`](architectures/preprocessor.md), and the
      lowering/name-resolution design in
      [`architectures/resolve.md`](architectures/resolve.md)
- [x] `Session` — per-compilation state container with per-file revisions, so
      editor edits keep a stable `FileId` while the contents change

## 1. Preprocessor — `src/pp`

Design in
[`architectures/preprocessor.md`](architectures/preprocessor.md): a client of
the lexer that owns `#`, file inclusion and macro expansion, and emits the
preprocessed text plus the token stream. Cost model first: every hazard has an
always-on budget, and provenance survives expansion.

**Shipped**, and wired into the front end: `mincc parse` runs `lex -> preprocess
-> parse`, so a directive is no longer a syntax error.

- [x] `#include` with an include-path search (`-I`), `<>` vs `""`,
      `#include_next`
- [x] `#define` / `#undef`, object- and function-like macros, identical-redefine
      check
- [x] Macro expansion with correct rescanning, the "ineligible for further
      replacement" mark, `##` and `#`
- [x] Variadic macros (`...`, `__VA_ARGS__`) and `__VA_OPT__`
- [x] Conditionals: `#if`/`#ifdef`/`#ifndef`/`#elif`/`#elifdef`/`#else`/`#endif`,
      `defined`
- [x] Constant-expression evaluation for `#if`: 64-bit, no UB, division by zero
      diagnosed, short-circuit branches not evaluated
- [x] `#line`, `#error`, `#warning`, `#pragma` (parsed and preserved, `once`
      honored)
- [x] Predefined macros (`__FILE__`, `__LINE__`, `__COUNTER__`, `__has_include`)
- [x] **Provenance that survives expansion**: `TokenLoc` + a hash-consed
      `ExpansionTable`, so a diagnostic can name the macro, the invocation, and
      the include chain
- [x] **Always-on budgets**, each with a code and a test: include depth,
      conditional nesting, expansion depth, expanded-token count, preprocessed
      bytes, pasted token length, macro parameters, includes per unit
- [x] File identity by `(device, inode)` / Windows file index, so `#pragma once`
      and include guards are right and symlink cycles are reported by name
- [x] Multiple-include optimization for the canonical guard pattern and
      `#pragma once`, asserted equivalent to running without it
- [x] A missing include guard on a twice-read header is diagnosed
- [x] Reproducibility gate: `__DATE__`/`__TIME__` come from `SOURCE_DATE_EPOCH`
      or are an error, never the clock
- [x] The record: directive spans, include graph, branch decisions, and the
      original↔expanded map, for `mincc pp` and the LSP
- [x] `mincc pp` modes: default stream, `--at LINE:COL`, `--defines`,
      `--includes`, `--deps`
- [x] The output is text the parser *and* the tree builder read, with a
      per-token origin, so the tree holds every byte and a caret still points at
      the header a token was written in
- [x] Lexical errors of every file the run read, not just the one named on the
      command line
- [x] `_Pragma("...")`, the operator form of `#pragma`: it produces no token
      and is routed through the same handler as the directive, so
      `_Pragma("once")` elides a second include exactly as `#pragma once` does
- [x] `-isystem`, and the `#pragma GCC system_header` semantics that go with it:
      a search list of its own, and warnings -- never errors -- inside a system
      region dropped at the report step
- [x] `__has_include` answers about the search list without reading the file, so
      a header that exists but cannot be read is reported by the `#include`
      itself instead of silently taking the `#else` branch
- [x] `pp-include-unreadable`: a path that resolved to a file whose bytes cannot
      be used is not the same error as a path nothing resolved
- [ ] Target/ABI predefined macros (`__LP64__`, type widths) — they need the
      type table, so they land with sema
- [x] Differential test against `cc -E` / `clang -E` over a curated macro corpus
      (`tests/unit/pp/differential_test.cc`): 16 standard-C inputs compared token
      for token against the reference, skipped -- not failed -- where no
      reference compiler exists
- [ ] Line splicing (C's phase 2): a `\` at end of line is currently an
      `Invalid` token, so a macro definition cannot continue onto the next line.
      The differential test is what made the cost of [`lexer.md` decision
      4](architectures/lexer.md) visible -- `#define X a \` + newline is how
      macros are ordinarily written, and every C-family preprocessor accepts it.
      Carrying it into the lexer costs a spelling that is no longer a contiguous
      slice of the source, which is why it is its own task rather than a fix
- [ ] Reserved with the decision recorded, not accepted yet: `#embed`
- [ ] `startup/deprecated/overloadable`-style vendor pragmas are parsed and
      preserved, never interpreted
- [ ] A fuzz target over the macro corpus, seeded from the examples

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

## 3. Parser and syntax tree — `src/parse` · `src/syntax`

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
- [x] `TreeStore`, keyed `(FileId, revision)`, owning the shared node cache so
      identical subtrees in two files are one node. It lives in `src/syntax`
      rather than in `Session` because `support` is syntax-free by contract; it
      builds into the session's arena by reference
- [ ] Golden-file tests (`tests/parse/data/*.mx` with expected tree and errors)
- [ ] `if`/`else`, loops, `break`/`continue`, `switch` — when their syntax is
      decided
- [ ] `struct`/`union`/`enum`, typedefs, and the full C declarator grammar
- [ ] Initializers, `sizeof`/`alignof`, casts, and the C-compatible `fn` forms
- [ ] Generated typed AST layer, once the node count justifies the generator
- [ ] Reserved syntax kinds for macro calls, token trees, and attributes
- [ ] Grammar documented next to the code it implements

## 4. AST lowering and name resolution — `src/ast` · `src/resolve`

The stages between the syntax tree and type checking, and the reason they are
stages rather than bullets inside `sema`: a C-like grammar lets a file-scope
call name a function defined further down, so no body can be type-checked before
every declaration visible to it exists. Name resolution has to finish first.
`rustc` gives it a crate of its own (`rustc_resolve`, two phases: collect, then
resolve) and lowers the AST to HIR before type checking; Roslyn runs
`parse -> declaration table -> bind -> emit`; TypeScript has the binder; Zig has
`AstGen -> ZIR` and then `Sema -> AIR`. The order is stated once, in
[`architecture.md#the-pipeline`](architecture.md#the-pipeline).

- [ ] `src/ast` — lowering the lossless green tree into a compact, arena-backed
      AST. Not the same thing as the *typed view* section 3 already has: that is
      a set of accessors that makes the green tree pleasant to traverse, and it
      still carries trivia and error nodes. The green tree exists for *fidelity*
      (trivia, error nodes, byte-exact reconstruction), which is what the
      formatter and the LSP need and what analysis should not have to walk. The
      lowered AST is a separate arena built for analysis, and every node keeps
      the `(FileId, range)` it came from so a diagnostic still lands on the
      user's bytes
- [ ] Structural validation the parser could not do: a duplicate parameter, an
      attribute where it has no meaning, `break` outside a loop. Cheap, and it
      runs before the expensive passes (`rustc` calls it AST validation)
- [ ] `src/resolve` — two phases, deliberately: **collect** every declaration
      into its scope first, then **resolve** each use against scopes that are
      complete by then. One phase cannot work; forward reference is the point
- [ ] Scopes and shadowing rules, with a `SymId` per name interned once, so
      identity is compared instead of spelling
- [ ] Every identifier node records the declaration it denotes, so `sema` never
      searches a scope again
- [ ] Redeclaration and unknown-name errors, plus the warnings that need scopes
      and nothing else (unused entity, shadowing)
- [ ] Caching keyed `(FileId, revision)`, because an editor asks the same
      question on every keystroke
- [ ] `mincc resolve <files...>`: the scopes, and each name with the declaration
      it resolved to
- [x] Design record: [`architectures/resolve.md`](architectures/resolve.md) --
      lowering, structural validation, two-phase resolution, the item tree and
      the scopes, with the open questions that are the language's to answer

## 5. Semantic analysis — `src/sema`

Consumes a **resolved** tree — every name already tied to a declaration — so
nothing here searches a scope.

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

## 6. IR — `src/ir`

- [ ] IR design and textual form `[?]` typed SSA vs. simple three-address
- [ ] Module/type/function/block/value model with an arena-backed builder
- [ ] AST → IR lowering (including `switch`, short-circuit, aggregate copies)
- [ ] IR verifier (types, terminator presence, dominance)
- [ ] Pass manager plus core passes: DCE, mem2reg, constant propagation,
      CFG simplification
- [ ] Debug-info hooks so source locations survive into the backend

## 7. Backend — `src/backend/llvm` (isolated)

- [ ] LLVM target initialization for AMD64; target machine and data layout
- [ ] IR → LLVM IR translation
- [ ] Object emission (`.o`) and assembly output (`--emit=asm`)
- [ ] Optimization pipelines for `-O0`..`-O3` and size
- [ ] Symbol visibility, sections, and relocations matching the ABI
- [ ] `[?]` Whether a hand-written AMD64 codegen is in scope at all
- [ ] `[?]` Whether non-AMD64 targets are ever planned

## 8. C interoperability — `src/cinterop`

- [ ] System V AMD64 argument classification (INTEGER/SSE/MEMORY) and returns
- [ ] Aggregates by value: struct passing/returning, unions, alignments
- [ ] Variadic calls (`va_list` conventions); bitfields `[?]`
- [ ] Calling into C: `extern` declarations resolved against real libc
- [ ] Being called from C: exported symbols with C linkage
- [ ] Emit/consume `.o` and `.a`; drive `cc`/`ld` for the final link
- [ ] `[?]` Whether to parse C headers directly or require declaration blocks
- [ ] Interop test suite that links against libc in both directions

## 9. Driver — `src/driver`

- [ ] `check`: run lex/parse/sema, render `DiagBag`, no codegen
- [ ] `build`: full pipeline → `.o` → link; `-o`, multiple inputs
- [ ] `run`: build then execute, forwarding program arguments after `--`
- [ ] Common flags: `-I`, `-D`, `-O`, `--emit`, `--target`
- [ ] Response files (`@file`) for long command lines
- [ ] `--color` with tty detection and `NO_COLOR`; wire `DiagRenderer` colors
- [ ] `-ferror-limit` and a "too many errors" path using `kMaxDiagnostics`
- [ ] `--version` printing version, host, and target triple
- [ ] `[?]` Incremental compilation / on-disk cache

## 10. Language extras

- [ ] `[?]` Fix the extension list with the language checklist
- [ ] Specify each extra: syntax, semantics, and C-interop interaction
- [ ] Reject extensions cleanly when a C-compatible mode is requested
- [ ] Language reference documenting every extension with rationale

## 11. Runtime and standard library

- [ ] `[?]` Reuse libc only, or ship a small bundled runtime/stdlib
- [ ] Entry-point handling and startup (`main`, CRT assumptions)
- [ ] Headers for interop and for the extras that need library support
- [ ] Decide what "minimal C" guarantees beyond the C standard subset

## 12. Quality

- [x] Unit tests per shipped module (support, lexer, parser, syntax tree,
      driver); preprocessor, sema, and IR suites land with those stages
- [x] Negative tests: every lexical flag code and every parse error code has a
      test that triggers it, and a sweep fails if a code becomes unreachable
- [ ] Snapshot tests for AST dumps and rendered diagnostics (golden files)
- [ ] End-to-end tests: every `examples/*.mx` compiles, links, runs, and its
      output is asserted
- [ ] Fuzzing for lexer, preprocessor, parser, and UTF-8, with a seeded corpus
- [x] Sanitizer builds (ASan/UBSan) in CI
- [ ] ThreadSanitizer run for the driver
- [ ] Coverage reporting and compile-time/memory benchmarks
- [ ] Cross-platform CI extended from `support` to the whole pipeline

## 13. Release and maintenance

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
