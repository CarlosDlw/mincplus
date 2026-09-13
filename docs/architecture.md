# Architecture

This document is the contract map for the code that exists today. It answers
three questions for every module: what it owns, what it guarantees to its
callers, and what it is allowed to depend on.

## Layering

```
mincc                      driver: argv -> exit code
  └── minc_driver          cli / help_text / error_report / input_source
        │                  lex / pp / parse / resolve commands
        ├── minc_support
        ├── minc_lex
        ├── minc_lex_report
        ├── minc_pp_report
        ├── minc_syntax
        ├── minc_parse_report
        └── minc_resolve_report

minc_resolve_report        AST and resolve errors -> diagnostics
  ├── minc_resolve         scopes, defs and refs: errors as values, no DiagBag
  │     ├── minc_ast       lowered AST + structural validation: no DiagBag
  │     │     ├── minc_syntax
  │     │     ├── minc_lex
  │     │     ├── minc_intern
  │     │     └── minc_span
  │     ├── minc_intern
  │     ├── minc_span
  │     └── minc_source    to render a definition's location
  └── minc_diag

minc_parse_report          syntax errors -> diagnostics (the only reporter)
  ├── minc_parse
  └── minc_diag

minc_syntax                green tree + cursor + typed AST + dump
  ├── minc_parse           events and errors only: no tree, no diagnostics
  │     ├── minc_lex
  │     └── minc_span
  ├── minc_lex
  ├── minc_span
  └── minc_mem

minc_lex_report            token flags -> diagnostics
  ├── minc_lex             raw lexer: tokens, lossless stream, dump
  │     ├── minc_span
  │     └── minc_term
  └── minc_diag

minc_pp_report             preprocessor errors -> diagnostics
  ├── minc_pp              a client of the lexer: `#`, includes, macro expansion
  │     ├── minc_lex
  │     ├── minc_fs        file identity (device/inode, Windows file index)
  │     └── minc_session
  ├── minc_lex_report      lexical errors of every file the unit read
  └── minc_diag

minc_pp_parse              the adapter that lets the parser read the pp output
  ├── minc_pp
  └── minc_parse

minc_support               INTERFACE alias over the support libraries
  ├── minc_source          trusted source text (the validation boundary)
  │     ├── minc_line
  │     ├── minc_span
  │     └── minc_utf8
  ├── minc_diag            diagnostic collection + rendering
  │     ├── minc_source
  │     ├── minc_span
  │     ├── minc_term
  │     └── minc_utf8
  ├── minc_span            leaves: no support dependencies
  ├── minc_line
  ├── minc_utf8
  ├── minc_term            the only platform-specific code (tty, ANSI/VT)
  ├── minc_mem             Arena
  ├── minc_intern          Interner
  ├── (expected)           header-only: Expected / Unexpected / Fallible
  └── minc_session         per-compilation state
        ├── minc_source      (sources, symbols, diagnostics, node arena)
        ├── minc_diag
        ├── minc_intern
        └── minc_mem
