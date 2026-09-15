# Contributing to minc+

Thanks for being here. This project is a language and the compiler that
implements it, and both are written to a stated standard rather than to taste:
the standard is what the design records in `docs/` say, and the way to change
the project is to change one of them first.

Everything below is meant to be enough to make a first contribution without
asking. If something here is not enough, that is a bug in this file.

- [What is worth contributing](#what-is-worth-contributing)
- [Getting set up](#getting-set-up)
- [The gates](#the-gates)
- [House rules](#house-rules)
- [Adding a language feature](#adding-a-language-feature)
- [Adding a builtin](#adding-a-builtin)
- [Adding a command-line option](#adding-a-command-line-option)
- [Commits](#commits)
- [Pull requests](#pull-requests)
- [Licensing](#licensing)

## What is worth contributing

Every one of these is a real contribution, and they are listed in the order they
are usually most useful:

- **A bug report that reproduces.** The smallest `.mx` file that shows it, the
  command, and the two outputs. See [SUPPORT.md](SUPPORT.md) for what to include.
- **An example.** `examples/` is the corpus the test suite compiles, so an
  example is a specification that cannot go stale. A construct with no example is
  a construct with no test that a reader can see.
- **A documentation fix.** A page that is wrong, unclear, or missing a
  *not implemented yet* mark.
- **A diagnostic that could be better.** The message, the caret, or the code.
  Diagnostics are part of the interface here, not an afterthought.
- **A compiler change.** New syntax, a new check, a new lowering, a performance
  problem. Start with an issue if the design is not already decided.

Read `README.md` first for what exists today, and the
[feature checklist](website/docs/language/features.md) for what is *decided*,
what is *planned* and what is still *open*. A pull request for a feature marked
`[?]` (open) will be sent back to that decision before it is reviewed as code.

## Getting set up

Requirements, build and test commands are in
[`README.md#build`](README.md#build). The short version:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

`make quick` is the inner loop (build, test, format check) and `make gates` is
everything CI runs. Four presets exist — `dev`, `release`, `ci` (warnings as
errors) and `sanitize` (ASan + UBSan, Linux) — and the `Makefile` is a shortcut
over them and nothing else.

The build downloads nothing except a pinned googletest, and only when the system
has none (`-DMINC_FETCH_GTEST=OFF` forbids even that). Adding a dependency to the
compiler is a design decision, not a convenience: open an issue before you write
the code.

## The gates

CI runs these on Linux, macOS and Windows, and a pull request is expected to pass
all of them:

| Gate | Command | What it is |
| --- | --- | --- |
| build + test | `make ci` | the whole suite, warnings as errors |
| sanitizers | `make sanitize` | ASan + UBSan over the same suite |
| format | `make format` / `make format-check` | clang-format, the version in `.clang-format` |
| static analysis | `make tidy` | clang-tidy with the project's `.clang-tidy`, findings as errors |
| examples | `make examples` | every command over every file in `examples/` |
| docs | `make docs` | the language reference, which throws on a broken link |

`make gates` runs the first four, in the order CI does — cheapest first, so the
gate that fails is the one that failed. The `docs` and `examples` targets are not
part of it: one needs Node, the other builds the compiler.

Three notes that save time and are not obvious:

- **ccache is the difference between ~106 s and ~4 s** for the same clean build.
  `make ccache` reports the hit rate; `make ccache-tune` raises the limit
  (`CCACHE_SIZE=20G make ccache-tune`), because the 5 GiB default is small for
  four presets — entries get evicted and paid for twice.
- **`dev` and `ci` do not share a cache.** They differ only by `-Werror`, which
  ccache keys on, so iterating in `dev` and running `make gates` before a commit
  costs one build of each. Running `make dev` *and* `make ci` pays twice.
- **clang-tidy is the slowest gate** (minutes, not seconds). `make tidy` uses
  `run-clang-tidy -j` when it is installed — same checks, same
  `--warnings-as-errors`, same non-zero exit on a finding — which is ~6m20s
  against ~1m50s on the reference machine.

## House rules

These are conventions and not preferences, and they decide most reviews:

- **English**, everywhere: code, comments, commit messages, docs. Identifiers
  are meaningful English words.
- **No god-files.** One responsibility per file, split before a file grows past a
  few hundred lines. If a change makes a file do two things, it is two files.
- **No exceptions in utility code.** A recoverable failure is an `Expected<T, E>`
  (alias `Fallible<T>`); `Arena` reports exhaustion with `nullptr`.
- **`[[nodiscard]]`** on anything whose result must be observed.
- **A rule that two stages need lives in the stage that owns the fact**, not in
  each of them. A rule copied into two modules is two answers waiting to differ.
- **Errors are values.** With the one exception of the driver, a stage library
  does not link the diagnostic machinery; it returns errors, and a `*_report`
  library turns them into diagnostics.
- **Every limit lives in `support/limits.h`**, with the `static_assert`s that keep
  the numeric and human-readable forms in sync. Nothing re-derives one.
- **Comments say why.** The code says what; a comment that repeats the code is
  noise, and one that explains the alternative that was rejected is the reason
  the file is readable in a year.
- **No attribution footers.** A commit message is about the change.

## Adding a language feature

The order matters, and it is the order that keeps the pipeline honest:

1. **Decide what the feature means, in writing.** Add it to the
   [feature checklist](website/docs/language/features.md) with its status, and if
   it needs an argument — a choice against what the market does, a rule with a
   consequence — write a record in `docs/architectures/`. A feature whose design
   is not written down is not ready to be implemented.
2. **Say which stage owns each fact.** Every fact is computed once, in the stage
   with the information to compute it, and every stage below *reads* it: a
   conversion, a type width, an access obligation, a signature, which declaration
   a node names. If two stages would compute the same fact, the fact belongs to
   the earlier one.
3. **Grammar, then types, then lowering.** A change that starts in the IR has to
   be undone later. The lexer learns a token only if the language has real
   syntax for it; otherwise the token is reconstructed where it belongs.
4. **A test per stage the change touches**, including the negative cases: the
   type that must not convert, the body that must not compile, the argument that
   must not be promoted, the offset that must not be accepted. The failure modes
   that matter are the ones that compile.
5. **An example in `examples/`**, because the corpus is compiled end to end by
   the test suite and it is the only specification a reader can run.
6. **The documentation site.** The page that describes the feature, and — if any
   part of it is not implemented — a *not implemented yet* mark on the part that
   is not. A page that promises something the compiler does not do is worse than
   a missing page.
7. **The record.** `docs/architecture.md` for where a new file or library sits,
   and the stage's record in `docs/architectures/` for what it now decides.

A small checklist for the awkward parts:

- A new **token** is a `TokenKind`, a name in `token_kind.cc`, a spelling in the
  parse layer's table, and a lexer test.
- A new **node** is a `SyntaxKind`, a lowering case, a validator case if it can
  be malformed, and a dump test.
- A new **diagnostic** is a code in its stage's table, a row with its severity,
  a test that reaches it, and — if the code is reachable — a message naming
  whatever the reader has to change.
- A new **budget** is a constant in `support/limits.h`, a `static_assert`, and a
  test that lowers it and proves the bound is a bound.

## Adding a builtin

Read `docs/architectures/builtins.md` first: it states which of the three
families a new name belongs to, and that a builtin is a row in a closed table
with a *required* effect field — not a name matched in a stage. `exit` and
`abort` are not builtins: they are `extern` symbols the C runtime provides.

## Adding a command-line option

The command line is data, not code: one row per option in
`src/driver/command_spec.cc`, read by both the parser and the help renderer, so a
name and its description exist once. Add the row, the group it belongs to, the
command that owns it, and a test that the option is accepted there and rejected
elsewhere. `docs/architectures/cli.md` has the rules, including the one that
matters: an option that is a *view* of what a stage decided must print what the
stage recorded, not re-derive it.

## Commits

One topic per commit, and the message says **what changed and why** — the subject
names the modules, because that is what a reader greps for:

```
sema, ir: the bottom type `!` — `fn ! f()` never returns, and that is a type
lex, pp, parse, sema, ir: variadic declarations, and `...` as the language's token
cli: declare the command line as data, and render a page per command
docs: mark the pipeline as shipped end to end, and state what is left
```

A body is welcome when the change has a reason worth keeping: the alternative
that was rejected, the bug the refactor closed, the measurement that decided it.
That is what `git log` is for in this repository — the history is the third
design document, after `docs/` and the code.

Do not mix an unrelated reformat or rename into a change: the gates catch the
formatting, and a reviewer cannot see the change in a diff that touches
everything.

## Pull requests

A pull request is ready when:

- it does one thing, and the description says what and why;
- `make gates` is green, and `make examples` too if the front end changed;
- every new behaviour has a test, including the case that must *not* work;
- the documentation is updated, including the site, and anything unimplemented is
  marked as such;
- if the change affects the language, the
  [feature checklist](website/docs/language/features.md) is updated.

Reviewers look for: the fact belonging to the right stage, the rule existing
once, the error being a value, the comment explaining why, and the test proving
the claim. A change that passes the gates and leaves one of those out will get a
question rather than a rejection — the answer is usually the interesting part.

## Licensing

minc+ is MIT-licensed (see [LICENSE](LICENSE)). Contributions are accepted under
the same license, inbound equals outbound; there is no CLA and no copyright
assignment. Every new file starts with the two-line header the other files carry:

```
// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
```
