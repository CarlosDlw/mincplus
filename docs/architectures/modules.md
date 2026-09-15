# The module system — constraints, not a design

`minc+` will have a module system, in which constants, functions and the
top-level types still to come are importable by name from other `.mx` files. It
is not designed yet, and this record does not design it. What it does is record
**the seams that must stay open** and **the questions that must be answered**, so
that the features built before it do not close a door.

It exists because of one observation: almost every language that added modules
late paid for it in a place nobody expected. C++20 modules changed the **mangled
name** of every exported function — the module is part of the symbol
([Stack Overflow, *Why is the module name part of a mangled name*][cpp-mangle]) —
so a feature that looks like syntax turned out to be ABI. Go forbids import
cycles, which is a rule about the *graph*. Rust forbids cycles between crates but
allows them between modules. Those are not details of a parser; they are
decisions that reach down to the symbol table.

A record that lists them early is cheaper than a rewrite that discovers them.

## The seams today's code must keep open

Five. Each is small, each is cheap to keep open now, and each is expensive to
reopen later.

### S1 — a symbol name is a *function* of a declaration, never the declaration's spelling

Today the symbol for `fn i32 add()` is `add`, and for a file-scope binding it will
be the binding's name. That is correct while a unit is the whole world, and it
stops being correct the moment two modules can each define `add` — which is
exactly the case C++20 mangling exists for, and which **`minc+` cannot avoid**,
because it promises full C interop in both directions and a flat symbol namespace
cannot hold two modules' private names.

The seam already exists in the right shape: `Lowering::linkageName(def)` is the
one place a symbol is spelled, and it currently returns the interned name. When
modules land it computes a mangled name instead, and **the only other thing that
changes is who calls it**. The rule to keep: a stage asks for a *linkage name*;
it never reads `Def::name` and calls that the symbol.

