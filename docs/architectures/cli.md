# The command line — `src/driver`

The design record for `mincc`'s user interface: how options are declared, how
help is produced, how a mistake is answered, and why the parser is not where any
of that is written down. It sits under
[`architecture.md`](../architecture.md), which states the stage order and the
stage contract; this document is about the one part of the compiler a user talks
to directly.

**Status: shipped.** The help engine, the per-command help, `mincc help <cmd>`,
`--color`, the verbose version block and the spelling suggestions are
implemented, and the parser reads the same tables the help is rendered from.

## What was wrong, measured

The three complaints are one complaint, and it is worth writing down as
evidence rather than as an impression:

```
$ mincc --help | wc -l
63
$ mincc build --help | diff - <(mincc --help) && echo "identical"
identical
$ mincc lex --help | diff - <(mincc help) 2>/dev/null ; mincc help
mincc: error: unknown command 'help'
```

1. **There was no per-command help and no place to put it.** The command table
   held `name`, `args`, `summary` and `implemented` — nothing that could say what
   `build`'s options are, because no such record existed. So `build --help` had
   nothing to print and fell through to the global text. That is a **missing data
   model**, not a missing flag, and it is why the fix is a table rather than
   another string.
2. **The help text was a hand-written string with command names inside it.** The
   global block carried lines like `--refs  resolve: print every name use`. A
   single `constexpr const char*` is a second source of truth beside the parser,
   and the two agreed only because a test checked a few of them.
3. **`help` did not exist** as a command, and `--version` printed one line.

## The decision that decides the rest

**The command line is data, and the parser and the help are two readers of it.**

```
CommandSpec ──┬─→ parseArgs()      recognition: which options exist, which take
              │                    a value, which command owns them
              └─→ renderHelp()     presentation: groups, columns, wrapping
```

Everything else in this document follows from that one sentence. It is the same
move the rest of the compiler makes — `sema` publishes what the IR may not
re-derive, the codegen failure table is one table with a derived enumeration —
applied to the user interface: **one statement of each fact, and a test that the
readers agree.**

The alternative was considered and rejected: a chain of `if (arg == "--emit")`
branches beside a hand-written help string is two lists, and two lists drift.
That is not a hypothetical here; it is what complaint 2 *is*.

### What is data, and what is code

Not everything can be a table, and pretending otherwise would produce a small
argument-parsing DSL to maintain. The line is drawn where it is honest:

- **Data, in `command_spec.cc`** — the surface: names, short letters, whether an
  option takes a value and what it is called, the one-line description, the
  default, the permitted values, which group it is shown in, examples, the
  environment variables, `see also`.
- **Code, in `cli.cc`** — the behaviour: `-D` becomes a `(name, body)` pair,
  `-o` last-wins, `-W` carries a warning name, a `--` stops option parsing.

So each option has an `OptionId` enumerator, the table attaches the *surface* to
it, and the parser switches on the id. A test then proves the pair is total in
both directions: **every id in the table is handled by the parser, and every id
the parser handles has a row in the table.** Adding an option without a row is a
failing test rather than an option that exists but cannot be discovered.

## `mincc` is git-like, and that fixes six forms

