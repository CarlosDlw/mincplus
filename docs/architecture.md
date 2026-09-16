# Architecture

This document is the contract map for the code that exists today. It answers
three questions for every module: what it owns, what it guarantees to its
callers, and what it is allowed to depend on.

## Layering

```
mincc                      driver: argv -> exit code
  └── minc_driver          command_spec / cli / help_render / help_text
        │                  suggest / error_report / input_source
        │                  lex / pp / parse / resolve / check / ir / build
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
- `src/support/` is LLVM-free by contract. The LLVM boundary is the pipeline's
  order and not one directory: nothing up to and including `sema` may include
  `llvm/*`, and `src/ir`, `src/backend/llvm` and the tests may. A unit test
  greps the tree for the inclusion, so the rule fails in CI rather than in
  review. A module that crosses the boundary also carries its own `.clang-tidy`
  subtracting `clang-analyzer-security.ArrayBound`: that one check reads LLVM's
  deliberate hung-off operands in `User.h` as an out-of-bounds access, and no
  header filter can scope it away because the report's path starts in the
  translation unit. `src/ir/.clang-tidy` is the worked example, and it is
  needed only where that check actually fires: `src/backend/llvm` reaches far
  fewer LLVM headers and the tidy gate is green there with no subtraction at
  all, so a module crossing the boundary adds the three lines when it has to
  and not as ceremony.
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
- **A rule two stages need lives in the stage that owns the fact, not in each of
  them.** `sema` and `ir` both ask "which declaration is this name node" of the
  same `DefMap`, so the answer is one class, `resolve::DefIndex`, and both ask
  *it* — the alternative was a copy of the key, the index and the written-span
  fallback in each, held together by nothing the compiler can check, and the
  copies had already drifted. The same reasoning gives
  `resolve/predefined.h` one table for the names the language binds before any
  source is read: `resolve` binds them, `sema` types them and `ir` lowers them,
  and all three now switch on a category instead of matching on a spelling.
  The test of the rule is a question, not a principle: *if this changes, how many
  files have to change with it, and does anything fail if one does not?*
- `src/ast/` and `src/resolve/` have **no platform branch**: a file is named by
  `support::FileId`, and the path identity behind it comes from `support`. Unix
  and Windows cannot disagree about which name a program means.
- **The whole platform branch is three files, all in `support`**: `support/term`
  (`<windows.h>`/`<unistd.h>`, virtual-terminal mode), `support/fs` (`stat`
  versus the Windows file index, case rules — see
  [`architectures/preprocessor.md`](architectures/preprocessor.md)) and
  `support/source/file_io` (binary mode on `stdin`, and the UTF-16 path
  conversion that bypasses the ANSI code page). No other file in `src/` or
  `include/` contains a platform branch, and the last two exist for the same
  reason as the first: a question is asked, a yes/no comes back, and the caller
  never learns which OS answered. This is checkable rather than aspirational —
  `grep -rn _WIN32 src include` is the whole issue.
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
- `renderAll` folds away one kind of repetition a `render` loop would show: a
  note anchored on exactly the span of the diagnostic above it prints its
  sentence and not the excerpt again, so one mistake never reads as two. A note
  pointing somewhere else keeps its excerpt. That is a property of the sequence,
  which is why the two entry points are not the same function.

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

- `command_spec.h` / `.cc`: **the command line as data**, and the reason the help
  and the parser cannot drift. `OptionSpec` rows state a name, a short letter,
  whether a value is taken, the one-line description, the default and the
  permitted values; `OptionGroup`s hold *ids*, so a group is a list of rows read
  from the other side; `CommandSpec` carries the usage lines, the paragraphs, the
  examples and the option set; `ProgramSpec` carries the overview. `cli.cc` reads
  it to decide what it is looking at and `help_render.cc` reads it to print a
  page, so a name or a description exists once. The relationships are **total in
  both directions**: the parser's `switch` over `OptionId` has no `default`, so
  the compiler proves every row is handled (`-Wswitch`, and CI builds warnings as
  errors), and a test walks the table the other way — every option a command's
  groups name is accepted by that command, every option the command accepts is in
  its groups, and every name in a `See also` is a command that exists. Design
  record: [`architectures/cli.md`](architectures/cli.md).
- `cli.h`: `parseArgs` is pure — it never prints, exits, or throws — and is
  **dirigido pela tabela**: no option is understood that is not a row, and an
  option of another command is refused rather than ignored. The first positional
  names the subcommand, `--` ends option parsing, `-` is a file, and `-h`/`-V`
  outrank any other problem on the line so `mincc --nosuch -h` still prints the
  page. The `help` command is deliberately *not* covered by that rule: it is an
  ordinary command that took an argument, so `mincc help buidl` is a usage error
  and not a page. Each row also says whether its command is implemented, and the
  overview derives the "(not implemented yet)" marker from that rather than
  naming commands in prose. `@file` expansion is not part of the parse at all:
  it happens first, and its failure arrives here as a string
  (`parseArgs`' `expansionError`) so the same precedence rule covers it.
  `opts.target` defaults to `sema::kDefaultTriple`, which is the **host** -- so
  `mincc check` reads the type widths of the machine it runs on, as `gcc` does.
- `help_render.h` / `.cc`: the renderer, pure and width-aware. One function
  serves the overview and one a single command's page, from `COLUMNS` →
  `ioctl`/`GetConsoleScreenBufferInfo` → 80 (in `support/term`, not here), so
  text is wrapped rather than pre-aligned by hand. ASCII-only, and the alignment
  is derived from the rows it is about to print.
- `response_file.h` / `.cc`: `@file`, the command line as a file, expanded
  **before** the parse because the parse is pure and reads nothing. The words in
  the file are the words on the line, so it may hold options, a command name and
  inputs alike; the expansion is depth-first and in place, bounded by
  `kMaxResponseFileDepth`, and a file that includes itself is refused by name. The
  tokenizer's one unusual rule is stated where it is implemented: `\` escapes
  only what would otherwise be special, so a Windows path survives.
- `diagnostic_options.h` / `.cc`: the one place a rendering's options are
  assembled -- the stream's color, the driver's tab width, `-ferror-limit`. A
  small file on purpose: it is the only dependency `lex` needs to render a
  diagnostic, and `stage_report.h` would pull `ir` and `backend` into it.
- `suggest.h` / `.cc`: the nearest name to an unknown command or option, for the
  `did you mean` note. Bounded edit distance over the spellings the table lists,
  so the suggestion can only name something that exists.
- `error_report.h`: the one way a driver-level error is written. Every
  subcommand uses it, so the prefix, the optional suggestion, the hint and the
  exit code are identical whichever command hit the problem. Each function also
  has a stream-taking form, which is what pins the exact text in a test and lets
  a command that is handed its streams report through the same code path.
- `help_text.h`: the I/O side of help — which stream and which exit code. It
  dispatches the six spellings a CLI of this kind has to answer (`mincc`,
  `--help`, `-h`, `help`, `help <cmd>`, `<cmd> --help`) onto the two renderings.
  Diagnostics go to stderr and pages to stdout.
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
  [x]             sema       the resolved AST  -> typed AST
  [x]             ir         the typed AST     -> LLVM module (CFG included)
  [x]             codegen    the LLVM module   -> object file / assembly
  [x]             link       objects           -> executable
```

Tokenization is phase 3 and directives are phase 4, so the lexer feeds the
preprocessor rather than the other way round, and no stage *after* the
preprocessor reads the source bytes directly: everything above it consumes the
preprocessed stream, which is the only reason a directive is not a syntax error
in the grammar.

**Declarations and definitions are one entity, not two.** `extern fn Type
Name(...);` is how the language writes "this is defined elsewhere" — in another
unit, in a library, in the C runtime — and `fn Type Name(...) { }` is the
definition. They are one `FnDecl` node kind, one `DefId` in `resolve`, one type
in `sema` and one symbol in the object, which is the property the whole pipeline
below `resolve` depends on: `Def::canonical` is the identity every stage keys its
maps on. The design record is
[`architectures/extern.md`](architectures/extern.md), and the ABI facts a value
crossing the boundary obeys are `src/cinterop`'s (roadmap §8) rather than this
form's.

**The same rule makes a file-scope binding one entity.** `let`/`const` at the top
of a unit is the *same node kind* a block-scope binding is, so one production,
one `DefId`, one type and one symbol serve both positions, and `extern let x: T;`
is a declaration by the word `extern` exactly as `extern fn` is. What is decided
for the file scope and not for a block is the initializer: it must be a constant
expression, evaluated in dependency order, because the language compiles one unit
at a time and a runtime initializer across units would have no order it could
name. That is why a `let` at file scope is the one construct that cannot be
computed at startup — the record is
[`architectures/globals.md`](architectures/globals.md).

**Builtins are not a stage.** When they arrive they are a table read by two
stages that already exist — `sema` for the signature and the effect, `ir` for the
lowering — plus the `sizeof`/`alignof`/`static_assert` operators, which are
grammar and a `sema` fold rather than anything new. The design record is
[`architectures/builtins.md`](architectures/builtins.md), and its first decision
is that `exit` and `assert` are **not** builtins: one is a declared symbol in the
runtime, the other is a macro that calls one.

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

**Every stage is shipped, end to end**: a `.mx` file goes in and an executable
that runs comes out, and no stage is a placeholder. They are also **wired**:
`mincc parse` runs the whole front end, so a file that begins with `#define` has
a syntax tree of its translation unit rather than a lex error on the `#`;
`mincc resolve` runs that tree through lowering, validation and name resolution;
`mincc check` runs all of it, types every expression, and reports the verdict
without emitting anything; `mincc ir` lowers the typed tree to an LLVM module and
prints it; and `mincc build` and `mincc run` carry that module through the
backend to an object, a link and a process. The eight commands are eight views of
one pipeline, and each names the others so a reader is never left guessing which
one to reach for:

| Command | Stage | Sees |
| --- | --- | --- |
| `mincc lex` | lexer | one file, raw tokens — a directive's `#` is an ordinary `Hash` token, because `#` is a punctuator of the lexical grammar and only its *meaning* is positional |
| `mincc pp` | lexer + preprocessor | the token stream of the translation unit — macros expanded, includes resolved |
| `mincc parse` | lexer + preprocessor + parser | the syntax tree over the preprocessed stream |
| `mincc resolve` | the front end through name resolution | the lowered AST, the scopes, and every name with the declaration it denotes |
| `mincc check` | the front end through type checking | the verdict — silent on success; `--stats` one line per file, `--types` the table of types, `--ast` every node with the type it was given |
| `mincc ir` | the front end + lowering | the LLVM module of the translation unit; `--target` selects the ABI, and the module's `target triple` is that ABI |
| `mincc build` | the whole pipeline, to a file | an executable, an object or an assembly listing — `-o`, `-O`, `-g`, `--emit`, `-L`/`-l`, and `-v` to print the commands it runs |
| `mincc run` | the same pipeline, to a process | the program's own output and its own exit status; everything after `--` is passed through unread |

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
- **sema** (`src/sema`) is the first stage that consumes a tree where every
  name already denotes a declaration, so nothing in it searches a scope. Its
  subject is types, filtered by the type checklist in
  `website/docs/language/features.md`, plus the two
  flow questions this grammar answers exactly — reachability (`terminates`,
  which is why a missing `return` is reported here) and definite assignment
  (`src/sema/check_flow.cc`, which is why a read of a binding nobody assigned is
  an error here rather than an `undef` in the IR). Those two are here and not in
  the backend because the constructs are structured: a later stage would have to
  re-derive what the tree shape already decides.
  Lowering, validation and resolution are stages of their own rather than
  bullets inside it, for the reason above; `resolve`'s source→definition map is
  also the layer an editor asks "where is this defined", so it is built here
  rather than bolted on when the LSP arrives. It returns the *typed* AST the IR
  consumes — types interned, conversions implemented once, and a failed
  expression typed as a poison rather than as a missing value, which is what
  keeps one mistake from becoming twenty.

  The shape is a small one, and each part exists for a consumer that already
  exists or is one stage away: `TypeStore` interns a type by *structure*, so
  `int`, `i32` and `signed int` are one `TypeId` — which is the property the IR
  and the linker need before they can compare two signatures. Its widths come
  from `sema/target.h`, a table selected by **triple** (`--target
  x86_64-unknown-linux-gnu`, `--target x86_64-pc-windows-msvc`), so `long` means
  what the *target* means and a cross
  build is not silently wrong; there is no `#ifdef` in the stage. The triple the
  table is keyed on is canonical and the host is a row of it: **the default target
  is the host** (`kHostTriple`, written by `cmake/minc_host.cmake` into
  `sema/host.h` at configure time, and `kFallbackTriple` when the build could not
  name a machine), because a compiler that defaults to a constant refuses to link
  where it runs and reads `long` as the wrong width there. `--target` is how a
  cross build is spelled, `sameAbi` is what the driver asks before calling a link
  native -- not the triple *text*, since the vendor component is identity and not
  ABI -- and an architecture alias (`arm64`, `amd64`, `i686`) is input syntax that
  parses to the canonical row. The typed AST
  is a **parallel array** beside the lowered tree rather than a field inside its
  nodes, the same decision `resolve` made for its `NameRef`s: the tree stays a
  value, so it stays hashable and the item-tree cache keeps working. And
  `sema::Context` is the compilation's central checker — one type store, one
  answer per `(FileId, revision)`, re-queryable for free — which is what lets
  the IR builder, a lint, and the language server all ask the same question
  without re-checking the unit or risking a different verdict.

  It also **publishes what the IR may not recompute**: every implicit conversion,
  as a pair of types keyed on the node that applies it, and the operation type of
  a compound assignment — the fact the tree cannot show, since `x <<= 9` on a
  `u16` is typed `u16` and *operates* at `i32`. A conversion recovered from a pair
  of types a stage later is a second copy of `convert.h`; a conversion asked for
  and answered is not. It is shipped together with the guarantee the table needs
  — no node of the artifact carries a deferred literal type, because such a type
  has no width and therefore no LLVM type at all.

  The same rule now covers memory. `sema` publishes one **`AccessObligation` per
  dereference** — the accessed type (and so the access's size and alignment, which
  the lowerer may not re-derive: an overestimated LLVM `align` is undefined
  behaviour, not slow code), the access kind, and a provenance it can *prove*,
  `object` or `foreign`. That record is what `memory.md` requires before the first
  `*` is lowered, and it is shipped with the pointer surface
  (`*T`, `&x`, `*p`, `p[i]`, stepping, comparison, `null`, `*void`) that produces
  it.

  The third record the same mechanism covers is the **cast**: the source's own
  conversion is recorded like an implicit one — `(from, to)` on the node that
  asked — so the lowerer materialises it through `Lowering::convert` and the
  constant folder folds it through the same pair → instruction table. The
  language has three spellings of it and one meaning (`x as T`, `(T)x`, and a
  literal's suffix, `10u8`), the C one made unambiguous by *reserving the type
  names* rather than by teaching the parser a symbol table, and the one
  conversion with a precondition — float to integer, where LLVM's `fptosi` is
  poison out of range — is guarded and traps, exactly as division by zero is.
  [`architectures/casts.md`](architectures/casts.md) is that record: the matrix,
  the suffixes, the refusals, and the plan.

  Design record: [`architectures/sema.md`](architectures/sema.md) — the type
  model, the conversion rules, the node-by-node surface, which stage owns which
  error, and the decisions the language had to make with it. `mincc check` is
  the command that proves it. [`never.md`](architectures/never.md) is the record
  for the bottom type: `fn ! name(...)` returns no value *and* never comes back,
  `!` converts into every other type because there is no value to be incompatible
  with, and the one thing a type cannot check — a body that promises to diverge —
  is proved rather than believed.
- **ir** (`src/ir`) lowers the typed tree into an `llvm::Module`. It is
  the first stage that may include `llvm/*`, and the boundary that moves with it
  is the pipeline's own: everything up to and including `sema` stays LLVM-free,
  and a test greps the tree so the rule fails in CI rather than in review. The
  stage *decides nothing*: it materialises what `sema` recorded, and a decision
  it cannot read is a failure rather than a guess. Everything the design record
  asked of `sema` — the coercion record, the operation type of a compound
  assignment (without which `u16 <<= 9` lowers to an out-of-range shift), and the
  access record — is **shipped**, so the lowering starts from a contract instead
  of waiting for one. A file-scope `let`/`const` is the case where "materialise"
  is literal: nothing runs before the program, so its bytes are an
  `llvm::Constant` built from the value the checker published and converted into
  the object's type **through the recorded coercion** — never from the pair of
  types, which is the silent cast (`const half: f64 = 1;`) the checker refuses.
  The object is emitted as an ordinary `global`, and the assumption scan has a
  row that keeps it that way, because LLVM's `constant` would claim nothing
  writes it (`globals.md`, `memory.md` decision 15; `ir.md` decisions 33–34). A
  `!` return type is the case where the contract pays off
  twice: it is `void` on the ABI side, `noreturn` is derived from the type rather
  than declared beside it, and the value that does not exist is a `poison` of the
  type the consumer asked for — the one named exception to the assumption list
  (`never.md`, `architectures/ir.md`). Design record:
  [`architectures/ir.md`](architectures/ir.md) — the LLVM decision, the coercion
  and access records, the runtime contract `sema`'s integer table imposes, the
  **assumption list** the closed set of guarantees the optimiser may be given and
  the scan that reads it, and the six mechanical rules that make a future node
  kind, type kind, operator or callee a compile error instead of a silent gap.
  [`memory.md`](architectures/memory.md) sits under that stage: the object and
  provenance model whose rules the assumption list is the emitted half of.
- **codegen** (`src/backend/llvm`) selects a `TargetMachine` from the
  module's triple, runs LLVM's pass pipeline, and writes an object or an
  assembly listing. It contains no instruction selection, no register allocation
  and no target knowledge, and it is the *smallest* stage in the pipeline for
  how much it decides. It is also where the project's promise becomes a
  statement about the program rather than about the compiler — **if the checker
  lets it pass, it must run** — which is why its failures are enumerated by
  class (environment, the program, this compiler) and why there is no semantic
  refusal in it at all. The driver's `build` and `run` sit above it: `build`
  drives a C linker *driver* (`clang`→`cc`→`gcc`) rather than a linker, and
  `run` is `build` into a temporary executable plus `exec` — one code path, and
  the program's own exit status. Design record:
  [`architectures/codegen.md`](architectures/codegen.md) — the target machine,
  the two pass managers, the position-independence rule that a host's default
  PIE makes load-bearing, the link through a driver, the exhaustive failure
  table, and debug information's half here and half in `ir`.
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
- **driver** links every stage and is the only place that turns a value into
  text: it picks `ColorMode` per stream with `support/term`, renders diagnostics
  with `DiagRenderer`, and owns the commands. `check` and `ir` share one front
  end (`driver/frontend.*`) so they cannot run different pipelines, and `build`
  and `run` are one function with a boolean between them for the same reason.
