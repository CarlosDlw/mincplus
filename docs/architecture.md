# Architecture

This document is the contract map for the code that exists today. It answers
three questions for every module: what it owns, what it guarantees to its
callers, and what it is allowed to depend on.

## Layering

```
mincc                      driver: argv -> exit code
  └── minc_driver          cli / help_text / exit_code

minc_support               INTERFACE alias over the support libraries
  ├── minc_source          trusted source text (the validation boundary)
  │     ├── minc_line
  │     ├── minc_span
  │     └── minc_utf8
  ├── minc_diag            diagnostic collection + rendering
  │     ├── minc_source
  │     ├── minc_span
  │     └── minc_utf8
  ├── minc_span            leaves: no support dependencies
  ├── minc_line
  ├── minc_utf8
  ├── minc_mem             Arena
  ├── minc_intern          Interner
  └── (expected)           header-only: Expected / Unexpected / Fallible
```

Rules:

- The dependency graph is acyclic and directed *upward* only. `span`, `line`,
  `utf8`, `mem`, `intern`, and `expected` are leaves.
- `src/support/` is LLVM-free by contract. Only `src/backend/llvm` may include
  `llvm/*`.
- Targets are created with `minc_add_library` / `minc_add_executable`, which
  apply the include dirs, the C++ standard, and the shared warning set. A new
  module is a directory with a three-line `CMakeLists.txt`.

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
- Blocks are allocated aligned and freed with the matching aligned delete.
  `rewind()` keeps the blocks for the next unit; `release()` frees them.

### `intern` — `Interner`

- Index keys are views into a `std::deque` pool, so keys and returned views are
  stable for the interner's lifetime; `clear()` is the only invalidating call.
- `contains` and the lookup path are allocation-free.
- `intern` returns `kInvalidSym` once the `SymId` space is exhausted.

### `driver`

- `cli.h`: `parseArgs` is pure — it never prints, exits, or throws. The first
  positional argument names the subcommand, `--` ends option parsing, `-` is a
  file rather than an option, and one shared command table feeds the parser,
  the error messages, and the help text so they cannot disagree.
- `help_text.h`: ASCII-only output, built from that same table.
- `exit_code.h`: the process contract — `0` success, `1` failure, `2` usage.
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

## Cross-platform guarantees

The same sources build on Linux, macOS, and Windows (Clang, GCC, MSVC), and CI
checks all three. The load-bearing decisions:

- Line terminators (LF/CRLF/CR) are handled in `line`, not by callers.
- Encoding is normalized once in `source`; nothing downstream re-checks it.
- Paths go through `std::filesystem` as UTF-8; `/tmp` is never hard-coded.
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

## Where the next stages plug in

Planned pipeline: `source -> preprocess -> lex -> parse (AST) -> sema -> ir ->
backend -> C interop`, with the driver orchestrating the stages.

- **preprocess** turns a `SourceFile` into a token stream with `#include`
  resolution; it owns file inclusion and macro expansion, and reports through
  `DiagBag` with spans that survive expansion.
- **lex** reads `SourceFile::text` (already trusted UTF-8), emits tokens
  carrying `Span`, and reports through `DiagBag`. It should not re-validate
  encoding or re-derive limits.
- **parse** takes tokens plus an `Arena` for AST nodes.
- **driver** will link `minc_support` when the first real command lands, pick
  `ColorMode` from tty detection, and render `DiagBag` with `DiagRenderer`.
