# Lexer design

Decision record for the lexer. It captures what production lexers actually do,
what that implies for `minc+`, and which choices are still open.

## The "two passes" intuition is not the real split

Lexers do not scan the source twice. Every production lexer here is a
**single forward pass** over the bytes. What is really layered is *who owns
what*:

- `rustc_lexer` is deliberately pure: it works on `&str`, produces tokens that
  are a *type tag plus a size*, never copies text, does not intern, does not
  report errors, and does not know about spans. Errors are stored as **flags on
  the token**. A second layer, `rustc_parse::lexer`, turns those into the "wide
  tokens" the parser consumes, and that is where spans, interned symbols, and
  diagnostics appear.
- Clang's lexer is forward-only and owns no file reading or buffering. It
  tracks facts like `IsAtStartOfLine`, `HasLeadingSpace`, and
  `HasLeadingEmptyMacro`, and it can optionally return comments and whitespace
  as tokens for clients that want every character ("raw mode", whitespace
  retention).
- Roslyn keeps a *full-fidelity* tree: whitespace, comments, and preprocessor
  directives are retained as trivia, and the source is reconstructible.
- rust-analyzer goes further with lossless trees whose nodes carry no
  positions and no parents; the original text is recovered by concatenating
  token texts, and parser errors live *outside* the tree.

So the accurate statement is: **one pass over characters, several layers of
ownership**, and *lossless retention* as a separate axis. The token stream
either keeps trivia or it does not, and that choice is irreversible.

There is a second, real sense of "two phases" inside one token: scan the
lexeme, then interpret it (validate escapes, check digit bases, decide if a
number overflows). rustc keeps those separate on purpose — recognition stays in
the lexer, value checks happen later.

## What that implies for `minc+`

The language is C-family, needs a real preprocessor, must interoperate with C
bit-for-bit, and is expected to grow a language server. Those constraints pick
the design:

### 1. Layer 0 — snapshot (exists)

`SourceManager` owns the bytes, validates them once (size, BOM, NUL, UTF-8),
and provides the line table. `SourceFile::revision` marks the snapshot: byte
offsets are only meaningful within one revision, because inserting a byte at
the front shifts every later offset.

### 2. Layer 1 — raw lexer: pure, resumable, trivia-complete

Signature shape:

```cpp
// Pure: no SourceManager, no Interner, no DiagBag, no allocations.
LexResult lexOne(std::string_view text, std::uint32_t offset, LexerState state);
```

- **Input**: bytes, a start offset, and a small resumable state.
- **Output**: one token as `{kind, length}` plus the next state; malformed
  input sets **flags on the token** instead of emitting a diagnostic.
- **No text copies, ever.** The lexeme is sliced from the snapshot by offset.
- **Resumable** because the only stateful constructs are block comments and
  line splicing. Being able to start mid-file in a correct state is what makes
  incremental re-lexing for the LSP trivial later, and it costs almost nothing
  to design in now. Retrofitting it is expensive.

### 3. Layer 2 — token recording: lossless buffer, indexed

The recorder walks the raw lexer over a whole `SourceFile` and stores:

- a contiguous `TokenBuffer` covering **every byte** of the file, including
  whitespace, newlines, and comments;
- a separate index of non-trivia tokens for the parser;
- interned `SymId`s for identifiers, so the parser compares integers;
- keyword classification, done here or in the lexer (see decisions).

The invariant, which is directly testable:

> Concatenating the text of every token in order reproduces the file byte for
> byte, and the last token is `EndOfFile` at `size()`.

Skipping trivia at lex time cannot be undone; it is exactly what makes a lexer
useless for formatting, semantic highlighting, and doc-comment hover. So trivia
is always produced and the parser filters it.

### 4. Layer 3 — preprocessor: a token rewriter

Directives and macro expansion run on the token stream, not on characters, and
produce a new token stream with spans that map back to the original spelling.
It owns `#include`, conditionals, and macro expansion, and it must never see a
keyword as a macro name (which is a C rule, and the reason keywords are
classified during lexing).

