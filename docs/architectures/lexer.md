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

### 4. Header-names — `src/lex/header_name.cc`

A second entry point beside `lexOne`, not a mode inside it. `#include <a/b.h>`
and `#include "a/b.h"` are header-names (C 6.4.7): context-sensitive, so the raw
lexer cannot produce them (`lexOne` never returns `HeaderName`), and not
expressible as tokens either, because inside a name `//` is not a comment, an
escape is not an escape, and `>` is legal within `"..."`. `scanHeaderName`
re-reads the bytes from the opening delimiter, which is what the preprocessor
calls before handling the directive. `lexOne` stays a pure, restartable function
of `(text, offset)` through all of it.

### 5. Preprocessor — `src/pp`

Directives and macro expansion belong here, on the token stream, producing a new
stream with spans that map back to the original spelling. It owns `#include`,
conditionals, and macro expansion, and it must never see a keyword as a macro
name — which is the C rule and the reason keywords are classified during lexing.

It is a **client of this module, not a pass before it**: it drives the same
`lexOne`, so there is one spelling of "what is an identifier" and a macro body is
stored as the tokens this lexer produced. See
[`preprocessor.md`](preprocessor.md) for the design and `docs/architecture.md`
for where it sits in the pipeline.

### 6. `Session` holds the derived tables

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

### Facts that are derived instead of stored

Clang stores several contextual facts on the token rather than in the text:
`IsAtStartOfLine`, `HasLeadingSpace`, `HasLeadingEmptyMacro`, and the
comment/whitespace tokens a client can request. The first two are worth
addressing directly, because the preprocessor needs them (a directive only
starts at the beginning of a line, and stringification cares about whitespace).

`minc+` does not store them, and does not need to, because it retains the fact
they are derived from: **the trivia is in the stream**. For any token at index
`i`:

| Clang flag | Derived as |
| --- | --- |
| `IsAtStartOfLine` | `i == 0`, or `tokens[i - 1]` is a `Newline` |
| `HasLeadingSpace` | `tokens[i - 1]` is a `Whitespace` |
| number of blank lines before | count consecutive `Newline` tokens before it |
| column | the token's offset through the line table |

Storing a copy would create a second source of truth: a formatter that inserts
or removes a blank line would have to remember to fix the flags, and any code
path that builds tokens another way would have to remember to set them. With
trivia retained, the answer is always recomputable and always consistent with
the bytes.

## Decisions, now fixed

