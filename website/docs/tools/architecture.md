---
sidebar_position: 3
---

# Architecture

This site documents the **language**: what a `.mx` file means and what the compiler
promises about it. The compiler's *internals* — why a stage is shaped the way it
is, what the alternatives were, what the market does — are a separate body of
writing, and it lives in the repository next to the code it describes.

This page is the map.

## The pipeline

```
source → lex → preprocess → parse → lower → validate → resolve → check → ir → object → link
```

| Stage | Source | Command | What it decides |
| --- | --- | --- | --- |
| lex | `src/lex` | `mincc lex` | tokens, and where every byte went |
| preprocess | `src/pp` | `mincc pp` | macros, conditionals, includes |
| parse | `src/parse`, `src/syntax` | `mincc parse` | the lossless syntax tree |
| lower | `src/ast` | — | a compact tree for analysis |
| validate | `src/ast` | — | structural rules, before the expensive passes |
| resolve | `src/resolve` | `mincc resolve` | scopes, definitions, one reference per name |
| check | `src/sema` | `mincc check` | types, conversions, flow, the verdict |
| ir | `src/ir` | `mincc ir` | the `llvm::Module` |
| backend | `src/backend/llvm` | `mincc build` | the object, and the link |

Everything below `check` reads what `check` recorded rather than re-deriving it.
That is the single rule that makes the pipeline safe to extend: a conversion, an
access, a type width and a function's signature each have one answer, decided
once, in the stage that has the information to decide it.

## The design records

In the repository:

| File | Why it exists |
| --- | --- |
| `docs/architecture.md` | the whole layout: targets, module graph, where the platform branch is |
| `docs/roadmap.md` | what is decided, what is next, and in what order |
| `docs/architectures/lexer.md` | tokens, trivia, the two-pass shape |
| `docs/architectures/preprocessor.md` | expansion, provenance, the budgets |
| `docs/architectures/parser.md` | the grammar, the lossless tree, error recovery |
| `docs/architectures/resolve.md` | scopes, the item tree, source-to-def |
| `docs/architectures/sema.md` | types, conversions, flow, what the checker publishes |
| `docs/architectures/memory.md` | **the memory model** — the level the language aims at |
| `docs/architectures/never.md` | the bottom type, and why it is a type |
| `docs/architectures/ir.md` | the lowering, the records, the assumption list |
| `docs/architectures/codegen.md` | the backend: objects, debug info, the link |
| `docs/architectures/cli.md` | the command line as data |
| `docs/architectures/builtins.md` | how a builtin is added, and what is not a builtin |
| `docs/architectures/slices.md` | the view (`{ptr, len}`), and the six rules that keep it from being C's decay |
| `docs/architectures/extern.md` | `extern`, variadics, the C ABI boundary |

If you are reading the compiler to change it, `docs/architecture.md` first, then
the record for the stage you are changing.

## A few things worth knowing before you read the code

**The language is the product; the pipeline is a means.** A feature lands in the
pipeline stage that owns the *fact*, and the stages below read it. There is no
stage that "just lowers" something another stage decided.

**Errors are values.** With one exception — the driver — a stage library does not
link the diagnostic machinery. It returns errors, and a separate `*_report`
library turns them into diagnostics. That is what lets a test drive a stage with
no `Session` and no terminal, and what lets the language server reuse the front
end whole.

**A rule two stages need lives in the stage that owns the fact.** If `sema` and
`ir` both need to know which declaration a node names, they ask `resolve` — one
implementation, because a rule copied into two modules is two answers waiting to
differ.

**The platform branch is three files.** `support/term` (terminal mode),
`support/fs` (files, case rules) and the driver's toolchain invocation are the
whole of it. Everything above is written to behave identically everywhere.

**Nothing is committed to the optimizer that the program did not state.** The
lowering's assumption list is closed, lives in one file, and is scanned: no
type-based alias analysis, no `nsw` on arithmetic the language defines to wrap, no
`inbounds` the compiler did not prove. It is the mechanical reason the language
can say "no undefined behavior the programmer cannot see" and mean it.

## Contributing a language feature

The order that keeps the pipeline honest:

1. **Write down what the feature means**, in the
   [feature checklist](/language/features) (decided / planned / open) and, when
   it needs an argument, in a design record.
2. **Decide which stage owns each fact** the feature needs. A fact belongs to the
   stage with the information to compute it, and every stage below reads it.
3. **Grammar first, then types, then lowering** — a change that starts in the IR
   is a change that has to be undone later.
4. **A test per stage it touches**, including the negative cases: the type that
   must not convert, the body that must not compile, the argument that must not
   be promoted.
5. **An example in `examples/`** that exercises the whole path, because the
   examples are the compiler's end-to-end test.
