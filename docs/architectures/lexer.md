# Lexer design

Decision record for `src/lex`. It captures what production lexers actually do,
what that implies for `minc+`, and every choice that is now fixed — so the next
person can tell a decision from an accident.

The code this describes: `include/lex/` (public), `src/lex/` (implementation),
`tests/unit/lex/` (spec).

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
  as tokens for clients that want every character ("raw mode").
- Roslyn keeps a *full-fidelity* tree: whitespace, comments, and preprocessor
  directives are retained as trivia, and the source is reconstructible.
- rust-analyzer goes further with lossless trees whose nodes carry no positions
  and no parents; the original text is recovered by concatenating token texts,
  and parser errors live *outside* the tree.

So the accurate statement is: **one pass over characters, several layers of
ownership**, and *lossless retention* as a separate axis. The token stream
either keeps trivia or it does not, and that choice is irreversible.

There is a second, real sense of "two phases" inside one token: scan the
lexeme, then interpret it (validate escapes, check digit bases, decide if a
number overflows). rustc keeps those separate on purpose, and so does this
lexer: recognition stays here, value checks happen later.

## Layers in `minc+`

### 1. Snapshot — `support/source` (exists)

`SourceManager` owns the bytes, validates them once (size, BOM, NUL, UTF-8),
and provides the line table. `SourceFile::revision` marks the snapshot: byte
offsets are only meaningful within one revision.

### 2. Raw lexer — `src/lex/lexer.cc` (exists)

```cpp
// Pure: no SourceManager, no Interner, no DiagBag, no allocations.
Token lexOne(std::string_view text, std::uint32_t offset);
```

There is **no `LexerState` parameter, and none is needed**. A lexer usually
needs resumable state because it *skips* comments: a lexer that discards a
block comment must remember it is inside one if it restarts mid-file. Here
trivia is emitted — comments are scanned through their terminator and returned
as ordinary tokens — and a string or char literal can never cross a line. Every
token boundary is therefore also a valid restart point, and the signature stays
honest. The consequence is directly tested: lexing from any token boundary
reproduces the token the whole-file pass produced
(`TokenStreamTest.AnyTokenBoundaryIsAValidRestartPoint`).

Loosening any of those rules (multi-line or raw strings, line splicing, nesting
block comments) is exactly when a state parameter must come back. The signature
is shaped so adding one is mechanical.

Contract:

- **Total.** `offset >= text.size()` returns `EndOfFile`; otherwise the token
  has `length >= 1`, so a caller looping from 0 always progresses and always
  terminates exactly at the end.
- **No copies.** The lexeme is `text.substr(token.offset, token.length)`.
- **No diagnostics.** Problems are flags on the token.
- **No semantics.** There is no "type name" or "declaration keyword" kind.

### 3. Token stream — `src/lex/token_stream.cc` (exists)

`TokenStream::lex` walks `lexOne` over the whole file and stores a contiguous
buffer covering **every byte**, plus an index of the non-trivia tokens for the
parser. It audits the invariant while building:

> Concatenating the text of every token in order reproduces the file byte for
> byte, the first token starts at 0, each token starts where the previous one
> ended, and the last is `EndOfFile` at `size()`.

`lossless()` exposes the result. It is not a comment: the audit runs on every
lex, and a deterministic pseudo-random byte-soup test asserts it for 2000
inputs on every platform with no extra tooling.

### 4. Preprocessor — `src/lex/pp` (planned)

Directives and macro expansion belong here, on the token stream, producing a new
stream with spans that map back to the original spelling. It owns `#include`,
conditionals, and macro expansion, and it must never see a keyword as a macro
name — which is the C rule and the reason keywords are classified during lexing.

### 5. `Session` holds the derived tables

`support/session/session.h` owns sources, interner, diagnostics, and arena. Once
incremental analysis lands, the per-file token buffers are keyed by
`(FileId, revision)`, which is why `SourceFile::revision` exists.

## Why hand-written instead of a generator

`lex`/`flex`/`re2c` produce table-driven DFAs. They are a poor fit here:

- C-family lexing is context-sensitive in places the DFA cannot see: the
  header-name form inside `#include <...>`, line splicing, numeric suffixes,
  string prefixes, and the difference between `.` and `...`.
- Error recovery quality is a stated goal, and mixing generated DFAs with
  hand-written recovery is the maintainability complaint that shows up
  repeatedly in the literature.
- Clang and rustc are both hand-written, and the usual argument for a generated
  lexer (it is faster to write) does not hold for a language whose lexical
  grammar is small but irregular.

Decision: **hand-written, direct-coded, forward-only.** The punctuator grammar
is a direct-coded longest-match dispatch in one readable block, and every
spelling in that block is pinned to its kind by a test, so a missing row fails
loudly.

## What is retained, and what is not

| Retained | Why |
| --- | --- |
| Every byte, as trivia | Formatting, semantic tokens, doc comments, lossless round-trip |
| Raw spelling, with interpretation deferred | `"a\n"` is six bytes and a 2-byte value; diagnostics need the spelling |
| Error flags on tokens | One pass reports every lexical error; no early bail-out |
| Keyword classification | The preprocessor must not expand a keyword as a macro name |
| Trivia as ordinary tokens | The parser can filter; a formatter cannot un-filter |
| `FileId` + revision on every cached table | Offsets are not stable across edits |

Rejected, with the reason:

