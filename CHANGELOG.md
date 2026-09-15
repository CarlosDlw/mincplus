# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

While the version is `0.x`, a minor version may make an incompatible change to
the language or to the command line; the changelog says so, and so does
`README.md`. The version has one source of truth, `CMakeLists.txt`, and
`mincc --version` prints it.

## [Unreleased]

Nothing yet. Add a line here as you would to a released section, and move it into
a version when it is tagged.

### Added

### Changed

### Fixed

## [0.1.0] — unreleased

The first version, and the one that establishes what an entry here looks like:
what the language does, what the compiler does, and what is deliberately not
there yet.

### Added

**The pipeline, end to end.** A `.mx` file goes in and an executable that runs
comes out: `source → lex → preprocess → parse → lower → validate → resolve →
check → ir → object → link`. Every stage is a library of its own, returns its
errors as values, and links no diagnostics; the driver is the only code that
prints and the only code that decides an exit status.

**The lexer** (`src/lex`). A lossless token stream — every byte of a file belongs
to exactly one token, trivia included — with strict UTF-8 at the boundary, LF,
CRLF and lone-CR line endings, and the five literal forms the language spells.
`mincc lex` is its view.

**The preprocessor** (`src/pp`). Macros (object-like, function-like, variadic),
`#`/`##`, the full conditional set, `#include` with a search path, `#pragma
once`, `#line`, `#error`/`#warning`, and the predefined macros. Every token
remembers where it was *written*, so a mistake inside a macro points at the
`#define`. Every failure mode is bounded: expansion depth, expansion and output
budgets, include depth and count, macro parameters and arguments. `mincc pp` is
its view.

**The parser and the tree** (`src/parse`, `src/syntax`). A recursive-descent
grammar over a token source, emitting events and error values, and a lossless,
hash-consed tree built from them. Errors do not stop the parse: the offending
tokens go under an `Error` node and the tree still covers every byte.

**Lowering, structural validation and name resolution** (`src/ast`,
`src/resolve`). A compact tree for analysis, the structural checks, and two-phase
resolution — file-scope declarations first, then bodies — over C's four
namespaces. Every name use is recorded with the declaration it denotes.
`mincc resolve` is its view.

**Type checking** (`src/sema`). The primitive type names (`i8`…`i128`, `isize`,
`usize`, `f32`, `f64`, `f80`, `bool`, `char`, `str`), the C spellings with the
target's widths, `void` and the bottom type `!`, pointers (`*T`, `*void`,
`&x`, `*p`, `p[i]`, `null`), `let`/`const` with inference and *definite
assignment*, `if`/`else`/`while`/C-style `for`/`break`/`continue`/`return`,
functions with `name: type` parameters and `extern` declarations, variadic
declarations, and the operator set with C's precedence and a specified evaluation
order. The type checker *publishes* what it decided — every conversion, every
access obligation, every operation type — so nothing below it re-derives a rule.

**The LLVM lowering and the backend** (`src/ir`, `src/backend/llvm`). The typed
tree into an `llvm::Module`, with debug information (DWARF on ELF and Mach-O,
CodeView on PE), and the module into an object and a link. The link is done by a
C driver (`clang`, `cc` or `gcc`), because the startup objects, the C runtime and
the platform's linker flags are knowledge a C toolchain already has.

**The driver** (`src/driver`). `mincc` with eight commands — `build`, `run`,
`check`, `lex`, `pp`, `parse`, `resolve`, `ir` — declared as *data*: one table of
options, read by the parser and by the help renderer both, so a name and its
description exist once. The default target is the host; `--target` takes any
triple in the spellings LLVM recognizes.

**The language reference** (`website/`). A Docusaurus site covering the language
and the tools, with every console block copied from a run of the compiler, and
anything unimplemented marked as such.

**The design records** (`docs/`). `docs/architecture.md` for the layout and the
module graph, `docs/roadmap.md` for what is next, and one record per stage in
`docs/architectures/` — including `memory.md`, which states the memory model the
language is being built to, and `never.md`, which argues the bottom type.

**The test suite**. `tests/unit/` (gtest, one suite per module), the examples as
an end-to-end corpus, and four build presets, three of which are gates: `ci`
(warnings as errors), `sanitize` (ASan + UBSan) and the format and clang-tidy
jobs.

### Not yet

The language is missing its *surface*, not its pipeline. Arrays, `struct`,
`union`, `enum`, member access, casts, `sizeof`/`alignof`, `switch`, `do`/`while`,
`goto`, file-scope bindings, `static`, `volatile`, defining a variadic function,
and the standard library. The
[feature checklist](website/docs/language/features.md) on the documentation site
carries the authoritative list, marked *decided* / *planned* / *open*, and each
unimplemented construct is marked on the page that would describe it.

[Unreleased]: https://github.com/CarlosDlw/mincplus/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/CarlosDlw/mincplus/releases/tag/v0.1.0
