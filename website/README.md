# minc+ documentation

The language reference, as a [Docusaurus](https://docusaurus.io) site.

```sh
npm install
npm start          # a dev server with hot reload at http://localhost:3000
npm run build      # static output in build/
npm run serve      # serve the built site
npm run typecheck  # tsc over the config and the theme
```

## What belongs here, and what does not

This site documents the **language**: what a `.mx` file means, what the compiler
promises about it, and what each tool does.

It does **not** document the compiler's internals. Why a pipeline stage is shaped
the way it is, what the alternatives were, what the references do — those records
live in the repository, next to the code they describe:

- `docs/architecture.md` — the layout, the module graph, the platform branch
- `docs/roadmap.md` — what is decided and what is next
- `docs/architectures/*.md` — one record per stage, plus the memory model

That split is deliberate. A design record is read *while reading the code* and
goes stale with the code; a language reference is read by someone writing a
program and has to be true of the language, not of this week's implementation.

## How to write a page here

- **Say what is true, and mark what is not.** Anything not implemented is in a
  `:::note[Not implemented yet]` block. The site is a description of the language,
  so a page that promises something the compiler does not do is a bug.
- **Show real output.** Every `console` block is copied from a run of the
  compiler, including the diagnostic text. If the compiler's message changes, the
  page changes with it.
- **One subject per page**, and the sidebar order is a reading order: the reader
  meets types before statements, and the reference tables after both.
- **No invented syntax.** If a page needs a construct, that construct has to be in
  `examples/` and passing.

## Layout

```
docs/
  intro.md                     the home page
  getting-started/             install, hello world, a tour
  language/                    the reference proper, one file per subject
  reference/                   grammar and the operator table
  tools/                       the command line, diagnostics, the architecture map
src/css/custom.css             theme overrides
docusaurus.config.ts           site configuration (docs-only: the docs are the site)
sidebars.ts                    the reading order, written out
```
