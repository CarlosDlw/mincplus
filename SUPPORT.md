# Getting help

minc+ is a young project: it has a small surface, everything about it is written
down, and a question that is not answered by the documentation is a bug in the
documentation.

## Where to ask

| What | Where |
| --- | --- |
| "How do I write X?", "what does this diagnostic mean?" | [Discussions](https://github.com/mincplus/mincplus/discussions), or an issue if Discussions is not enabled |
| "This file does not compile and I think it should" | an issue, using the **Bug report** template |
| "The compiler crashed, or produced a program that behaves differently from what the language says" | an issue — and if you believe it is exploitable, [SECURITY.md](SECURITY.md) instead |
| "This feature is missing", "this should be spelled differently" | an issue, using the **Feature request** or **Language proposal** template |
| "Here is a change" | [CONTRIBUTING.md](CONTRIBUTING.md) |

## What to include

The single most useful thing you can paste is **the smallest file that shows the
problem**, plus the command and the two outputs you expected and got. `mincc`
keeps its diagnostics deterministic in source order, so a report that fits in a
screen is usually the whole bug.

For anything about a build or a wrong answer, `-vV` first:

```console
$ mincc -vV
mincc 0.1.0
binary:         mincc
host:           x86_64-unknown-linux-gnu
default target: x86_64-unknown-linux-gnu
LLVM:           22.1.8
```

## Where the answers already are

- **The language reference** — `website/` in this repository, `make docs` to build
  it. What a `.mx` file means, page by page, with anything unimplemented marked
  as such.
- **`README.md`** — what the project is, what exists today, and how to build it.
- **`docs/architecture.md`** and `docs/architectures/` — why the compiler is
  shaped the way it is. These are the records to read before changing a stage.
- **`docs/roadmap.md`** — what is next, and in what order.
- **`examples/`** — the working corpus. Every file there is compiled by the test
  suite, so it is the specification that cannot go stale.