### 5. `Session` holds the derived tables

`support/session/session.h` is the container for state that outlives one phase:
sources, interner, diagnostics, arena, and — once the lexer exists — the
per-file token buffers keyed by `(FileId, revision)`.

## Why hand-written instead of a generator

`lex`/`flex`/`re2c` produce table-driven DFAs. They are a poor fit here:

- C-family lexing is context-sensitive in places the DFA cannot see: the
  header-name form inside `#include <...>`, line splicing, numeric suffixes,
  string prefixes, and the difference between a `.` and a `...`.
- Error recovery quality is a stated goal, and mixing generated DFAs with
  hand-written recovery is the maintainability complaint that shows up
  repeatedly in the literature.
- Clang and rustc are both hand-written, and the usual argument for a
  generated lexer (it is faster to write) does not hold for a language whose
  lexical grammar is small but irregular.

Decision: **hand-written, direct-coded, forward-only.**

## What we retain on purpose

| Retained | Why |
| --- | --- |
| Every byte, as trivia | Formatting, semantic tokens, doc comments, lossless round-trip |
| Raw spelling and interpreted value as separate facts | `"a\n"` is six bytes and a 2-byte value; diagnostics need the spelling |
| Error flags on tokens | One pass reports every lexical error; no early bail-out |
| Resumable lexer state | Incremental re-lexing for the LSP |
| File revision on every cached table | Offsets are not stable across edits |
| Trivia attachment question deferred | See below |

## Decisions still open

| # | Question | Options |
| --- | --- | --- |
| 1 | Where keywords are recognised | In the raw lexer (keeps the stream parse-ready, needs the keyword table there) vs. in the recorder (keeps the raw lexer minimal, like `rustc_lexer`) |
| 2 | Whether `SymId` lives in the token | Token stays 12 bytes and symbols live in a parallel array, vs. a 16-byte token with the `SymId` inline |
| 3 | Trivia model | Explicit trivia tokens (rust-analyzer) vs. leading/trailing trivia attached to tokens (Roslyn, Swift) vs. a token linked list (Dart) |
| 4 | Line splicing | Support `\` at end of line, or reject it with a diagnostic |
| 5 | Trigraphs and digraphs | Reject with a diagnostic, or implement for C compatibility |
| 6 | Logical vs. physical positions | Whether diagnostics report positions after line splicing, which needs a second mapping |
| 7 | Raw/multiline strings | Whether `.mx` gets them, and their scanning rules |
| 8 | Red/green trees | Deferred to the parser stage; the lexer only has to stay lossless so the option remains open |

## Non-goals for the lexer

- No type information, no name resolution, no semantic checks — those are sema.
- No source reading or buffering — that is `SourceManager`.
- No macro expansion — that is the preprocessor.
- No tree building — that is the parser.

## References

- `rustc_lexer` crate overview — pure lexing split from spans, interning, and
  error reporting: <https://doc.rust-lang.org/beta/nightly-rustc/rustc_lexer/index.html>
- Clang `Lexer` — forward-only lexing, raw mode, whitespace retention, token
  flags: <https://github.com/llvm/llvm-project/blob/main/clang/include/clang/Lex/Lexer.h>
- Roslyn red/green trees — immutable position-free nodes, sharing, caching:
  <https://github.com/dotnet/roslyn/blob/main/docs/compilers/Design/Red-Green%20Trees.md>
- rust-analyzer syntax — lossless, semantic-less trees; errors outside the
  tree; trivia discussion: <https://github.com/rust-lang/rust-analyzer/blob/master/docs/book/src/contributing/syntax.md>
- Lexer design notes — spans bound to a snapshot, longest match, the warning
  that skipping whitespace breaks losslessness: <https://doc.liz6.com/en/compilers/01-lexical-analysis/02-lexer-design>
