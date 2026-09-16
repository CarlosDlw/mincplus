# Implementation roadmap

The build order for the compiler itself. The **language** feature checklist —
what `.mx` will actually have — lives on the documentation site, at
[`website/docs/language/features.md`](../website/docs/language/features.md).

Legend: `[x]` done · `[ ]` planned · `[?]` open decision that changes scope.

## Where this stands

Every stage of the pipeline exists and runs: § 0–§ 7 and § 9 are implemented, and
the unchecked items inside them are the ones that wait on the **language
surface** rather than on the pipeline — aggregates, casts, `sizeof`, the C
declarator grammar, `switch`. § 8 (C interoperability) is the one stage
deliberately not started: it needs the C type model on this side of the boundary,
which is what the remaining items in § 3 and § 5 produce. § 10–§ 13 are the work
after that.

The order below is still the order. A stage is added where the pipeline puts it,
not appended to the end of this file.

Design targets: a compiler that runs on Linux, macOS, and Windows (Clang, GCC,
MSVC); an LLVM-based backend kept isolated so the front end never depends on
it; and targets named by **LLVM triple**, so *what* the compiler emits for is a
property of the triple rather than a table of two names kept by hand. Objects
link with `cc`/`ld`, or `link.exe` on Windows.

The stage order is not re-decided here: it is stated once, in
[`architecture.md#the-pipeline`](architecture.md#the-pipeline), as
`lex` and `preprocess` (phases 3 and 4 of translation) then `parse` -> `lower`
-> `validate` -> `resolve` -> `sema` -> `ir` -> `codegen` -> `link`. The sections
below follow that order, and a stage is added where the pipeline puts it rather
than appended to the end of this file. `lower`, `validate` and `resolve` are
their own stages because name resolution must *finish* before a body can be
type-checked, which is what section 4 is about.

## 0. Foundation

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
      [`architectures/preprocessor.md`](architectures/preprocessor.md), the
      lowering/name-resolution design in
      [`architectures/resolve.md`](architectures/resolve.md), the type-checking
      design in [`architectures/sema.md`](architectures/sema.md), and the
      memory model in [`architectures/memory.md`](architectures/memory.md) —
      whose stage-one surface (raw pointers) is now implemented
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
- [x] Digit separators, `_` and C23's `'`, claimed into the token and legal only
      between two digits of the same run (`1_000`, `0xFE'DC'BA'98`, `1e1_0`);
      `1000_`, `1__0`, `0x_FF` and `10_u8` are `lex-misplaced-separator`, one
      token and one sentence rather than a number and a name
- [x] Floating literals: decimal and hex floats, `e`/`p` exponents only when
      digits follow; a hex float may omit the exponent (`0x1.8`) or the integer
      part (`0x.8p3`), and a trailing point never starts one (`5.` is `5`, `.`)
- [x] Character and string literals with escape scanning; `''`, unknown
      escapes, and unterminated literals flagged
- [x] The escape alphabet: the C controls and punctuation, GCC's `\e`, octal
      (`\101`, `\o{101}`), hex (`\x41`, `\x{41}`), `\uXXXX`/`\UXXXXXXXX`/
      `\u{...}`, and `\` + a line ending as a continuation (LF and CRLF are one
      rule). Ownership is decided: an escape above a byte is the *string*
      reader's refusal and `lex-escape-too-wide`, a code point that is not a
      character is `lex-escape-out-of-range`, `\N{...}` is `lex-named-escape`,
      and a character literal's width is the checker's
      (`sema-literal-out-of-range`), so one mistake gets one sentence
- [x] Operators and punctuation with longest match
- [x] Comments `//` and `/* */` (non-nesting), retained as trivia
- [x] Trivia (whitespace, LF/CRLF/CR, comments) retained as ordinary tokens,
      with a significant-token index for the parser
- [x] Lexical errors reported as diagnostics with `file:line:col`, code, and
      caret, one per problem, in a single pass
- [x] Token dump for inspection (`mincc lex <files...>`)
- [ ] Identifiers interned through `Interner` (`SymId`) — happens where symbols
      are wanted, not in the token
- [x] Literal suffixes, as a **closed table** in `support/consteval`: a trailing
      run of identifier bytes is claimed into the token only when the run is a
      known suffix, so `10u8` is one token, `10z` is two and `1else` is still `1`
      and `else`; an unknown suffix adjacent to a literal is
      `parse-invalid-literal-suffix`
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
- [x] Parameter lists, written `name: type` — the `let` shape, and only that;
      the C order `type name` is rejected by name rather than guessed at, because
      a forgotten name would silently become a parameter (see
      [`architectures/parser.md#ambiguity-policy`](architectures/parser.md#ambiguity-policy))
- [x] Statements and blocks
- [x] `if` / `else` / `else if` — condition without parentheses (and accepts
      them), each arm a block, so the dangling-`else` ambiguity cannot arise
- [x] `while` and C-style `for` (`for init; cond; step`, parentheses optional;
      any clause may be empty, so `for ;;` is an infinite loop)
- [x] `break` / `continue`
- [x] `TreeStore`, keyed `(FileId, revision)`, owning the shared node cache so
      identical subtrees in two files are one node. It lives in `src/syntax`
      rather than in `Session` because `support` is syntax-free by contract; it
      builds into the session's arena by reference
- [ ] Golden-file tests (`tests/parse/data/*.mx` with expected tree and errors)
- [ ] `switch` — when its syntax is decided
- [ ] `struct`/`union`/`enum`, typedefs, and the full C declarator grammar
- [ ] C declarator forms still absent: `sizeof`/`alignof`, and the
      C-compatible `fn` declaration spellings
- [x] **Casts**, in three spellings with one meaning (`as T`, `(T)x`, and typed
      literal suffixes): the conversion matrix over every pair the language has,
      `(T)x` made unambiguous by **reserving the type names** (rather than by a
      typedef table in the parser, so a future user-defined type is `as`-only),
      the suffixes C has plus the language's own type names (`10u8`, `12f`,
      `1.5L`, with `long`/`long double` resolved per target), no
      reinterpretation in a cast (the bits get a name of their own), and
      **float → integer as a guarded trap** rather than LLVM's poison or Rust's
      silent saturation —
      ([`architectures/casts.md`](architectures/casts.md)): the grammar, the
      matrix, the lowering, the flags (`-Wcast`, `-Wprovenance`) and the tests
      that walk every ordered pair of the type universe are in, and
      `examples/017_casts.mx` is the runnable page
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

**Shipped.** `mincc resolve` runs the front end through this stage
(`lex -> preprocess -> parse -> lower -> validate -> resolve`), and
`mincc check` runs it and then type-checks.

- [x] `src/ast` — lowering the lossless green tree into a compact, arena-backed
      AST. Not the same thing as the *typed view* section 3 already has: that is
      a set of accessors that makes the green tree pleasant to traverse, and it
      still carries trivia and error nodes. The green tree exists for *fidelity*
      (trivia, error nodes, byte-exact reconstruction), which is what the
      formatter and the LSP need and what analysis should not have to walk. The
      lowered AST is a separate arena built for analysis, and every node keeps
      the `(FileId, range)` it came from so a diagnostic still lands on the
      user's bytes
- [x] Structural validation the parser could not do: a `let`/`const` with no
      type and no initializer, a `const` with a type but no value (a binding that
      can never be read), and the node budget. Each has a code and a test. It is
      short on purpose and grows with the syntax, not ahead of it: a rule lands
      when a real input can reach it, never as a placeholder that can never fire
      (`rustc` calls this pass AST validation)
- [x] `src/resolve` — two phases, deliberately: **collect** every declaration
      into its scope first, then **resolve** each use against scopes that are
      complete by then. One phase cannot work; forward reference is the point
- [x] Scopes and shadowing rules, with a `SymId` per name interned once, so
      identity is compared instead of spelling
- [x] Every name use records the declaration it denotes, so `sema` never
      searches a scope again; an unresolved use carries a *reason*
      (`not-found` / `wrong-namespace` / `error-region`) instead of being a hole
- [x] Redeclaration and unknown-name errors, plus the warnings that need scopes
      and nothing else (unused entity, shadowing), each with a stable code
- [x] The **item tree**: signatures as written, with bodies stored beside it, so
      an edit inside a body invalidates that body and nothing else — the
      invariant that makes an editor cheap
- [x] `source_to_def` — the syntax-node→def and offset→def mapping, so
      `--at FILE:LINE:COL` is go-to-definition, built here rather than bolted on
      later for the LSP
- [x] Caching keyed `(FileId, revision)`, because an editor asks the same
      question on every keystroke; reuse is asserted by a test, not assumed
- [x] `mincc resolve <files...>`: the lowered tree (`--ast`), the scopes on
      stdout, each name with the declaration it resolved to (`--refs`), and only
      the unresolved uses with their reasons (`--unresolved`)
- [x] Every budget always on and checked before the allocation (defs, scopes,
      refs, scope depth, walk depth); the body walk is iterative, so deep input
      is a diagnostic rather than a stack overflow
- [x] Language decisions A–F recorded in
      [`architectures/resolve.md`](architectures/resolve.md#decisions-the-language-owns)
      and in the [language feature checklist](../website/docs/language/features.md):
      order-independent file scope, the outer binding in a `let` initializer, no
      nested `fn`, shadowing allowed and warned under `-Wshadow`, `goto`/labels
      and visibility deferred with their name spaces reserved
- [x] Design record: [`architectures/resolve.md`](architectures/resolve.md) --
      lowering, structural validation, two-phase resolution, the item tree and
      the scopes, with the decisions above

## 5. Semantic analysis — `src/sema`

Consumes a **resolved** tree — every name already tied to a declaration — so
nothing here searches a scope — and returns a **typed** AST: every expression
carries a type, types are interned values, and C's conversions are implemented
once. Design in [`architectures/sema.md`](architectures/sema.md): the type
model, the deferred literal types, the conversion sites, the type-specifier
grammar, the error codes (with the list of which stage owns which error), and
the language decisions this stage had to make — `void`, conditions requiring
`bool`, implicit narrowing at assignment, `str` not being arithmetic.

**Shipped**, for every form the grammar produces today. What is left in this
section is the type and analysis work the *syntax* does not exist for yet
(aggregates, `sizeof`/`alignof`, and the checked layer of the memory model). The
flow analyses are here and not in the
IR, and that is a decision and not a convenience: definite assignment,
reachability and `break`/`continue` context are all answered exactly by the
*shape* of this grammar, so deferring them would have meant a later stage
re-deriving a fact this one already owns — see
[`architectures/sema.md#definite-assignment`](architectures/sema.md#definite-assignment),
which also records the reversal.

- [x] Design record: [`architectures/sema.md`](architectures/sema.md)
- [x] The type model: an interned, hash-consed `TypeId` with structural
      identity, so `i32`, `int` and `signed int` are one type; built-ins with
      stable ids; `Error` as a real poison type so a failure never cascades
- [x] The typed AST as a parallel array beside the lowered tree, so `src/ast`
      never depends on the type language and the tree stays a value — plus a
      compilation-wide `Context` that owns the type store and answers the same
      question for the same revision with the same artifact
- [x] The type-specifier grammar over the `Type` identifier run, with the
      **target ABI** supplying `long`/`long double` widths — never the host's
      `#ifdef`s — including the `uint` and `__int128` shorthands and the C
      spellings of `char`
- [x] Conversions: integer promotions and C17 6.3.1.8 usual arithmetic
      conversions, assignment conversion with `-Wconversion` implemented and off
      by default, and the rule that a condition must be `bool`
- [x] Deferred literal typing: context decides, `i32`/`f64` is the default, and
      a literal that does not fit its type is an error rather than a silent
      truncation (a value past 64 bits is accepted only where the context can
      hold it)
- [x] Lvalue/modifiable-lvalue rules, so assignment and `++`/`--` to a `const`
      or to a non-lvalue are errors — including through parentheses
- [x] Function checking: return type, `return;` vs `return expr;`,
      `sema-missing-return`, and `main` being `fn i32 main()`
- [x] `sema-division-by-zero` for a constant operand, and the constant-arithmetic
      core extracted to `support` (`support/consteval`) so `#if` and sema cannot
      disagree about integer arithmetic — the preprocessor's evaluator now
      calls it too
- [x] **The artifact publishes what the lowering may not re-derive**
      (`src/sema/coerce.cc`): every implicit conversion recorded as a pair keyed
      on the consumer that applies it (`TypedFile::coercions()`, and
      `--ast` prints the table), and the operation type of a compound assignment
      (`ExprInfo::opType`, printed as `[op=i32]`) — the fact the tree cannot
      show, since `x <<= 9` on a `u16` is typed `u16` and operates at `i32`
- [x] **No deferred literal type leaves the stage**: decided at the seam where
      the context is known, then swept down the tree so an operand the context
      reaches through an operation is decided too (`1 + 2.0` in an `f64` binding
      is two `f64`s). A literal the reached context cannot hold is an error
      however deep the reach (`let y: u8 = 300 / 3` is refused at the `300`),
      with a negation read as a negation (`-128` in an `i8` is legal)
- [x] `mincc check`: the stage's command, with `--ast` printing the typed tree
      and the conversions it recorded, `--types` the type table, `--target` the
      ABI, and `-Wconversion` the narrowing lint
- [x] The example corpus type-checks clean, as a test and through
      `make examples`

- [x] Type system: arrays `[N]T`, slices `[]T`, and the cast matrix over every
      pair the language has are **in** — the layout each one needs (size,
      alignment, the pointee's stepping) is answered by `TypeStore`, so no
      second place computes a size
- [ ] Type system, what is left: qualifiers (`const`/`volatile` on the pointee),
      function-pointer types, and aggregates — the primitive set and the
      C-compatible spellings are done (`i8`..`i128`, `u8`..`u128`, `f32`/`f64`/`f80`,
      `bool`, `char`, `str`, `void`, `int`/`long`/`long long int`/… per target ABI,
      with `char` fixed unsigned rather than inheriting C's sign)
- [x] Fixed-size arrays **designed**: `[N]T` with the count in the type, no
      decay (ever), value semantics, no VLA and no zero-length or flexible
      arrays, the context-typed `[...]` and the complete `T{...}` as the two
      literal forms with `[_]` as the only count inference (outermost), the
      element's *complete* size and alignment as the layout rule,
      `a[i]` as an access with a statically known extent (a constant index out of
      range is a diagnostic, not a trap), the stack-object limit, and C's array
      parameter mapped to `*T` at the boundary —
      ([`architectures/arrays.md`](architectures/arrays.md)); `[]T` is reserved
      for slices and parses with a sentence
- [x] Fixed-size arrays **implemented** (steps 1–9 of
      [`architectures/arrays.md`](architectures/arrays.md)): the type, its
      interning with the count in the identity, `sizeOf`/`alignOf`, the syntax
      run in the grammar and in the checker, element access with the constant
      bounds check, the by-value shape (caller copy + pointer, `sret`), both
      literal forms — `[1, 2, 3]` typed by its context, `[3]i32{1, 2, 3}`
      complete, `[_]u8{...}` with the count from the elements, `[64]u8{0; 64}`
      as a splat that is never expanded — and **file scope too**: the aggregate
      value record (a list or a splat, recursive) and the constant `ir` emits
      for it, so `const TABLE = [_]i32{...};` is read like any other object.
      Proven end to end: `mincc run` on a program that fills, copies, passes and
      indexes arrays, at file scope and inside a body
- [x] The two array **bounds**, in `ir` where the cost is the compiler's own: a
      frame object over `kMaxStackObjectBytes` and a non-zero fill over
      `kMaxFillElements` are each refused once, by name, with the fix in the
      sentence (decisions 27, 28 of
      [`architectures/arrays.md`](architectures/arrays.md)) — while a zero fill
      of any count stays legal, because it is one constant
- [x] The **boundary rule** for arrays: an `extern` declaration with a `[N]T`
      parameter or return is refused by name, because an array crosses this
      language's functions by value and no outside ABI promises that shape —
      while `*[N]T` crosses freely (decision 11 of
      [`architectures/arrays.md`](architectures/arrays.md)); mapping C's own
      `T a[N]` parameter to `*T` is `cinterop`'s, when that module lands
- [x] The **reserved spellings** (step 10 of
      [`architectures/arrays.md`](architectures/arrays.md)): `[]T` and `..` each
      parse and are refused by name, so no `.mx` file can mean anything else by
      either before slices land — and the lexer gained the one character of
      lookbehind that makes `..` lexable at all (`a[1..2]` is two dots; `.5` is
      still a float)
- [x] **Raw pointers** (stage one of
      [`architectures/memory.md`](architectures/memory.md)): `*T` at any depth,
      `&x` on a **modifiable** lvalue, `*p` and `p[i]` as places, element-scaled
      `p + n` / `p - n` / `p1 - p2` / `++p` / `--p` / `p += n`, comparison and
      ordering, `null` as the empty `*void`, and `*void` as the untyped pointer
      that converts to every other one. `sema` publishes one
      `AccessObligation` per dereference (`Object` or `Foreign` provenance) for
      the lowering to materialise rather than re-derive
- [x] **Slices**: the record is
      [`architectures/slices.md`](architectures/slices.md), and it answers the
      question `arrays.md` decision 17 left open (a slice does *not* need
      `struct` first — arrays already landed the aggregate-value machinery; what
      waits for `cinterop` is the promise, not the mechanism). `[]T` is a
      `{ptr, len}` descriptor, a **view** with no capacity and no literal, taken
      with `..` from an array, a slice or a pointer, with its own index `0`, a
      length that is a `usize` word, and an `extern` refusal the array shares.
      Landed: the type and its layout (two words, the pointer's width twice),
      the four forms (`a[l..r]`, `a[l..]`, `a[..r]`, `a[..]`) with the pointer
      required to write both, the constant bounds (`a[0..5]` and `a[3..1]` each
      with their own sentence), `s[i]` as a place, the view crossing a call **by
      value** (`{ ptr, i64 }` in the signature, where an array crosses as a copy
      and an `sret`), the two-member debug type, `examples/016_slices.mx`, and the
      site page. The two reserved spellings are gone — `parse-reserved-range` no
      longer exists, and `[]T` is a type rather than a sentence.
      **Deferred, and not gaps in the view:** `len(x)` and `sizeof(x)` are each a
      form the grammar has to have — a call whose argument is a place, and a type
      in an expression — and each is its own item below; until they land a view's
      extent is passed beside it, exactly as a view of a pointer always required
- [ ] The **reading operators**: `len(x)` (a constant for an array, a word for a
      view, one spelling for both — `architectures/slices.md` decision 15) and the
      grammar family `sizeof` / `alignof` / `static_assert`, which take a *type*
      in an expression. `len` is what a view still needs to be usable without
      passing its extent beside it, and `sizeof([]i32)` is how the descriptor's two
      words become askable from a program
- [ ] The rest of the memory model: `restrict`, `volatile`/`unaligned`
      accesses, the owning `slice<T>` / `&T` / `&mut T` layer, and the
      checked-build traps — the model is **decided**; what is left is the syntax
      it needs and the `src/ir` that enforces it. The `expose` /
      `with_exposed_provenance` pair **is implemented as the cast that carries its
      name** (`p as usize`, `addr as *u8`), counted by `-Wprovenance`; whether
      those two also get a call-like spelling of their own is the open half
- [x] Control-flow typing: `if`/`while`/`for` conditions must be `bool`, and
      `break`/`continue` outside a loop are `sema-break-outside-loop` /
      `sema-continue-outside-loop`. `terminates()` handles the branch and loop
      shapes, so `fn i32 f() { if c { return 1; } else { return 2; } }` has no
      missing-return and `while true {}` without a `break` does not either
- [x] **Definite assignment** (`src/sema/check_flow.cc`): a `let` with no
      initializer is not a value until an assignment reaches the read on every
      path — `sema-use-before-assignment`, an error. The rules are JLS 16's,
      applied to this grammar (a conditional merges by intersection, a loop is
      analyzed from its entry state, and a loop whose condition is constantly
      `true` is left only by its own `break`s), so it is exact rather than
      conservative and needs no CFG. One diagnostic per binding
- [x] **Integer edges**: a constant that does not fit its type, and a shift count
      outside the width, are errors; division/remainder by zero is already one.
      Signed and unsigned overflow are *defined* to wrap, and the lowering must
      not emit `nsw`/`nuw`; the runtime contract for `/0`, `%0`, `INT_MIN / -1`
      and an out-of-range count is a trap, not a poison value
- [ ] Control-flow checks that still want a CFG: `goto` targets, and
      unreachable-code precision inside loops (the per-block warning is exact
      for the statement after a `return`, less so after a `break`)
- [x] `extern fn Type Name(...);` — the **declaration** form: a function defined
      elsewhere, with no body, whose identity is shared with its definition — and
      `extern fn Type Name(first: T, ...);` for the variadic ones
      ([`architectures/extern.md`](architectures/extern.md)). `extern` is a
      declaration word and not a storage class, which is why the rest of this
      item is still open
- [x] The bottom type `!`: `fn ! name(...)` never returns to its caller. The type
      converts into every other type, so a call to one can be an argument, an
      assigned value or the arm of a `?:`, and it makes the code after it
      unreachable. The body is *proved* divergent — a reachable `return` or a body
      that can reach its end is an error — and the promise is never inferred. The
      lowering maps it to `void` and derives LLVM's `noreturn` from the return
      type ([`architectures/never.md`](architectures/never.md),
      `examples/012_never.mx`)
- [x] File-scope bindings **designed**: `let` and `const` at the top of a unit,
      `extern let`/`extern const` declarations, a **constant-only initializer**
      evaluated in dependency order (so no dynamic initialization exists and the
      static initialization order fiasco is unrepresentable), zero-initialization
      for an uninitialized `let`, **external linkage for both `let` and `const`**
      — visibility is the module system's decision and not a linkage default —
      with `static` narrowing either one to internal
      ([`architectures/globals.md`](architectures/globals.md));
      `website/docs/language/variables.md#file-scope`
- [x] File-scope bindings **implemented**: the file-scope item (the annotation ends
      the item's span and the initializer is its `body`, so the editor invariant
      holds), the ICE checker with its value record (dependency order, a cycle
      reported with its chain, one sentence per binding, in source order), and the
      `declareGlobals()` pass in `src/ir` — every object emitted `global` and never
      `constant`, which the assumption scan then keeps true (decision 33 of §6).
      `examples/013_file_scope.mx`, `tests/unit/sema/global_test.cc`,
      `tests/unit/ir/global_test.cc`
- [x] Storage classes and linkage: `static` (which narrows a file-scope `let` or
      `const` to internal linkage — and means nothing at block scope, where there
      is one storage duration)
- [ ] Tentative definitions (C's `i32 x;`) and thread-local storage
- [ ] Symbol table exported for the backend and C interop
- [ ] Warning set: sign/conversion issues beyond `-Wconversion`, and the rest of
      the lints (each with a code and a test)

## 6. IR — `src/ir`

**The IR is LLVM's.** This is a decision and not a deferral, and it replaces the
"typed SSA vs. simple three-address" question this section used to open with:
the front end hands over a typed tree, `ir` lowers it to LLVM IR, and that is
where the CFG comes from, where the optimizers are, and where the cross-platform
claim stops being a promise. The alternative was an IR of our own plus a hand
written backend, which is two large pieces to get right before a program can run
at all, and neither of them is something this project would do better than LLVM.
The design record is [`architectures/ir.md`](architectures/ir.md).

- [x] Design record: [`docs/architectures/ir.md`](architectures/ir.md)
- [x] **`sema`: publish what the lowering is not allowed to re-derive** — every
      implicit conversion as an explicit record keyed on its consumer
      (`TypedFile::coercions()`, `src/sema/coerce.cc`) and the *operation type*
      of a compound assignment (`ExprInfo::opType`). `u16 <<= 9` is defined at
      `i32` while the tree names only `u16`, so this was the blocker: a lowering
      reading the tree's type would emit an out-of-range shift — a poison value,
      not a crash. Shipped with the guarantee the record needs, that **no node
      of the artifact carries a deferred literal type** (decided at the seam,
      then swept down the tree), and with the enumeration test over every pair
      `convertible` permits
- [x] **`sema`: publish the access record** — one `AccessObligation` per
      dereference (`TypedFile::accesses()`, `accessAt(node)`, `src/sema/access.cc`)
      carrying the accessed type (its size *and* its alignment), the access kind
      and the provenance the stage can prove (`object`/`foreign`). This is the
      memory half of the item above and the same rule: an alignment re-derived in
      the lowering is a second copy of the layout table, and an overestimated
      LLVM `align` is undefined behaviour rather than slow code
- [x] Lowering of the typed tree: functions, parameters, calls, `if`/`else`,
      `while`, `for`, `break`/`continue`, and the operators `sema` typed
- [x] The runtime contract `sema`'s integer table imposes, honoured rather than
      inherited: **no `nsw`/`nuw`** on arithmetic the language defines to wrap,
      and an explicit test plus trap for `/0`, `%0`, `INT_MIN / -1` and an
      out-of-range shift count, where LLVM gives poison instead. One file
      (`src/ir/runtime.cc`), so the guard is at the only place these opcodes
      appear and cannot be forgotten by a construct added later
- [x] The **access record** materialised: `*p` and `p[i]` lowering through
      `TypedFile::accessAt`, the access's alignment read from the obligation and
      stated explicitly on every `load`/`store`, `getelementptr` **without
      `inbounds`**, and a node with no recorded obligation refused as
      `ir-missing-obligation` — an ICE, since a program that type-checked cannot
      be missing one
- [x] The **assumption list**, closed and scanned: no `!tbaa`/`!alias.scope`/
      `!noalias` metadata, no `noalias` but from a written `restrict`, no
      `inbounds` without a recorded proof (today: none at all), no `nsw`/`nuw`,
      no `dereferenceable`/`nonnull`/`noundef`/`range`, no fast-math flags, no
      `undef`/`poison`, no file-scope object emitted `constant`, and no alignment
      — an access's or an object's — that is not the one its type gives.
      Enumerated in `invariants.cc` in the shape of `sema`'s `allAccessKinds()`,
      one row per checked rule with the code it reports, and **every row is
      tripped** by `tests/unit/ir/invariants_test.cc` on a module this compiler
      built and then broke by hand — so a row without a test input fails there,
      and a check that stopped running is a failing test rather than a comment
- [x] The **target layout against LLVM's own**, as a mechanism and not a promise:
      `Lowering::layoutOf` compares `sema`'s `sizeOf`/`alignOf` against the
      target's `DataLayout` the first time a type enters a module, and a
      disagreement is an `ir-internal` naming the type, both numbers and the
      target, with no module emitted. Written by the bug it prevents: i386 aligns
      a 64-bit value to four bytes and gives the x87 format a **twelve**-byte
      object, where the table said eight and sixteen — so every `i64`, `f64` and
      `f80` on that target carried an alignment the scan refused, and an `f80`
      object was four bytes longer than the type LLVM emitted. `TargetInfo` now
      states `int64AlignBits`/`float64AlignBits`/`float80AlignBits` per row, and
      `tests/unit/ir/layout_test.cc` walks every named triple (plus a doctored row
      that proves the refusal fires); the same pass fixed `long double` on
      `x86_64-apple-darwin`, which is the x87 format and not a `double`
- [ ] The checked build's **module-statable guards** (null dereference,
      misalignment, an `object`-provenance extent) behind `-fcheck`, which `-O0`
      defaults to — explicitly *not* the semantic guards, which belong in every
      build. The shadow-memory half is § 11's runtime
- [x] Signedness from the *type* and not the opcode: `i32` and `u32` are one LLVM
      type, so `/`, `%`, `>>` and the comparisons pick `sdiv`/`udiv`,
      `ashr`/`lshr` and `sgt`/`ugt` from what `sema` recorded -- the difference
      between a correct lowering and a silent miscompile
- [x] Strict left-to-right evaluation of operands and argument lists, which the
      language specifies and the lowering therefore has to produce
- [x] `str` as opaque `ptr`, one private global per **distinct spelling**: the
      language needed no pointer *type* for the IR to have one, and now that `&x`
      makes addresses observable the deduplication is a contract rather than an
      optimisation
- [x] A verifier pass after lowering, so a mistake in this stage is a diagnostic
      here and not a miscompile two stages down
- [x] `mincc ir` and its dump: the module as text, with the diagnostics of this
      stage rendered through the same machinery every other stage uses
- [x] Debug-info hooks so source locations survive into the backend: under
      `-g` the lowering builds the `DICompileUnit`, a `DISubprogram` per function,
      a `DILocation` per instruction and a `#dbg_declare` per binding, and the
      assumption scan's rule became a **permit-list** (no metadata, except
      `!dbg`) because the two rules contradicted each other and nothing caught
      it. `tests/unit/ir/debug_test.cc` asserts that `-g` changes no instruction
      -- strip the debug metadata and the module is byte-identical -- and that no
      `llvm.dbg.*` intrinsic appears, since records and intrinsics cannot be
      mixed in one module. The emission half is § 7
- [x] `include/sema/target.h`'s two-name enum became a **triple**: the identity
      is now the canonical LLVM spelling (`x86_64-unknown-linux-gnu`), which is
      the string `codegen` has to hand to a `TargetMachine` anyway, so there is
      no second spelling of one target. The ABI facts (`long`, `long double`,
      the pointer width for `isize`/`usize`) are derived **by rule from the
      components** -- LP64/ILP32 on the Unices, 32 on Windows, x87 on System V,
      `double` on Darwin, binary128 on AArch64/RISC-V -- and a triple the table
      does not state, an architecture it does not name, or a shape that is
      ambiguous (`x86_64-linux-gnu` is `arch-os-env`, not `arch-vendor-os`) is
      **refused with a sentence** instead of silently defaulted

## 7. Codegen — `src/backend/llvm` (isolated)

The record is [`architectures/codegen.md`](architectures/codegen.md), and its
governor is the promise the whole project exists to make: **if the checker lets
it pass, it must run.** That is why the stage has no semantic refusal in it and
why its failures are enumerated by *class* — the environment, the program, or
this compiler.

**Shipped**, and wired into the driver: `mincc build` carries a unit through
`ir -> codegen -> link`, and `mincc run` is the same path with an `exec` at the
end. What is left in this section is not the stage but its *proof*: the failure
sweep below, and the triple matrix.

- [x] Design record: [`docs/architectures/codegen.md`](architectures/codegen.md)
- [x] LLVM initialization (the full target/`AsmPrinter` set, which is more than
      `ir` needs) and target selection from the triple: target machine, and the
      data layout the module already carries
- [x] Object emission (`.o`) and assembly output (`--emit=asm`), through
      `addPassesToEmitFile` with its inverted boolean wrapped once and its
      `DisableVerify=true` default overridden
- [x] Optimization pipelines for `-O0`..`-O3`, `-Os` and `-Oz` — the **new**
      pass manager for the middle end, and LLVM's **legacy** one for codegen,
      which is LLVM's own split and not an accident to fix
- [x] **Position-independent code on ELF and Mach-O, from a per-triple table** —
      measured, not assumed: this host's `cc` defaults to `-pie`, and the
      default relocation model links into `DT_TEXTREL` (a warning here, an
      error on other linkers and architectures). `Static` on COFF, `PIC_`
      everywhere else, and a platform the table does not name is a refusal
      rather than a default
- [x] The relocation model, code model and CPU/features stated per triple, never
      inherited from the host or from an LLVM default: the CPU and the feature
      set are empty strings and not `"native"`, so an object never depends on
      the machine that happened to compile it
- [x] No platform branch on the host: `src/backend` contains no `#if` at all,
      and every target fact it needs (`reloc`, `code model`, the object suffix)
      arrives from the triple. Symbol visibility and sections are LLVM's default
      for that target machine, which is what a triple is for
- [x] The **failure table**, one code per class
      (`codegen-target-unavailable`, `-emit-unsupported`, `-linker-not-found`,
      `-linker-unavailable`, `-link-failed`, `-object-write-failed`, `-internal`)
      in `src/backend/llvm/diagnostics.cc`, with `allDiagnosticCodes()` derived
      from the table so a code without a name is a failing test rather than a
      printed number
- [ ] The failure table's **per-code test input and unreachability sweep**, in
      the shape of every earlier stage's enumeration: today the table is proven
      complete and the *inputs* for the hardest rows (a missing linker driver,
      a target this LLVM cannot generate code for, a link that fails) are
      covered by the driver's tests, but a sweep that fails when a code becomes
      unreachable is section 12's work and not done
- [x] ~~`[?]` Which LLVM: the distribution's shared library, or a pinned version
      built once~~ — the distribution's (22.1.8 here), and § *The three hosts* in
      the record is what a contributor needs installed; linking `lld` in as a
      library to drop the C-toolchain dependency is recorded as a real option
      and expressly not taken
- [ ] A triple matrix that is exercised and not assumed: cross-compiling from
      any host in the design targets to the others. Two directions are proven
      today -- `--target aarch64-unknown-linux-gnu --emit obj` from this host
      writes a real object with no C toolchain involved (a driver test asserts
      it), and the sweep over every example x every stated triple found no crash
      -- and the matrix as a whole is not: windows-x64 COFF, riscv64 and darwin
      objects are produced but never handed to a linker or inspected for shape
- [x] ~~`[?]` Whether a hand-written AMD64 codegen is in scope at all~~ — **no**,
      and the question is closed: the IR is LLVM's, so a hand-written backend
      would be a second implementation of what LLVM is better at
- [x] ~~`[?]` Whether non-AMD64 targets are ever planned~~ — **yes**, and it is
      the point: one triple is all the lowering needs to know

## 8. C interoperability — `src/cinterop`

Note what this section is *not*: the driver already hands objects to a real C
linker and the compiler already emits nothing of its own for a calling
convention, because the convention is LLVM's to emit from the triple. The
*declaration* half has also landed: `extern fn` names a symbol defined elsewhere
and the linker resolves it, so `mincc run` already calls libc
([`architectures/extern.md`](architectures/extern.md),
`examples/010_extern.mx`). What remains is everything that needs the C **type**
model on this side of the boundary — the declarator grammar, aggregates by
value, and the headers — which is why the language-surface items in § 3 and § 5
come first.

- [ ] Argument classification and returns per the target's ABI (System V AMD64
      and Windows x64 first): INTEGER/SSE/MEMORY, aggregates by value, and the
      alignments that follow from the triple
- [ ] Aggregates by value: struct passing/returning, unions, alignments
- [x] Variadic calls, for the scalar and pointer types the language has:
      `extern fn i32 printf(fmt: str, ...);` declares, calls promote their extra
      arguments the way the ABI does, and `examples/011_variadics.mx` runs
      ([`architectures/extern.md`](architectures/extern.md))
- [ ] Reading variadic arguments (`va_start`/`va_arg`, or the builtin that would
      replace them), which is what a variadic *definition* needs; and bitfields `[?]`
**Shipped**: the **builtin system** — one `constexpr` table in its own module
(`include/builtins`, depending on `support` alone) where a row is the identity, the
spelling, the *class* of the spelling, a closed signature (families resolved per
target), an effect, the lowering as data, a status and a sentence. Five stages read
it — `resolve` binds the names, `sema` types the call, `ir` lowers it, `mincc
builtins` prints it and the reference page is generated from it — and no stage may
match a builtin by its spelling, which a source scan enforces
([`architectures/builtins.md`](architectures/builtins.md)). The two classes are the
part that makes the surface safe: `clz`/`ctz`/`popcount`/`bswap`/`rotl`/`rotr` are
ordinary names the language binds in the file scope (`true`'s treatment: shadowable
in a block, not redeclarable there) while `__builtin_*` cannot be declared or
`#define`d at all.

- [x] The table, the identity and the totality tests: one row per id, a sentence per
      row, a hole that has an argument to fill it, a source scan over `src/` and
      `include/`
- [x] The reserved namespace, in both places a name can be taken: a declaration
      (`resolve-reserved-identifier`) and a `#define` (`pp-reserved-identifier`)
- [x] Binding, typing and lowering, with a builtin's wrong count and wrong argument
      producing the same codes and the same sentences a function's call does
- [x] The bit operations on every integer width — including `isize`/`usize` at the
      target's width — with the answers the language defines (`clz(0)` is the width;
      a rotate's count is modulo the width), proven by running programs
- [x] `__builtin_trap()`, typed `!`, so a body that ends in it keeps its return type
- [x] The differential test: every intrinsic row's declaration attributes are compared
      against `Intrinsic::getAttributes`, and every row's effect against LLVM's own
      memory property — the test that fails the day LLVM changes its mind
- [ ] The families that are not rows: `sizeof`, `alignof`, `static_assert` (grammar —
      they take a type), `__builtin_{add,sub,mul}_overflow` (out-parameters), and
      `offsetof` behind `struct`
- [ ] The target/feature availability field, which arrives with the first row that
      needs one (the shape is decided in the record; inventing the field before a row
      can set it would be a field no test can pin)
- [x] Calling into C: a declaration resolved against real libc, for the scalar
      and pointer types the language has today (`extern fn i32 puts(s: str);`,
      and `examples/010_extern.mx` runs)
- [ ] The C type model behind it: `long`/`unsigned` spellings that already
      resolve, aggregate parameters, and the declarator forms a header uses
- [ ] Being called from C: exported symbols with C linkage
- [x] Emit `.o`, and link objects into an executable through the C driver —
      shipped as `mincc build`/`run` (§ 9), including `-L`/`-l` and `--sysroot`
- [ ] Consume `.o` and `.a` as inputs: linking a `.o` this compiler did not
      produce, and building an archive
- [ ] `[?]` Whether to parse C headers directly or require declaration blocks
- [ ] Interop test suite that links against libc in both directions

## 9. Driver — `src/driver`

- [x] `check`: the whole front end through type checking, `DiagBag` rendered,
      no codegen — `--ast`, `--types`, `--target`, `-Wunused`/`-Wshadow`,
      `-Wconversion`

**Shipped**: `build` and `run` are one function with a boolean between them, so
the two cannot disagree about what the pipeline means.

- [x] `build`: full pipeline → `.o` → link through a C driver
      (`clang`→`cc`→`gcc`, overridable with `--linker`); `-o`, multiple inputs,
      `-O`, `-g`, `--emit=exe|obj|asm`, `-L`/`-l`, `-v` to print the `argv`. The
      linker is driven with an `argv` array and never a shell string, and the
      driver is discovered with the platform's own rules (`PATHEXT` on Windows)
- [x] `run`: **`build` into a temporary executable plus `exec`**, not an
      in-process JIT — one code path with `build`, process isolation (a crashing
      program must not take the compiler with it), argument forwarding after
      `--` interpreted by nobody, and the program's own exit status (a signal
      death reported as a non-zero status, since naming the signal is platform
      code this module may not contain)
- [ ] `run`'s **`LLJIT` differential oracle in `tests/`**: in process, running a
      program is an *advantage* — no linker, no process per case, and a crash is
      a failing test rather than a dead compiler — which is exactly why it
      belongs there and not in the command. Today the driver's tests build and
      execute the program instead
- [ ] No-entry-point refused by the compiler before the link, in its own words,
      because three linkers spell that failure three unhelpful ways. Measured
      today: `fn i32 f() { return 1; }` reaches the linker and the user reads
      `undefined reference to 'main'` in the linker's own language
- [x] Common flags: `-O`, `--emit`; `-I`, `-D`, `-U`, `-isystem` and `--target`
      are shared by `build` and `run` — the request both commands build comes
      from one function, so `run` cannot accept an option `build` refuses or
      default one differently
- [x] Response files (`@file`) for long command lines: expanded before the
      parse (`driver/response_file`), so the words in the file are the words on
      the line. Quoting, an escape that leaves a Windows path alone, nesting with
      a depth bound, a cycle refused by name, and `-h` still outranking an
      unreadable file
- [x] tty detection and `NO_COLOR` (and `TERM=dumb`) in `support/term`, with the
      renderer's colors wired to it per stream, so a pipe or a log file gets
      plain text and no command has to ask
- [x] The `--color` **override flag** (`auto`/`always`/`never`). The precedence
      is stated once and tested: `never` beats everything, `always` beats
      `NO_COLOR` and `TERM=dumb`, and `auto` is the detection above
- [x] `-ferror-limit=N`: shows at most `N` errors, counts the rest, and is spent
      **across the inputs of one command line** rather than once per file (the
      bag is cleared per translation unit, so the counter lives in the caller).
      A display bound and not a work bound: every input is still compiled, and a
      suppressed error still fails the build
- [x] `-vV` printing version, host, default target triple and the LLVM version
      compiled against — the block a bug report needs. `-V` alone stays one line,
      because that is what a script parses
- [x] **The default target is the host** (`kHostTriple`, generated by CMake), so
      `build` links natively on every platform instead of refusing for `--linker`
      and `--sysroot` off Linux; the native/cross decision asks `sameAbi` instead
      of comparing triple text; and the architecture aliases LLVM itself
      recognizes (`arm64`, `amd64`, `i686`) parse to the canonical row
- [x] **The command line as data** (`driver/command_spec.*`), read by the parser
      and by the help renderer both, with a test that walks the table in both
      directions: every option a page lists is accepted, and every option the
      parser accepts is on a page. The class of bug it kills is the hand-written
      help beside the hand-written parser, drifting one option at a time
- [x] **Help per command** (`driver/help_render.*`, `driver/help_text.*`): all six
      spellings (`mincc`, `-h`, `--help`, `help`, `help <cmd>`, `<cmd> --help`),
      a page that is grouped and width-aware (`COLUMNS` → `ioctl`/console → 80)
      rather than pre-aligned, and `did you mean` for a misspelled command or
      option (`driver/suggest.*`). An option passed to a command that does not
      take it names the command that does, and points at its page
- [x] Reference for all of it written down in `docs/architectures/cli.md`
- [ ] `[?]` Incremental compilation / on-disk cache

## 10. Language extras

- [ ] **Module system and imports** `[?]`: constants, functions and top-level
      types importable by name from other `.mx` files. The constraints it must
      honor, the seams today's code keeps open for it, and the questions it has
      to answer (granularity, visibility default, cycles, reference syntax,
      where the interface lives, and how C symbols stay flat) are recorded in
      [`architectures/modules.md`](architectures/modules.md) — written *before*
      the feature on purpose, because C++20 modules changed every exported
      function's mangled symbol and that is not a detail a parser decides late
- [ ] Top-level types: `struct`, `enum`, and type aliases, with **nominal**
      identity across modules (`architectures/modules.md`, seam S4 — the type
      store interns by structure today, which is right for scalars and wrong for
      an aggregate two modules each define)
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

- [x] Unit tests per shipped module — `support`, `lex`, `pp`, `parse`,
      `syntax`, `ast`, `resolve`, `sema` and `ir` each have a suite, and the
      driver's tests are where `backend`, `build` and `run` are proven (they
      are the stage's only consumer, and a test that links a program is the
      only test that proves the link)
- [x] Negative tests: every lexical flag code and every parse error code has a
      test that triggers it, and a sweep fails if a code becomes unreachable
- [ ] Snapshot tests for AST dumps and rendered diagnostics (golden files)
- [ ] End-to-end tests: every `examples/*.mx` compiles, links, runs, and its
      output is asserted. `make examples` walks the corpus through `lex`,
      `parse`, `resolve`, `check`, `ir` and `ir -g`, which is the front-end
      half; no example is built into an executable and compared against its
      expected output, and there is no corpus for a program that *does* something
      (it cannot be: see the language checklist — no `extern` declaration, so no
      `printf`)
- [ ] Fuzzing for lexer, preprocessor, parser, and UTF-8, with a seeded corpus
- [x] Sanitizer builds (ASan/UBSan) in CI
- [ ] ThreadSanitizer run for the driver
- [ ] Coverage reporting and compile-time/memory benchmarks
- [x] Cross-platform CI extended from `support` to the whole pipeline: the
      `build-test` job configures, builds and runs the whole suite on Linux and
      macOS, `build-test-windows` does the same on Windows under MSYS2's
      MinGW-w64 toolchain (LLVM's prebuilt Windows packages ship no CMake package
      or libraries to link against, so a development tree has to come from
      somewhere else), and `sanitize`, `format`, `tidy` and `docs` are separate
      jobs. The first run on a real runner found four tests that assumed a Linux
      x86-64 host and two more that assumed a Unix one, which is the whole point
      of running it
- [ ] The MSVC configuration exercised in CI: `cmake/minc_cxx.cmake` has flags
      for it and no job builds with it, because an LLVM for Windows that ships a
      CMake package *and* matches this project's build configuration is not
      available as a binary. Building LLVM from source on a runner is the price
      of asking, and it is its own decision
- [x] Install and pin LLVM in CI rather than relying on the runner image having
      it: `find_package(LLVM CONFIG REQUIRED)` means a runner without LLVM
      development files fails at *configure*, which is a red CI run that says
      nothing about the change. One composite action
      (`.github/actions/install-llvm`) installs the pinned release on Linux and
      macOS and exports `MINC_LLVM_ROOT` — the single variable every preset reads
      — along with the names of the matching clang tools, so the version and the
      path are stated once and the gates run the same tools a developer does.
      Windows gets its development tree from MSYS2's MinGW-w64 packages instead,
      and the job that uses it says why

## 13. Release and maintenance

- [x] Versioning policy and `CHANGELOG.md`: SemVer, with the `0.x` allowance for
      an incompatible change documented on both sides; the version is
      single-sourced in `CMakeLists.txt` and printed by `mincc --version`; the
      changelog is in [Keep a Changelog](https://keepachangelog.com) form
- [ ] Packaging: install layout, `mincc` on `PATH`, tarballs/installers
- [ ] Reproducible builds
- [x] `LICENSE` (MIT), `CONTRIBUTING.md`, `SECURITY.md`, `SUPPORT.md`,
      `CODE_OF_CONDUCT.md`, the issue and pull-request templates, and Dependabot
      for the two ecosystems that have dependencies
- [ ] Release automation: tag → CI artifacts
- [x] Docs: the language reference as a site (`website/`, built by `make docs`
      and checked by a CI job), whose `tools/cli` page is the CLI reference. The
      **C-interop guide** is § 8's and arrives with the C type model

## Definition of done

The compiler is "complete" for this project when:

- every example compiles, links, and runs on Linux, macOS, and Windows;
- interop tests both call into libc and are called *from* C, on all three
  hosts;
- every diagnostic has a code, a test, and a documented meaning;
- the pipeline runs clean under ASan/UBSan in CI and under `-Werror` with
  GCC, Clang, and MSVC;
- the language reference, CLI reference, and interop guide are published.