There is a second half, and it is the half that makes interop survive the change:
**`extern` always means the bare symbol.** A declaration written `extern` names a
symbol that somebody else chose — the C runtime's, a library's — so it is never
mangled. `extern let environ: *str;` must keep resolving to the flat `environ`
whatever `minc+` does to its own names, which is precisely what `extern "C"` does
in Rust and C++, and what Zig's `extern`/`export` split does
([Rust FFI][rust-ffi], [Zig's exported-symbols discussion][zig-462]).
`extern.md` already deferred this with "a `@symbol` attribute is the mechanism
for the day a name needs two spellings" — that day is the module system's, and
this is the constraint it must satisfy.

### S2 — the file scope is read as *the enclosing named unit's scope*

`resolve` collects file-scope items into `ScopeKind::File`. Decision A of
`resolve.md` says a file-scope name is visible "as in Go's package block" — and a
**package block is per package, not per file**. So the rule already in the record
is written for a larger unit than the one it currently runs on.

That is the right way round, and it needs saying out loud: today a module is
exactly one file, so the file scope *is* the module scope, and the collect pass
and the `ItemTree` are per file because the file is the unit of *compilation*.
When modules arrive, the module becomes the unit of *visibility* and a file its
part, and the change is "collect the union, then resolve", not "invent a scope
kind". The item tree keys on the file, so the editor's cache continues to work.

### S3 — `#include` is for C; `import` is for `.mx`

Two mechanisms that both bring names into a unit and must not be confused:

| | `#include` | `import` |
| --- | --- | --- |
| Kind | textual, preprocessor | name-based, language |
| For | **C headers** | **`.mx` modules** |
| Names it brings | whatever text declares, with C's linkage rules | the module's exported names |
| Repeatable? | yes — that is why C needs include guards | yes, and idempotent by construction |

C's whole header apparatus — guards, `static const`, internal-linkage defaults —
exists because C has no module system and includes text. Once `minc+` has
`import`, including a `.mx` file is the wrong tool for sharing a declaration, and
the rule should be that it is not the idiom. Recording it now is what keeps the
language from acquiring a header convention it will then have to deprecate.

The transitional cost is real and worth naming: **until both `static` and
`import` exist, a `const` in a shared `.mx` file included by two units is two
external symbols.** The answer is `static`, which is C's answer, and which is
already in this project's plan (`extern.md`, *Not a linkage change*).

### S4 — type identity must be *nominal* across modules, and `TypeStore` interns by structure

This is the seam nobody looks for, and it will bite when `struct` lands.

`TypeStore` interns a type **by structure**, on purpose: `int`, `i32` and
`signed int` must be one `TypeId`, and that is what lets the IR and the linker
compare two signatures. For scalars that is exactly right — they are the same
type.

For an aggregate it is wrong, and it is wrong in a way that only shows up across
modules. Two modules each defining `struct Point { x: i32; y: i32; }` must be
**two** types: a function taking one must not silently accept the other, and a
`Point` field in one must not be a `Point` field the other can write. That is
nominal typing, and it is what C, C++, Rust, Swift and Zig all do for aggregates.

The seam: **aggregate identity is a name plus the declaring module, and only
scalar and pointer types may intern by structure.** Since `struct` does not exist
yet, nothing is closed today; the point of writing it down is that the day it
lands, `TypeStore` grows a nominal path in one place rather than being relied on
to intern structurally everywhere.

### S5 — `static` is a finer granularity than module visibility, and may vanish into it

`static` on a binding means *internal to this unit*; `pub`/`private` means
*visible to importers of this module*. Two different axes — linkage and
visibility — but the same *practical* effect when one module is one file.

That is a genuine fork the module record has to settle: if a module is always
exactly one file, `private` and `static` denote the same set of names and one of
them is redundant. The recommendation to keep open is that they stay two
words, because `static` keeps its C meaning (the linker must not see it, which
matters for interop and for symbol collisions) while `private` is a language-level
filter over lookup (`resolve.md` decision F). A language that is going to link
against C will keep wanting the first.

## The questions the module system must answer

Each with what the market does, because the answers are correlated and picking
one constrains the others.

| # | Question | Go | Rust | Zig | C++20 | Swift |
| --- | --- | --- | --- | --- | --- | --- |
| Q1 | Is a module one file or many? | package = dir of files | crate = many modules, module = a file by default | one file (a module is a file; packages group them) | a module unit is a file; a module may have partitions | module = a build target, many files |
| Q2 | Visibility default? | exported iff the name is Capitalized | **private**, `pub` to export | public within the module; `pub` for the package API | private, `export` to export | internal by default, `public` to export |
| Q3 | Cycles between modules? | **forbidden** | forbidden between crates, **allowed** between modules of one crate | **allowed** | **forbidden** (ill-formed) | allowed within a module |
| Q4 | Reference syntax | package-qualified, no import statement needed for qualified | path `a::b::c`, `use` to bring in | `@import("x")`, `.field` access | `import m; m::name` | qualified by module, `import` |
| Q5 | Where does the interface live? | export data in the object | metadata in the crate artifact | **the source is the interface** | the module interface unit | a `.swiftmodule` file |
| Q6 | Is the module in the symbol name? | n/a (own toolchain) | yes (v0 mangling) | yes (internal names hashed) | **yes** | yes |
| Q7 | How do C symbols stay flat? | cgo, a generated shim | `#[no_mangle]` / `extern "C"` | `export fn` / `extern` | `extern "C"` | `@_cdecl` |

The correlations that matter:

- **Q3 and Q1/Q5 are one decision.** Go can forbid cycles because it compiles a
  package set together and knows the whole graph; Zig can allow them because the
  interface is the source and evaluation is lazy; C++20 forbids them and its
  users complain that "circular imports do not work", which is a real cost
  ([Lobsters thread][cpp-modules-cost]). For `minc+`, Q3 should be decided
  together with Q5, not before it.
- **Q6 is a consequence, never a choice.** Whatever Q1 and Q4 are, two modules may
  hold one name, so the module must be in the symbol unless Q2's answer makes a
  name unambiguous. This is S1, arriving as a table row.
- **Q7 is not optional for this language.** `minc+` promises C interop in both
  directions, so whatever mangling Q6 implies, the flat spelling must be reachable
  and `extern` is where it is spelled.

### What a source-based interface (Zig's Q5) would mean for constants

Worth stating, because it is the cheapest option and `globals.md` makes it work:
if the interface is derived from the source rather than stored in a new binary
artifact, an importer re-evaluates the exporting module's constant initializers.
That is only sound because **every file-scope initializer is a constant**
(`globals.md`, decision 2) — evaluating one has no side effects, no ordering, and
no work. Had runtime initialization been allowed, a source-derived interface
would have had to *execute* the exporter to learn a value, and that is the whole
fiasco again at module granularity.

So decision 2 is not merely compatible with a module system: it is what makes the
cheapest module interface viable.

## What earlier records already require of it

| From | The requirement |
| --- | --- |
| `resolve.md` decision A | file-scope names are order-independent "as in Go's package block" — **per module**, so the collect pass grows from a file's items to a module's |
| `resolve.md` decision F | visibility `pub`/`private` **filters lookup** and does not nest scopes, so it does not change linkage |
| `extern.md` | `extern` is a declaration word, `minc+` mangles nothing *today*, and the `@symbol` attribute is the deferred mechanism for a name with two spellings |
| `extern.md` | a declaration and a definition are one entity, one `DefId`, one canonical identity — which is what a module boundary keys on |
| `globals.md` decision 5 | a file-scope binding is externally linked and visibility is **not** a linkage default; `static` is what narrows |
| `globals.md` decision 2 | every file-scope initializer is a constant, so a module's exported constants need nothing to run |
| `memory.md` decision 15 | a `const` is an **object** with an address, not an inlined spelling — which is why an exported constant needs a symbol and cannot be instantiated per importer the way a Rust `const` is |
| `architecture.md` | `support/` is LLVM-free; a module resolution stage cannot include LLVM either |

## What is deliberately not decided here

Everything in the table above, plus: whether `import` binds a name, a namespace or
nothing (Q4); whether a module is a file, a directory or a declared unit (Q1);
whether the interface is source-derived or an artifact (Q5); what the mangling
scheme is, though it should be *stable and specified* rather than incidental, and
probably versioned, because a compiler that changes its mangling silently breaks
the artifacts it produced yesterday; whether `pub` is spelled `pub`; and how a
module names a C library to link (the roadmap's "Declaring links to libraries
from source").

## References

- cppreference, *Modules (since C++20)* — interface units, `export`, and the fact
  that a module's declarations reach an importer through an artifact rather than
  text: <https://en.cppreference.com/w/cpp/language/modules>
- Why C++20 modules put the module name in the mangled symbol — the surprise this
  record is built to avoid: <https://stackoverflow.com/questions/79904504/>
- The cost of C++20's no-cycles rule, in users' words — "circular imports do not
  work": <https://lobste.rs/s/en1p2u/>
- The Go specification, *Import declarations* — a package's import graph is
  acyclic, and an import cycle is an error:
  <https://go.dev/ref/spec#Import_declarations>
- The Rust Reference, *Modules* and Cargo's crate graph — cycles are forbidden
  between crates and allowed between modules of one crate:
  <https://doc.rust-lang.org/reference/items/modules.html>
- The Zig language reference, *Importing Files* — `@import` by module name, and
  "if module A depends on module B, then any source file in module A can import
  the root source file of module B": <https://ziglang.org/documentation/master/>
- Rust's FFI, *`extern "C"` and `#[no_mangle]`* — how a mangled language keeps a
  flat namespace reachable:
  <https://doc.rust-lang.org/nomicon/ffi.html>
- Zig issue 462, *different way to specify exported symbols* — the `export` /
  `extern` split and why the exported name is the linker-visible one:
  <https://github.com/ziglang/zig/issues/462>

[cpp-mangle]: https://stackoverflow.com/questions/79904504/
[cpp-modules-cost]: https://lobste.rs/s/en1p2u/
[rust-ffi]: https://doc.rust-lang.org/nomicon/ffi.html
[zig-462]: https://github.com/ziglang/zig/issues/462