The guide the whole industry writes these rules down in
([clig.dev](https://clig.dev/)) is specific about a multi-command program, and
it is specific because these six forms are what people type:

| Invocation | Today | What it prints |
| --- | --- | --- |
| `mincc` | exit 2, "missing command" | the **overview** (concise: what this is, the commands, the common options, and how to get more) — but still exit 2, because nothing was done |
| `mincc --help`, `mincc -h` | global wall | the overview, exit 0 |
| `mincc help` | unknown command | the overview, exit 0 |
| `mincc help build` | unknown command | `build`'s own page, exit 0 |
| `mincc build --help` | the global wall | `build`'s own page, exit 0 |
| `mincc build -h` | the global wall | `build`'s own page, exit 0 |

`-h` is **not overloaded**: it is help here and it stays help. (`-V` is version,
`-v` is verbose, and the collision between those two letters on a *compiler* is
real — § *The version block* below.)

`mincc` with no arguments prints help and exits **2**, not 0: printing help is
what it does, but nothing succeeded, and a script that runs `mincc` with an empty
variable must not read the result as success. It prints to stderr in that case
and to stdout when help was asked for, which is the rule the next section states.

## Output, streams, and exit codes

Unchanged from the rest of the compiler, restated because it is a contract a
script depends on:

| | Stream | Exit |
| --- | --- | --- |
| help, asked for (`--help`, `help`, `-h`) | stdout | 0 |
| help, as the answer to an empty command line | stderr | 2 |
| diagnostic, warning, error | stderr | 1 |
| usage error (bad option, bad value, unknown command) | stderr | 2 |
| `run`: the program's own status | the program's | the program's |

The distinction between "stdout, exit 0" and "stderr, exit 2" for *the same text*
is deliberate: `mincc --help | less` gets the help, and `mincc > /dev/null` in a
test does not silently pass because a message went to the wrong stream.

## The page

Every command's page is rendered from its spec in one order, and the order is
`cargo`'s because it is the one that reads well at 80 columns:

```
Compile and link an executable                    <- one line, no period
Usage: mincc build [options] <files...>           <- one or more, as written
       mincc build -o prog main.mx
                                                  <- blank
Description, wrapped to the terminal width.       <- paragraphs
                                                  <- blank
Options:                                          <- groups, unnamed first
  -o PATH          where the output goes ...
  ...
Emission:
  -O LEVEL         ...
  --emit KIND      exe, obj or asm [exe]
Examples:
  $ mincc build -o prog main.mx
  $ mincc build --emit asm main.mx
See also: run, check

Run 'mincc help' for the list of commands, or 'mincc help <command>' for another
one.
```

`Environment:` is on the **overview** and not on every page: those variables are
read by the program, not by one command. A page prints what it can act on.

Rules the renderer obeys, each of which is a test:

- **The width is asked for, never assumed.** `COLUMNS` first — an explicit user
  answer beats a guess — then the terminal, then 80. The terminal question is
  `support/term`'s (see *Cross-platform* below).
- **Wrapping counts display columns, not bytes.** The text is ASCII-only by
  contract (`help_text.h` states it and a test enforces it, because a Windows
  console renders UTF-8 unpredictably), so today the two are equal — but the
  renderer is written as if they were not, so a future non-ASCII character cannot
  silently make a line wider than the terminal.
- **A hint about the next step is part of the page.** `build`'s page ends by
  naming `run`, `run`'s names `build --emit obj`. clig.dev's "suggest commands
  the user should run" is cheap here because the table already knows the graph.
- **Nothing is printed that the spec does not contain.** No command's page is
  assembled from prose with command names in it; if a fact is worth printing it
  is a field, and if it is a field it is printed for every command that has it.

## The overview

The overview is **short on purpose**: the program and what it is, the commands
with their one-line summaries, the options that apply to every command, and the
two lines that make the rest discoverable:

```
Run 'mincc help <command>' for one command's page.
```

A command's own page closes symmetrically, and the pair is what makes the graph
navigable in both directions:

```
Run 'mincc help' for the list of commands, or 'mincc help <command>' for another
one.
```

The list of options is not repeated per command here. This is the failure the
old text had — a global block with `resolve:`, `check:`, `pp:` prefixes inside
option descriptions — and it is what "tudo jogado no `--help`" described. A fact
about `resolve` belongs on `resolve`'s page.

`--help-all` was considered and rejected: an option that dumps every command's
page is a wall of text with a flag in front of it, and `mincc help <cmd>` is one
keystroke more for a page a human can read.

## A mistake, answered

Today: `mincc: error: unrecognized option '--targt'` followed by a hint. The
hint stays, and the error gains a **suggestion** from the same table:

```
$ mincc build --targt x86_64-unknown-linux-gnu a.mx
mincc: error: unrecognized option '--targt'
note: did you mean '--target'?
Try 'mincc build --help' for more information.
```

Three rules, and the third is the one that matters:

1. The candidate set is the **spec's**, so a suggestion can only name something
   that exists — the table cannot suggest an option that was removed.
2. The comparison is edit distance with a length-relative threshold, and it is
   symmetric over the command's own options and the global ones.
3. **It suggests, and never acts.** clig.dev's warning is the reason: an invalid
   input is not necessarily a typo, and a program that "fixes" a command line
   changes what the user typed into something they did not write — and then has
   to keep supporting it. The suggestion is a note; the exit code stays 2.

The same applies to the command itself (`mincc buld` → `note: did you mean
'build'?`), which is also what makes the help discoverable: a user who mistypes
learns the real name.

## `-h` outranks a typo; `help` does not

The two ways to ask for help look equivalent and are not, and the difference is
one line in the parser (`flagForm` in `cli.cc`):

```
$ mincc --nosuch -h        # the flag wins: the page is printed, exit 0
$ mincc help buidl         # error: unknown command 'buidl'; note: did you mean
                           # 'build'? -- exit 2
```

`-h` and `-V` are **a request about the line itself**, so they outrank any problem
on it — clig.dev's "you should be able to add `-h` to the end of anything and it
should show help". `mincc help <name>` is an **ordinary command that took an
argument**, and an argument that does not answer to any command name is a usage
error like any other. Collapsing the two would mean `mincc help buidl` printed
the general index and exited 0, which answers a question nobody asked and hides
the one they did. `mincc help build check` is the same rule: one command name is
what the command takes, and the second is an error rather than ignored.

`Try 'mincc build --help'` — and not `mincc --help` — is part of the answer: the
usage error knows which command was being invoked, so the hint can name the page
that has the answer.

## The version block

`-V`/`--version` prints one line. `-vV`, `-Vv` and `-V -v` print the **bug-report
block**:

```
$ mincc -vV
mincc 0.1.0
binary:         mincc
host:           x86_64-pc-linux-gnu
default target: x86_64-unknown-linux-gnu
LLVM:           22.1.8
```

This is `rustc -vV`'s shape, and it is that shape for one reason: a bug report
needs the four facts that decide whether a bug is reproducible — the version,
the machine it ran on, the target it was aimed at, and the LLVM it was built
against.

`host:` is **asked of LLVM** (`llvm::sys::getDefaultTargetTriple()`) rather than
derived here, because LLVM's answer is the one its own backend will use, and a
second spelling of the host is a second thing to be wrong. That it can differ
from `default target:` is not a bug in the display: it is the display being
honest about a difference that is real, and it is how the two findings at the end
of this document were noticed.

`-vV` and `-Vv` are accepted as clusters because `rustc` made that spelling
muscle memory. A general short-option cluster (`-abc` = `-a -b -c`) **is**
implemented, but only over letters that take no value: `-vo out` is not a
spelling this compiler accepts, because guessing which letter owns the value is
how a CLI starts inventing grammar. There is no `--verbose` long spelling; `-v`
is the option `build` and `run` own, which is why the pair is written as a
cluster or beside `-V` rather than as `--version --verbose`.

## `--color`

`--color=auto|always|never`, and it is a **global** option rather than a per
command one: it is a property of the terminal the output goes to, not of the
stage being run.

The precedence, in order, is:

1. `--color=never` — nothing wins over an explicit no.
2. `--color=always` — an explicit yes beats the environment, which is what `gcc
   -fdiagnostics-color=always` and `cargo --color always` do, and it is the only
   way to get color into a file or a pager.
3. `NO_COLOR` (set to anything, per [no-color.org](https://no-color.org)) and
   `TERM=dumb`, both of which already exist in `support/term`.
4. `auto`: color when the stream is a terminal.

`auto` is the default, and it is decided **per stream**: help goes to stdout and
diagnostics go to stderr, and `mincc build a.mx 2> err.log | less` has one of each.

## Environment variables

| Variable | Effect | Precedence |
| --- | --- | --- |
| `NO_COLOR` | disables color | below `--color` |
| `TERM=dumb` | disables color | below `--color` |
| `COLUMNS` | the width help wraps to | above the terminal, below nothing |
| `SOURCE_DATE_EPOCH` | `__DATE__`/`__TIME__` become reproducible | owned by the preprocessor (`preprocessor.md`) |

They are listed here, and printed on the pages where they matter, because an
undocumented environment variable is a hidden option.

## Cross-platform

The CLI adds **no** platform branch, and that is a rule rather than a hope
(`architecture.md` states it: the whole platform branch is three files, all in
`support`). Two things here are platform questions, and both are asked of
`support/term`:

- **Is the stream a terminal?** already there (`stdoutSupportsColor`).
- **How wide is it?** `COLUMNS`, then `ioctl(TIOCGWINSZ)` on POSIX and
  `GetConsoleScreenBufferInfo` on Windows, then 80 — one function, in the one
  module allowed to contain the two headers. A driver file that tested
  `_WIN32` to size a column would be the fourth such file, and it would need an
  argument rather than an accident.

The help text stays **ASCII-only** (enforced by a test): a Windows console with a
legacy code page renders UTF-8 unpredictably, and a box-drawing character in a
help page is not worth a broken page.

## What this work does not own

Two things were found while writing this record and are **not** fixed here,
because they are the target table's and not the command line's:

1. **"Host" is a constant.** `driver` decides whether a link crosses a target
   boundary with `request.target.triple.text == sema::defaultTarget().triple.text`,
   and `defaultTarget()` is `kDefaultTriple` — `x86_64-unknown-linux-gnu`,
   written down. On a macOS or Windows host that comparison is false, so
   `mincc build hello.mx` refuses to link with "needs `--linker` and
   `--sysroot`" for a program aimed at the machine it is running on. The fix is
   the host triple as the default target, which is `sema/target.h`'s decision.
2. **`arm64-apple-darwin` is refused.** That is LLVM's own spelling of an Apple
   Silicon host, and the table states `aarch64` and not `arm64`, so a name LLVM
   produces for every M-series machine — the value `--version` now prints under
   `host:` — cannot be passed to `--target`. Measured:

   ```
   $ mincc check --target arm64-apple-darwin a.mx
   mincc: error: unknown target 'arm64-apple-darwin': unknown architecture `arm64`;
   the default is 'x86_64-unknown-linux-gnu'
   $ mincc check --target x86_64-apple-darwin a.mx
   (accepted)
   ```

Both are one-file changes in `sema/target.h` with tests, and both are the kind of
gap this document exists to make visible rather than to hide behind a
`--version` line that prints a host the compiler cannot actually target.