| # | Question | Decision |
| --- | --- | --- |
| 1 | Where keywords are recognised | **In the raw lexer.** Same table (`keywords()` in `token_kind.cc`) drives the lexer, the dump, and the tests. The parser and preprocessor do not have to re-test a spelling. |
| 2 | `SymId` in the token | **No.** Token stays 12 bytes; interning happens where symbols are wanted. |
| 3 | Trivia model | **Explicit trivia tokens** (rust-analyzer style), always produced. The parser filters via `significantIndices()`. |
| 4 | Line splicing (`\` at end of line) | **Not supported, and not a lexical error.** A backslash at end of line inside a literal leaves it unterminated, which is the truthful report. A backslash outside a literal is `Invalid`. **Known cost, measured**: the differential harness in `tests/unit/pp/differential_test.cc` pins the divergence this creates against `cc -E` -- a macro definition cannot continue onto the next line, so `#define X a \` + newline defines `X` as `a` and leaves the next line loose. Supporting it means the lexeme is no longer always `text.substr(offset, length)`, which is why it is a task of its own (see [`../roadmap.md`](../roadmap.md)). |
| 5 | Trigraphs and digraphs | **Rejected.** They are a C compatibility affordance with a long history of accidental behavior; `.mx` has no reason to inherit them. |
| 6 | Logical vs. physical positions | **Physical.** The line table is physical, so `line:col` in a diagnostic is the position in the file as the editor shows it. |
| 7 | Raw/multiline strings | **Not yet.** They are the one feature that would force a state parameter back; the door is left open deliberately rather than accidentally. |
| 8 | Red/green trees | **Deferred to the parser.** The lexer only has to stay lossless so the option remains open, and it is. |
| 9 | Nested block comments | **No**, C rule. `/* /* */` closes at the first `*/`. |
| 10 | Implicit octal (`010`) | **Rejected.** Leading zero without a prefix is plain decimal; octal is spelled `0o`. C's implicit octal is a well-known footgun and `.mx` states widths explicitly elsewhere too. |
| 11 | Literal suffixes (`10u`, `1.0f`) | **Open.** The lexer consumes the numeric core and leaves a following letter to start an identifier, so `10u` is `10` then `u`. Suffixes tie into the type system, so they are decided with it, not ahead of it. |
| 12 | Unicode identifiers | **Not supported.** Names are ASCII; a non-ASCII byte outside a comment or string is one `Invalid` token covering the whole character. |
| 13 | `#` and `##` in the lexer | **Tokens, `Hash` and `HashHash`.** They are punctuators of the lexical grammar, not a preprocessor concept: Clang spells them `tok::hash`/`tok::hashhash` and GCC's cpplib `CPP_HASH`/`CPP_HASHHASH`. Only their *meaning* is positional, and position is the preprocessor's business ([`preprocessor.md`](preprocessor.md)). Classifying them here is what keeps the later stages small — no stage has to identify a directive by sniffing the spelling of a byte the lexer refused to classify, and no stage has to reassemble a `##` out of two adjacent `#` bytes. Longest match applies as it does everywhere else, so `##` is one token and `# #` is two, which is exactly the distinction a paste operator needs. |

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
  sets `unknown-escape`, `missing-digits`, or `escape-out-of-range`. That last
  one covers `\uD800` and `\U00110000`: the digits are all present but they do
  not name a character UTF-8 can encode. Checking it is the same call the
  escape alphabet already gets -- an encoding constraint, not a type question --
  and C constrains universal character names the same way. `''` sets
  `empty-char`; `'ab'` does not, because what a multi-character literal means is
  a type question.
- **Punctuators**: longest match over the full set — assignment, arithmetic,
  bitwise, logical, comparison, the delimiters, and the preprocessor's `#` and
  `##`. `#`/`##` are classified here and *interpreted* later: a `Hash` that
  starts no directive and a `HashHash` outside a macro body are diagnosed by the
  preprocessor, which is the only stage that knows what position they are in.
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

## How the claims above are checked

Every property this document asserts has a test that fails if it stops being
true, and the interesting ones are exhaustive rather than sampled:

| Claim | Checked by |
| --- | --- |
| Every byte belongs to exactly one token | `lossless()` audited on every lex, plus a byte-for-byte reconstruction test |
| Every token boundary is a restart point | re-lexing from each boundary of a whole file |
| No byte is unhandled, nothing reads past the end | exhaustive over all 1-byte and 2-byte inputs, and all 3-byte combinations of the bytes that change scanning |
| Arbitrary input terminates and tiles | 2000 deterministic pseudo-random byte soups |
| One pass finds every problem | multi-error source, plus a token wrong in two ways at once |
| The invariants hold under a tool that watches memory | the `sanitize` preset: ASan + UBSan, `-fno-sanitize-recover`, run in CI |
| The examples stay lexable | `tests/unit/lex/examples_test.cc` lexes every file in `examples/` |

The sanitizer preset earned its place immediately: it turned
`Arena::allocate(SIZE_MAX)` from "returns nullptr on this platform" into a hard
AddressSanitizer abort, because handing an impossible size to `operator new` is
implementation-defined. The arena now refuses such a request itself
(`kMaxArenaAllocation`), which is what its documented contract promised.

## Non-goals for the lexer

- No type information, no name resolution, no semantic checks — those are sema.
- No source reading, buffering, or encoding validation — that is `SourceManager`.
- No macro expansion or directive handling — that is the preprocessor
  ([`preprocessor.md`](preprocessor.md)).
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
