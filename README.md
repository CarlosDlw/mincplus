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

Scaffold v0.1: the `src/support` foundation and the `mincc` driver shell. No
lexer, parser, or backend yet, so `build`/`run`/`check` parse correctly but
report that they are not implemented. `--help` and `--version` are functional.

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
- `src/driver/` — `mincc` entry point: `cli` (parsing), `help_text` (help and
  version output), `exit_code`. The version header is generated from
  `cmake/version.h.in`; the source tree holds no second copy.
- `src/lex|parse|ast|sema|ir|backend|cinterop/` — planned, not started.
- `tests/unit/` — gtest suites, one per support module.
- `examples/` — `.mx` samples (`001_main_func.mx` is the first e2e target).

Module contracts, ownership, and the dependency graph are documented in
[`docs/architecture.md`](docs/architecture.md).

## Build

Requires CMake 3.28+, Ninja, a C++20 compiler (Clang, GCC, or MSVC), and
GTest. GTest is found through the CMake package config, then `pkg-config`,
then a pinned `FetchContent` download; pass `-DMINC_FETCH_GTEST=OFF` to forbid
the download. ccache is used when present.

```sh
cmake --preset dev      # dev (Debug) | release | ci (warnings as errors)
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

`cmake --preset ci` and `ctest --preset ci` are what CI runs on every platform.

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
  redirected output and non-UTF-8 consoles behave the same everywhere.
