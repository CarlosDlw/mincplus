# Architecture

This document is the contract map for the code that exists today. It answers
three questions for every module: what it owns, what it guarantees to its
callers, and what it is allowed to depend on.

## Layering

```
mincc                      driver: argv -> exit code
  └── minc_driver          cli / help_text / error_report / lex_command
        ├── minc_support
        ├── minc_lex
        └── minc_lex_report

minc_lex_report            token flags -> diagnostics
  ├── minc_lex             raw lexer: tokens, lossless stream, dump
  │     ├── minc_span
  │     └── minc_term
  └── minc_diag

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
- `src/support/term` is the only module allowed to contain `#if defined(_WIN32)`
  and `<windows.h>` / `<unistd.h>`. Everything else asks it a yes/no question
  and stays platform-agnostic.
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
| `kMaxArenaAllocation` | 2 GiB | `Arena::newBlock` |

## Cross-platform guarantees

The same sources build on Linux, macOS, and Windows (Clang, GCC, MSVC), and CI
checks all three. The load-bearing decisions:

- Line terminators (LF/CRLF/CR) are handled in `line`, not by callers.
- Encoding is normalized once in `source`; nothing downstream re-checks it.
- Paths go through `std::filesystem` as UTF-8; `/tmp` is never hard-coded.
- Standard input is read in **binary** mode on Windows (`_setmode(_O_BINARY)`),
  so piping a file through `mincc lex -` sees the same bytes as opening it.
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
- `cmake --preset sanitize` builds the whole project with AddressSanitizer and
  UndefinedBehaviorSanitizer, and `-fno-sanitize-recover` makes a finding abort
  the run instead of scrolling past. The preset is one switch rather than
  per-target flags, so a new module cannot be left uninstrumented. CI runs it on
  Linux; MSVC has no UBSan, so the preset reports that instead of
  half-instrumenting.

## Where the next stages plug in

Planned pipeline: `source -> preprocess -> lex -> parse (AST) -> sema -> ir ->
backend -> C interop`, with the driver orchestrating the stages.

- **preprocess** turns a `SourceFile` into a token stream with `#include`
  resolution; it owns file inclusion and macro expansion, and reports through
  `DiagBag` with spans that survive expansion. It runs *before* the lexer and
  therefore owns `#` and every directive; the lexer is what it feeds, and `#`
  outside a directive is not part of the language.
- **lex** (`src/lex`) reads `SourceFile::text` (already trusted UTF-8) and
  produces the token stream. It does not re-validate encoding, re-derive
  limits, or resolve names — it answers "what is here", never "what does it
  mean". Keyword classification is the one thing it does store, because the
  preprocessor must not expand a keyword as a macro name.
- **parse** (`src/parse`) walks the significant-token index and emits a stream
  of **events**, never a node; it links no diagnostics, so a grammar change is
  testable without a `Session`. **`src/syntax`** consumes those events and the
  full token stream into a lossless, untyped **green tree** (arena-backed,
  position-free) with a cursor and a typed AST view. Design record:
  [`architectures/parser.md`](architectures/parser.md). The lexer deliberately
  does not pre-filter trivia; the tree builder is the layer that attaches it.
- **driver** links `minc_support`, `minc_lex`, and `minc_lex_report`, picks
  `ColorMode` per stream with `support/term`, and renders `DiagBag` with
  `DiagRenderer`.