```

Rules:

- The dependency graph is acyclic and directed *upward* only. `span`, `line`,
  `utf8`, `term`, `mem`, `intern`, and `expected` are leaves.
- `src/support/` is LLVM-free by contract. Only `src/backend/llvm` may include
  `llvm/*`.
- `src/lex/` is the *raw* lexer and does not depend on diagnostics at all.
  That is enforced by the build graph (`minc_lex` lists no diag target), not by
  a comment, and it is why the lexer can be tested and fuzzed without a
  `Session`.
- `src/parse/` does not depend on diagnostics **or** on the tree. The parser
  emits events and error *values*; `minc_parse_report` turns those into
  diagnostics and `minc_syntax` turns the events into a tree. So a grammar
  change is testable with neither layer linked, and a change to the tree layout
  cannot reach the grammar.
- `src/ast/` and `src/resolve/` follow the same rule: lowering and structural
  validation are in `minc_ast`, collection and resolution in `minc_resolve`, and
  both return error *values*. `minc_resolve` depends on `minc_ast` and never the
  reverse, and `minc_resolve_report` is the only target on this path that links
  `minc_diag` — so the resolver is testable with no `Session` and no terminal,
  and can be reused by the language server without a `DiagBag` in sight.
- `src/ast/` and `src/resolve/` have **no platform branch**: a file is named by
  `support::FileId`, and the path identity behind it comes from `support`. Unix
  and Windows cannot disagree about which name a program means.
- `src/support/term` is the only module allowed to contain `#if defined(_WIN32)`
  and `<windows.h>` / `<unistd.h>`. Everything else asks it a yes/no question
  and stays platform-agnostic. `src/support/fs` (planned, for file identity and
  canonicalization — see
  [`architectures/preprocessor.md`](architectures/preprocessor.md)) will be the
  second and last such module, for the same reason.
- Targets are created with `minc_add_library` / `minc_add_executable`, which
  apply the include dirs, the C++ standard, and the shared warning set. A new
  module is a directory with a three-line `CMakeLists.txt`.
- Cross-phase state lives only in `Session`. Stages take a `Session&` instead
  of owning or reaching for sources, symbols, or diagnostics.

## Module contracts

### `expected`

- `Expected<T, E>` stores exactly one of `T`/`E` in a `std::variant`. There is
  no state where both are absent, and no default constructor for the
  non-`void` case, so a value is always explicitly a success or a failure.
- Success is implicit (`return value;`); failure must go through
  `makeUnexpected` so the intent is visible at the call site.
- `value()` / `error()` are preconditions. Misuse throws
  `std::bad_variant_access` — that is a bug, not a runtime condition to handle.
- `Fallible<T>` is `Expected<T, std::string>`.

### `span` (+ `span_ops`)

- A `Span` is a half-open `[begin, end)` byte range inside one `FileId`.
- `valid()` means a real file and `begin <= end`. `size()` never underflows: a
  reversed span reports `0` instead of ~4 GiB.
- `Span::at` saturates at `kMaxOffset` rather than wrapping to offset `0`.
- `merge` / `extendToCover` refuse to combine spans from different files
  (invalid result / no-op), so a span never silently spans two sources.

### `line`

- Accepts LF, CRLF, and lone CR. `lineRange` / `lineText` never include the
  terminator, which is what keeps a caret on its token for Windows checkouts.
- `lookup` clamps the offset to `textSize` before mapping.
- Columns are 1-based **byte** columns — the convention `file:line:col`
  consumers expect. Display-column conversion happens only in `diag`.

### `utf8`

- There is exactly one decoder, `decodeOne`, and it is strict: overlong forms,
  surrogate halves, values above U+10FFFF, truncated sequences, and stray
  continuation bytes are all rejected. `firstInvalidOffset` walks the text with
  that same decoder, so "what validation accepts" and "what decoding produces"
  cannot drift.
- `countCodePoints` is total: each malformed byte counts once.
- `bom.h` detects UTF-8/UTF-16/UTF-32 marks (longest first, so UTF-32LE is not
  mistaken for UTF-16LE) and classifies everything but UTF-8 as unsupported.

### `source` — the validation boundary

- `SourceManager` owns every `SourceFile` in a `std::deque`, so addresses and
  the `string_view`s handed out by `SourceFile` stay valid as files are added.
- `addFile` and `loadFromDisk` both return `Fallible<FileId>` and apply the
  same normalization: size limit, BOM handling, NUL rejection, UTF-8
  validation. On failure nothing is stored and the `FileId` space is untouched.
- Invariant every later stage may rely on: a `SourceFile` is valid UTF-8, has
  no embedded NUL, is within `kMaxSourceBytes`, and had a UTF-8 BOM stripped.
- `readFileBytes` is the raw, unnormalized read; it reports missing files,
  directories, oversized files, and short reads rather than returning partial
  input.

### `diag`

- `DiagBag` never prints and never throws. It counts by severity, caps
  retention at `kMaxDiagnostics`, and counts what it dropped so the cap cannot
  hide errors silently.
- `DiagRenderer` is pure formatting: no I/O, no globals. It defaults to
  `ColorMode::Plain`, expands tabs, counts display columns by scalar (not by
  byte) for the caret, clips long lines, and appends the suppressed-diagnostic
  summary in `renderAll`.

### `mem` — `Arena`

- Bump allocation for AST/IR nodes. **Destructors do not run**: only
  trivially-destructible values, or values whose destructor is handled by the
  caller before `rewind()`/`release()`.
- It never throws. `allocate`/`create`/`createArray` return `nullptr` on out of
  memory, on an alignment above `kMaxAlignment`, and on an unsatisfiable size.
- A size above `kMaxArenaAllocation` is refused *before* the allocator is asked.
  What `operator new` does with an impossible size is implementation-defined --
  AddressSanitizer aborts rather than returning null -- so relying on it would
  break the "returns nullptr" contract on some builds and not others.
- Blocks are allocated aligned and freed with the matching aligned delete.
  `rewind()` keeps the blocks for the next unit; `release()` frees them.

### `intern` — `Interner`

- Index keys are views into a `std::deque` pool, so keys and returned views are
  stable for the interner's lifetime; `clear()` is the only invalidating call.
- `contains` and the lookup path are allocation-free.
- `intern` returns `kInvalidSym` once the `SymId` space is exhausted.

### `session`

- `Session` owns one `SourceManager`, `Interner`, `DiagBag`, and `Arena`. It is
  not copyable or movable, so member addresses, and everything they hand out
  (`SourceFile` pointers, interned views), stay stable for its lifetime.
- Stages take `Session&`; nothing else owns cross-phase state.
- `updateFile` is the editor path: it keeps the `FileId` stable, validates the
  new bytes, rebuilds the line table, and bumps `SourceFile::revision`. On
  failure the previous revision is left untouched, so a rejected edit cannot
  corrupt a document the editor still believes is loaded.
- **Revision rule.** A span, token, or offset-keyed diagnostic is only valid
  for the revision it was produced from, because byte offsets do not survive an
  edit. Caches key on `(FileId, revision)` and a bumped revision invalidates
  derived data rather than repairing it.

### `term`

- Owns `ColorMode` (`Plain` / `Ansi`) and the two questions
  `stdoutSupportsColor()` / `stderrSupportsColor()`. Everything that can be
  colored takes a `ColorMode` instead of a `bool`, so "is it colored?" and
  "which escape sequences?" stay one decision with one implementation.
- Color is off when the stream is not a terminal (so pipes and logs stay
  clean), when `NO_COLOR` is set to anything -- including the empty string,
  which is what the convention says -- when `TERM=dumb`, or when the Windows
  console refuses virtual-terminal mode. On Windows the enabling call is
  attempted once, and a failure means plain text, never an error.
- The two environment rules are exposed as pure predicates over the variable's
  value, so they are tested directly instead of by mutating the process
  environment. The answer is stable: asking twice never disagrees.

### `lex` — the raw lexer

Design record: [`docs/architectures/lexer.md`](architectures/lexer.md).

- `lexOne(text, offset)` is a **pure total function**: no `Session`, no
  `SourceManager`, no `Interner`, no `DiagBag`, no allocation. Given the same
  bytes it returns the same token, which is what makes it fuzzable and what
  lets the language server re-lex a suffix of a file.
- It returns **one token per byte range** and never skips anything: whitespace,
  newlines, and comments are tokens too. Trivia cannot be recovered later, and
  a formatter, a highlighter, and doc-comment hover all need it. Consumers that
  only want code filter on `Token::isTrivia()` or walk
  `TokenStream::significantIndices()`.
- A `Token` is 12 bytes and owns no text: the lexeme is
  `text.substr(token.offset, token.length)`. Every offset arithmetic that could
  overflow is done in `uint32` against a source already bounded by
  `kMaxSourceBytes`.
- Malformed input sets a **flag on the token** (`TokenFlag`) rather than
  producing a diagnostic, so one pass reports every problem instead of stopping
  at the first. `lex_report.h` is the only place that knows about spans and
  severity.
- **Resumable by construction, with no state parameter.** A lexer normally
  needs state because it *skips* comments; here trivia is emitted, comments are
  scanned through their terminator, and a string or char literal can never
  cross a line, so every token boundary is also a valid restart point. Adding a
  state parameter is a mechanical change if the rules ever loosen.
- `TokenStream::lex` builds the buffer and **audits the lossless invariant**
  while doing it: tokens tile `[0, size())`, offsets are contiguous, and the
  last token is `EndOfFile` at `size()`. `lossless()` exposes the result so
  tests and any future incremental splice assert it instead of trusting it.
- `dumpTokens` is pure formatting for `mincc lex`. It computes its own columns
  from the data and prints `line:col` by walking the stream, so the table is
  aligned whatever the file holds; it writes nothing and colors nothing that is
  not asked for.
- `#` and `##` are `Hash`/`HashHash` tokens, and a file name in a directive is a
  `HeaderName` -- the one token kind `lexOne` does not produce, because only the
  directive it appears in says it is one. `lex::scanHeaderName` reads it from the
  raw bytes; see `architectures/preprocessor.md` for why tokens cannot express
  it. `lexOne` stays a pure function of `(text, offset)` either way.
- A stream can describe **more than one file**: `fromPreprocessed` carries an
  origin span per token, so the parser's caret points at the header a token was
  written in while the tree is built over the preprocessed text. `spanOf`
  answers for the single-file case, `spanOfAt` for both.

### `pp` — the preprocessor

Design record: [`docs/architectures/preprocessor.md`](architectures/preprocessor.md).

- It is a **client of the lexer**, never a second tokenizer: it walks
  `TokenStream`s and re-lexes a paste with the same `lexOne`. `#` and `##` are
  `Hash` and `HashHash` tokens coming out of the lexer; this stage supplies the
  one thing the lexer cannot — *position* — and reports `pp-stray-hash` when a
  `#` starts no line and a `##` sits outside a macro body.
- Directives are recognised only while the expansion stack is empty -- that is
  literally "this token was written in a file, not produced by a macro".
- The output is the preprocessed **text** plus the tokens that tile it, trivia
  included, because the parser above it is trivia-blind while the tree builder
  is not: one output, two consumers, no way for them to disagree.
- Two tokens are separated by one space exactly when the second does not
  continue the previous bytes verbatim, decided from the two origins. A file
  with no directives therefore preprocesses to bytes identical to its input.
- Every token carries its spelling site, and the expansion chain is a separate
  hash-consed table, so a diagnostic can say "in expansion of macro `X`" and
  point at the header the bytes came from.
- Errors are values, so nothing here prints and the stage links no diagnostics;
  `pp_report` is the only file that knows about `DiagBag`.
- `_Pragma("...")` is registered as a builtin rather than special-cased in the
  scanner, because that is what makes the macro form work: `#define PUSH
  _Pragma("...")` only meets the operator after expansion. It emits no token and
  goes through the same handler as `#pragma`, so the two spellings cannot mean
  different things.
- The result keeps **every file it lexed**, which is the only way a bad byte in
  a header can be reported: no other stage knows the header was opened.
- A leaf's text is a view into the preprocessed text and the node cache is shared
  across inputs, so the text must outlive the cache. That is a lifetime rule, not
  a detail: freeing a result early is a use-after-free the sanitizer preset
  catches.

### `parse` — the grammar

Design record: [`architectures/parser.md`](architectures/parser.md).

- The parser emits **events**, never a node: `Start(kind)` / `Finish` / `Token`
  as a flat `std::vector<Event>`. A separate builder turns them into the tree,
  so the grammar holds no arena, no offsets, and no tree storage, and can be
  tested with a trivial sink.
- Errors are **values**, `ParseError{span, message, code}`, collected in a
  vector. One run therefore reports every syntax error instead of stopping at
  the first. `parse_report.h` is the only file that knows about `Span`,
  severity, and `DiagBag`.
- The code is an enumerator, not a string written at each call site: a closed
  `ParseErrorCode` set with one table holding the code and its name, so the
  parser cannot invent a code no test knows about and two sites cannot spell
  the same condition two ways. `allParseErrorCodes()` is derived from that
  table and a test requires every code to be reachable from some input, which
  is the same guarantee the lexer's flag table gets.
- **Trivia-blind by contract.** A `Token` event means "the next *significant*
  token"; it carries no source index. The builder, which is the only component
  that sees trivia, flushes the whitespace and comments before it. So the
  grammar has no whitespace rules while the tree stays lossless.
- **`forwardParent` is how left-associativity is built left to right.**
  `a + b + c` parses `a + b` first and only then learns a parent exists;
  instead of moving the finished node, its `Start` event records the distance
  to the later `Start` that will adopt it, and the builder enters the parents
  when it gets there.
- **Missing tokens are a zero-width `Token` event** (`missing = true`). A `;`
  slot is a `;` slot whether or not the character is present, so a node's shape
  does not depend on how far the user has typed -- which is exactly the case an
  editor sits in.
- `TokenSource` is an **interface**, not `lex::TokenStream`. The parser today
  reads a file's significant tokens; macro expansion will read a token tree. If
  the grammar talked to the concrete stream, that second source would be a
  rewrite of the grammar instead of a second implementation of one tiny class.
- **Two bounded-work guards, both tested.** `support::kMaxNestingDepth` (1024
  guarded frames, about 250 nesting levels) makes `DepthGuard` refuse to
  descend, so deeply nested input cannot overflow the stack; and after
  `support::kMaxParseErrors` (4096) the parser bails out, wrapping the unparsed
  remainder in one `Error` node, so a pathological file costs bounded work
  instead of a quadratic error cascade. In both cases the tree still covers
  every byte. Every production that can call itself takes a guard -- not just
  the expression entry point, because `parseUnary`, `parseAssign`, and
  `parseConditional` recurse without passing back through it.
- `TokenKind`/`SyntaxKind` share a numeric space: token kinds are exactly their
  `lex::TokenKind` value below `kFirstNodeKind` (256), node kinds sit at or
  above it. Generic tree code -- the dump, the validator, a future highlighter
  -- never needs a special case for a leaf. The gap also means adding token
  kinds never renumbers a node kind that a golden file pins.

### `syntax` — the tree

- **Green tree: immutable, untyped, position-free.** A `GreenNode` is its kind,
  its byte width, and its children -- deliberately no parent and no absolute
  offset, because one green node is shared by every place it appears and so has
  no single parent or position. `SyntaxNode` / `SyntaxToken` (a green pointer
  plus an absolute offset) add both back on the way down; nothing is allocated
  or cached, and identity is `(file, byte range)`, never a pointer.
- **Nodes are hash-consed through `GreenCache`**, so two identical subtrees come
  back as the *same pointer*. That makes the structure a DAG, and it is the
  mechanism a future incremental reparse uses to recognise an unchanged
  subtree without comparing it. The cache lives beside the arena in the tree
  store, so an empty `()` is one node however many functions have one.
- **`TreeStore` is the keyed owner**, and the reason the node cache lives where
  it does: one store per invocation owns the trees keyed by
  `(FileId, revision)` *and* the shared `GreenCache`, so identical subtrees in
  two different files are the same pointer. A newer revision of a file
  replaces its tree instead of being kept beside it. The store takes the
  session's arena by reference rather than owning one, and it cannot live in
  `support` because `support` is syntax-free by contract.
- **Nothing here owns memory.** Green nodes come from the `Session`'s `Arena`
  and leaf text is a view into the `Session`'s source, so a `SyntaxTree` is
  valid exactly as long as its `Session` -- the same lifetime the sources have.
  It carries the source `revision`, because a byte offset is only meaningful
  within one.
- **`stats().lossless` is audited, not asserted.** The builder checks while it
  runs that the leaves tile `[0, size())` exactly and every node's width is the
  sum of its children's; `SyntaxTree::validate()` re-checks it and
  `reconstruct()` is the canonical text. A formatter, a refactor, or a
  language server all rely on that, so it is checked the way the token stream's
  lossless invariant is.
- **Tree walking is iterative, not recursive.** Any walk that could be as deep
  as the parser's guard allows (validate, reconstruct, dump) uses an explicit
  stack, so the tree can never overflow the stack even on input the parser had
  to accept.
- `dumpTree` is pure formatting for `mincc parse`: it returns a string, writes
  nothing, and prints kinds, offsets, and lexemes -- never an address -- so the
  same input gives byte-identical output on every run and every platform. Color
  is computed on the uncolored text, so turning it on cannot shift a column.
- `ast.h` is a **typed view** over the untyped tree: a `SyntaxNode` plus checked
  accessors, every field optional on purpose -- a half-written function has a
  name and no body, and the AST must be able to say so. Shared shape is
  expressed with CRTP rather than a macro, so the compiler checks every member's
  spelling and a reflow cannot cut a line continuation in half. When the node
  count justifies it this layer is generated from one grammar description
  (parser design decision 16), not hand-extended.

### `driver`

- `cli.h`: `parseArgs` is pure — it never prints, exits, or throws. The first
  positional argument names the subcommand, `--` ends option parsing, `-` is a
  file rather than an option, and one shared command table feeds the parser,
  the error messages, and the help text so they cannot disagree. Each row also
  says whether the command is implemented, and the help derives its
  "Implemented"/"Scaffolded" lists from that, so help can never advertise a
  command the dispatch still refuses.
- `error_report.h`: the one way a driver-level error is written. Every
  subcommand uses it, so the prefix, the hint, and the exit code are identical
  whichever command hit the problem. Each function also has a stream-taking
  form, which is what pins the exact text in a test and lets a command that is
  handed its streams report through the same code path.
- `help_text.h`: ASCII-only output, built from that same table.
- `exit_code.h`: the process contract — `0` success, `1` failure, `2` usage.
- `lex_command.h`: the `lex` subcommand. Token dump on stdout, diagnostics on
  stderr, and each stream picks its own `ColorMode`, so a redirected stdout
  stays clean even when stderr is a capable terminal. The body is split into
  `lexInputs()` (the command, with the streams injected) and `runLex()` (the
  choice of streams and colors), so the contract -- which stream carries what,
  in what order, and which exit code -- is tested without spawning a process.
- `parse_command.h`: the `parse` subcommand, built on the same split
  (`parseInputs()` / `runParse()`). It lexes, parses, and builds the tree, then
  prints the tree on stdout and the lexical *and* syntax diagnostics on stderr,
  so `mincc parse` shows exactly the syntax layer and nothing downstream of it.
- `input_source.h`: the one way a file-taking subcommand loads an input.
  `loadInput` handles a path or `-` (standard input) and returns the same
  errors for both, so `lex` and `parse` cannot drift in how they treat `-` or
  in what they say when a file cannot be read.
- The version string comes from `cmake/version.h.in` via CMake; the source
  tree carries no second copy.

## Error model

| Kind | Mechanism |
| --- | --- |
| Recoverable failure | `Fallible<T>` — value or human-readable message |
| Allocation failure | `nullptr` from `Arena` (never an exception) |
| Programming error | A precondition violation (`bad_variant_access`, ignored `nullptr`) |
| Compile diagnostics | `DiagBag`, rendered later by `DiagRenderer` |

Utility code does not throw. Failure messages are lowercase, name the path or
construct involved, and say what was expected.

## Limits

All of these live in `support/limits.h`; nothing re-derives them. The
`static_assert`s there keep the numeric value and its human-readable form in
sync, and keep the source limit strictly below the `uint32` offset ceiling.

| Constant | Value | Enforced by |
| --- | --- | --- |
| `kMaxSourceBytes` | 64 MiB | `SourceManager::addFile`, `readFileBytes` |
| `kMaxOffset` | `0xFFFFFFFF` | `Span` arithmetic |
| `kMaxSourceFiles` | 2^20 | `SourceManager::addFile` |
| `kMaxSymbols` | `0xFFFFFFFF` | `Interner::intern` |
| `kMaxRenderLineCols` | 240 | `DiagRenderer` |
| `kMaxDiagnostics` | 1024 | `DiagBag::add` |
| `kMaxNestingDepth` | 1024 guarded frames | `parse::DepthGuard` |
| `kMaxParseErrors` | 4096 | `Parser::error` (then one bail-out) |
| `kMaxArenaAllocation` | 2 GiB | `Arena::newBlock` |

## Cross-platform guarantees

The same sources build on Linux, macOS, and Windows (Clang, GCC, MSVC), and CI
checks all three. The load-bearing decisions:

- Line terminators (LF/CRLF/CR) are handled in `line`, not by callers.
- Encoding is normalized once in `source`; nothing downstream re-checks it.
- Paths go through `std::filesystem` as UTF-8; `/tmp` is never hard-coded.
- Standard input is read in **binary** mode on Windows (`_setmode(_O_BINARY)`)
  by `driver/input_source`, so piping a file through `mincc lex -` or
  `mincc parse -` sees the same bytes as opening it.
- ANSI color is enabled on Windows by turning on
  `ENABLE_VIRTUAL_TERMINAL_PROCESSING`, and only after `GetConsoleMode`
  succeeds; a console that cannot do it gets plain text rather than escape
  soup. `NO_COLOR` and `TERM=dumb` are honored everywhere.
- MSVC gets `/utf-8` (sources contain UTF-8) and `/Zc:__cplusplus`; warnings are
  `/W4 /permissive-`.
- `.gitattributes` normalizes the tree to LF so a Windows checkout cannot
  produce CRLF-only diffs.
- CLI output is ASCII and color is opt-in, so consoles and pipes agree.

## Testing

- `tests/unit/` has one suite per module, registered by
  `tests/unit/CMakeLists.txt`; GTest is resolved by config, then pkg-config,
  then a pinned `FetchContent` download.
- Tests are discovered in `PRE_TEST` mode so the build step never executes a
  test binary (which is what breaks cross-compilation and Windows CI).
- Regression tests are expected for every fixed bug, and they are expected to
  fail on the pre-fix code.
- The lexer is checked **exhaustively** where the input space is small enough:
  every 1-byte and 2-byte input, and every 3-byte combination of the bytes that
  change scanning. A property test over deterministic byte soup covers longer
  input.
- The examples are held to the parser as well: every `.mx` in `examples/` must
  lex and parse with **zero** diagnostics and a `validate()`d, lossless tree, so
  a grammar change that breaks the documented surface fails a test instead of
  being noticed by hand.
- `cmake --preset sanitize` builds the whole project with AddressSanitizer and
  UndefinedBehaviorSanitizer, and `-fno-sanitize-recover` makes a finding abort
  the run instead of scrolling past. The preset is one switch rather than
  per-target flags, so a new module cannot be left uninstrumented. CI runs it on
  Linux; MSVC has no UBSan, so the preset reports that instead of
  half-instrumenting.

## The pipeline

One order, fixed here so a stage is never inserted in the wrong place and never
forgotten. Each row is one module with one artifact on each side; `[x]` is
shipped, `[ ]` is planned.

```
source (.mx)
  [x] phase 3     lex        one file          -> TokenStream (lossless)
  [x] phase 4     preprocess the unit          -> preprocessed text + stream
  [x]             parse      the token stream  -> event stream + errors
  [x]             syntax     events + tokens   -> lossless green tree, typed view
  [x]             lower      the green tree    -> compact AST
  [x]             validate   the AST           -> the AST, structurally legal
  [x]             resolve    the AST           -> scopes + a symbol per name
  [ ]             sema       the resolved AST  -> typed AST
  [ ]             ir         the typed AST     -> CFG (init / borrow / optimize)
  [ ]             codegen    the IR            -> object file / assembly
  [ ]             link       objects           -> executable
```

Tokenization is phase 3 and directives are phase 4, so the lexer feeds the
preprocessor rather than the other way round, and no stage *after* the
preprocessor reads the source bytes directly: everything above it consumes the
preprocessed stream, which is the only reason a directive is not a syntax error
in the grammar.

**`lower`, `validate` and `resolve` are stages, not part of `sema`.** The order
is forced by the language, not chosen for tidiness. A C-like grammar lets a
file-scope call name a function defined further down, so no body can be checked
before every declaration visible to it exists: name resolution must *finish*
before type checking starts. And the green tree is built for fidelity — trivia,
error nodes, byte-exact reconstruction — which is exactly what the formatter and
the LSP need and the opposite of what analysis wants to walk. Every large
compiler separates these the same way: `rustc` has `rustc_resolve` (two phases:
collect, then resolve) and lowers the AST to HIR before type checking; Roslyn
runs `parse -> declaration table -> bind -> emit`; TypeScript has the binder as
its own pass; Zig has `AstGen -> ZIR` and then `Sema -> AIR`.

**The contract every stage obeys.** A stage receives an artifact and returns an
artifact. It never prints, never exits, and never sees a `DiagBag`: every error
is a *value* with a stable code and a span, and the `*_report` libraries and the
driver are the only code that turns those values into text and into an exit
code. That is what makes a stage testable without a `Session`, fuzzable without
a terminal, and reusable by the language server — which needs `resolve` and
`sema` and never wants a process exit.

The stages through `resolve` are shipped and **wired**: `mincc parse` runs the
whole front end, so a file that begins with `#define` has a syntax tree of its
translation unit rather than a lex error on the `#`, and `mincc resolve` runs
that tree through lowering, validation and name resolution. The four commands
are four views of one pipeline, and each names the others so a reader is never
left guessing which one to reach for:

| Command | Stage | Sees |
| --- | --- | --- |
| `mincc lex` | lexer | one file, raw tokens — a directive's `#` is an ordinary `Hash` token, because `#` is a punctuator of the lexical grammar and only its *meaning* is positional |
| `mincc pp` | lexer + preprocessor | the token stream of the translation unit — macros expanded, includes resolved |
| `mincc parse` | lexer + preprocessor + parser | the syntax tree over the preprocessed stream |
| `mincc resolve` | the front end through name resolution | the lowered AST, the scopes, and every name with the declaration it denotes |

`-D`/`-U`/`-I`/`-isystem` belong to the *front end*, not to one command that
prints it, so all three commands that preprocess accept them and one helper
splits `-DNAME=V` so no two commands can disagree about what it means.
`-isystem` is not a synonym for `-I`: the files it finds are system headers, so
warnings inside them are dropped at the report step while errors are not.

- **preprocess** (`src/pp`) is a **client of the lexer**: it owns `#` and every
  directive, file inclusion, and macro expansion, and it emits the preprocessed
  text and the token stream the parser consumes. `#` outside a directive has no
  meaning, but `#` itself is still a token the lexer produced — a stray one is
  the preprocessor's error to report (`pp-stray-hash`), not a byte the lexer
  refused to classify.
  Every token carries provenance that survives expansion (macro body, argument,
  or invocation site), so a diagnostic can name the macro, the invocation, and
  the include chain; it links no diagnostics and reports errors as values, like
  the lexer and the parser. Design record:
  [`architectures/preprocessor.md`](architectures/preprocessor.md) — including
  the resource budgets that make it total on untrusted input, and the file
  identity rules that make `#pragma once` and include guards correct on
  case-insensitive filesystems.
- **lower** (`src/ast`) turns the green tree into a compact, arena-backed AST
  built for analysis — every node keeping its `(FileId, range)` — and
  **validate** is a pass of its own over that AST for the structural checks the
  parser could not make. Then **resolve** (`src/resolve`) collects every
  declaration into its scope, then resolves every use against scopes that are
  complete by then; it never looks at a type. Design record:
  [`architectures/resolve.md`](architectures/resolve.md) — lowering, structural
  validation, two-phase resolution, the item tree, the scope tables, the error
  codes, the bounds, and the language decisions they depend on. `mincc resolve`
  is the command that proves them.
- **sema** is next, and it is the first stage that consumes a tree where every
  name already denotes a declaration, so nothing in it searches a scope. It is
  about types and nothing else, filtered by the type checklist in `README.md`.
  Lowering, validation and resolution are stages of their own rather than
  bullets inside it, for the reason above; `resolve`'s source→definition map is
  also the layer an editor asks "where is this defined", so it is built here
  rather than bolted on when the LSP arrives.
- **lex** (`src/lex`) reads `SourceFile::text` (already trusted UTF-8) and
  produces the token stream. It does not re-validate encoding, re-derive
  limits, or resolve names — it answers "what is here", never "what does it
  mean". Keyword classification is the one thing it does store, because the
  preprocessor must not expand a keyword as a macro name.
- **parse** (`src/parse`) walks the significant-token index and emits a stream
  of **events**, never a node; it links no diagnostics, so a grammar change is
  testable without a `Session`. **`src/syntax`** consumes those events and the
  full token stream into a lossless, untyped **green tree** (arena-backed,
  position-free) with a cursor and a typed AST view. Both are shipped; design
  record: [`architectures/parser.md`](architectures/parser.md). The lexer
  deliberately does not pre-filter trivia; the tree builder is the layer that
  attaches it.
- **driver** links the support, lex, parse, and syntax libraries, picks
  `ColorMode` per stream with `support/term`, and renders `DiagBag` with
  `DiagRenderer`.