| Not retained in a token | Why not |
| --- | --- |
| `SymId` for identifiers | Would grow the token to 16 bytes and make the raw lexer depend on interning. Interning belongs to the layer that also interns, i.e. the parser/recorder path. |
| Leading/trailing trivia attached to a token (Roslyn/Swift style) | Requires deciding which side a comment belongs to, twice, at lex time. Explicit trivia tokens keep the decision available later and keep the stream a flat array. |
| Position (line/col) in the token | Derived data, recomputable from offset plus the line table, and it would go stale on edit. `dumpTokens` derives it on the fly. |
| A resumable state machine | Not needed while trivia is emitted, comments cannot nest, and literals cannot cross a line. See above. |

## Decisions, now fixed

| # | Question | Decision |
| --- | --- | --- |
| 1 | Where keywords are recognised | **In the raw lexer.** Same table (`keywords()` in `token_kind.cc`) drives the lexer, the dump, and the tests. The parser and preprocessor do not have to re-test a spelling. |
| 2 | `SymId` in the token | **No.** Token stays 12 bytes; interning happens where symbols are wanted. |
| 3 | Trivia model | **Explicit trivia tokens** (rust-analyzer style), always produced. The parser filters via `significantIndices()`. |
| 4 | Line splicing (`\` at end of line) | **Not supported, and not a lexical error.** A backslash at end of line inside a literal leaves it unterminated, which is the truthful report. A backslash outside a literal is `Invalid`. |
| 5 | Trigraphs and digraphs | **Rejected.** They are a C compatibility affordance with a long history of accidental behavior; `.mx` has no reason to inherit them. |
| 6 | Logical vs. physical positions | **Physical.** The line table is physical, so `line:col` in a diagnostic is the position in the file as the editor shows it. |
| 7 | Raw/multiline strings | **Not yet.** They are the one feature that would force a state parameter back; the door is left open deliberately rather than accidentally. |
| 8 | Red/green trees | **Deferred to the parser.** The lexer only has to stay lossless so the option remains open, and it is. |
| 9 | Nested block comments | **No**, C rule. `/* /* */` closes at the first `*/`. |
| 10 | Implicit octal (`010`) | **Rejected.** Leading zero without a prefix is plain decimal; octal is spelled `0o`. C's implicit octal is a well-known footgun and `.mx` states widths explicitly elsewhere too. |
| 11 | Literal suffixes (`10u`, `1.0f`) | **Open.** The lexer consumes the numeric core and leaves a following letter to start an identifier, so `10u` is `10` then `u`. Suffixes tie into the type system, so they are decided with it, not ahead of it. |
| 12 | Unicode identifiers | **Not supported.** Names are ASCII; a non-ASCII byte outside a comment or string is one `Invalid` token covering the whole character. |
| 13 | `#` in the lexer | **Not a token.** `#` introduces a preprocessor directive, and the preprocessor is a separate layer that owns it. `mincc lex` shows it as `Invalid`, which is the honest answer for "lex this file with no preprocessing". |

## The lexical grammar as implemented

- **Trivia**: ` `, tab, vertical tab, form feed are one `Whitespace` token per
  run; LF, CRLF, and a lone CR are one `Newline` token per terminator (the same
  rule `LineTable` uses, so positions and tokens cannot disagree);
  `//` to end of line; `/* ... */` non-nesting, unterminated at end of file.
- **Identifiers**: `[A-Za-z_][A-Za-z0-9_]*`, then an exact-match lookup in the
  keyword table. Only `fn`, `let`, `const`, and `return` are keywords today;
  primitive type names and the C spellings still lex as identifiers because
  making them keywords is a decision that follows the type system.
- **Numbers**: decimal, `0x`, `0b`, `0o`; a `.` continues the literal only when
  a digit of the current base follows it (so `1.` is `1` `.` and `obj.field`
  stays unambiguous); decimal `e`/hex `p` exponents only when digits follow (so
  `1else` is `1` then `else`). A base prefix with no digits is one token flagged
  `missing-digits`, not a split, so the caret underlines the whole prefix.
- **Strings and chars**: `"..."` and `'...'`, never crossing a line. Escapes are
  scanned (they must be, or `\'` would end a literal early) and a malformed one
  sets `unknown-escape` or `missing-digits`. `''` sets `empty-char`; `'ab'` does
  not, because what a multi-character literal means is a type question.
- **Punctuators**: longest match over the full set — assignment, arithmetic,
  bitwise, logical, comparison, and the delimiters.
- **Anything else**: one `Invalid` token per byte, except that a non-ASCII lead
  byte takes its continuation bytes with it so one character is one token.

## Error reporting

Flags exist so one pass finds every problem. `src/lex/lex_report.cc` is the only
file in the module that knows about spans and severity, and it walks the
`flagInfos()` table rather than the token's bits, so a flag added to the enum
cannot be silently unreported. `FlagInfo` keeps the diagnostic code, the short
name used by the dump, and the human message in one row, so the three cannot
drift apart.

`src/lex` (the `minc_lex` target) has **no dependency on diagnostics at all**.
That is enforced by the build graph, which is why `mincc lex` links
`minc_lex_report` separately and why a test can exercise the lexer with no
`Session` in sight.

## Non-goals for the lexer

- No type information, no name resolution, no semantic checks — those are sema.
- No source reading, buffering, or encoding validation — that is `SourceManager`.
- No macro expansion or directive handling — that is the preprocessor.
- No tree building — that is the parser.
- No printing, and no I/O of any kind. `dumpTokens` returns a string; the driver
  decides where it goes.

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
