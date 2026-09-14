# AST lowering and name resolution design

Decision record for the two stages between the syntax tree and type checking:
`src/ast` (lowering and structural validation) and `src/resolve` (scopes and
names). It captures what production front ends actually do, where they disagree,
what `minc+` takes from each, and every choice that is now fixed — so the next
person can tell a decision from an accident.

It builds on three earlier decisions and does not re-open them:

- `lexer.md` decision 12: identifiers are ASCII, so nothing here does locale
  case folding.
- `parser.md`: the tree is **lossless, untyped and semantic-less**, and every
  type name is recognized by *position*, never by asking a symbol table. This
  document is where that second half has to be paid for, and it is.
- [`../architecture.md#the-pipeline`](../architecture.md#the-pipeline): the
  stage order, and the contract every stage obeys — artifact in, artifact out,
  errors as values, nothing prints.

Implementation order is in [`../roadmap.md`](../roadmap.md) § 4. This document
fixes the *design*; the roadmap tracks what is built.

## The one thing every reference agrees on

Rust, Roslyn, TypeScript, Swift, Clang, Go and Zig are seven different
compilers, and on this stage they agree on five things:

1. **The tree is not the analysis IR.** Every one of them lowers the
   syntax tree into something else before analysing it: rustc lowers the AST to
   **HIR**, rust-analyzer condenses a tree into an **item tree** and a **def
   map**, Roslyn builds a **declaration table** and then a bound tree, Zig's
   `AstGen` produces **ZIR** and only then does `Sema` run, Go nodes the AST
   into IR. The reason is the same everywhere: the tree is shaped for fidelity,
   analysis wants shape-for-lookup.
2. **Name resolution is its own phase, and it finishes.** rustc has a crate
   (`rustc_resolve`) and calls full resolution *late* resolution; Roslyn has a
   binder distinct from both parser and semantic model; TypeScript has the
   binder. Types come after, because a body cannot be checked against
   declarations that are not all known yet.
3. **A name is bound to a declaration, not to a string.** Resolution produces an
   edge from every use to the declaration it denotes, and every later stage
   reads that edge instead of re-doing a lookup.
4. **Resolution is total and resilient.** It answers *every* name. The answer
   may be "unresolved, and here is the reason", but it is still an answer —
   the same shape as the lexer's flags and the parser's recovered trees.
   Nothing throws, nothing stops at the first failure.
5. **What is derived is a value, and it is cached by revision.** rust-analyzer
   states the invariant outright: *"typing inside a function's body never
   invalidates global derived data."* Their syntax tree is a value type and
   deliberately **does not store semantic information**, so an assist can
   transform a tree without dragging resolution along with it.

## What the references do, and where they disagree

### rustc — two phases, ribs, and a crate of its own

`rustc_resolve` is not a bullet inside type checking; it is a crate with a
two-phase contract. The first phase runs *during* macro expansion and resolves
only what expansion needs (imports, macro names). The second — `rustc_resolve::late`,
after the whole crate is parsed — resolves everything, and a failure there is a
compiler error. Names are separated into **namespaces** (types, values, macros,
lifetimes) with **an independent rib stack per namespace, built in parallel**.
A *rib* is pushed whenever the visible set can change: a block, a function
boundary, a module, a `let` (shadowing), and — because macros must not capture
— an expansion boundary. Lookup walks ribs innermost-outward.

The detail worth stealing: **items may be used before they are encountered**, so
*"every block needs to be first scanned for items to fill in its Rib"* — the
collect phase exists because the language is order-independent, not because the
implementation found it convenient.

### rust-analyzer — an item tree and a def map, because the editor asks on every keystroke

Two structures, both of which `minc+` needs:

- **`ItemTree`** condenses one syntax tree into a *summary* that is
  **stable over modifications to function bodies**.
- **`DefMap`** holds the module tree of a crate and its scopes.

The division is the whole trick. A body edit changes a body; it does not change
the file's list of declarations, so the item tree — and therefore everything
derived from it, including the scope that bodies are resolved against — is
*unchanged*, and nothing global is recomputed. Everything is a query over input
facts, with the mediating structure (the item tree) placed deliberately between
the tree and the analysis, which is exactly what makes the caching possible
rather than accidental.

They also name the *source→definition* mapping: resolve the parent syntax node
to its parent item, then ask that item for its syntax children, then pick ours
out. They call it an unnamed "uber-IDE pattern" and note it is present in
Roslyn and Kotlin too. It is the primitive behind go-to-definition, and it is
the reason a binder is also the IDE's foundation rather than a compiler
internal.

### Roslyn — a declaration table between parse and bind

Roslyn's pipeline is `parse -> declaration table -> bind -> emit`. The
declaration table is a *summary of the declarations of a compilation*, built
without touching the trees, and the binder is what consumes it. The tree itself
is never modified to carry meaning: the semantic model is a separate,
immutable snapshot that can be re-created cheaply.

### Swift — unqualified lookup, as a library of its own

Swift has been extracting unqualified name lookup out of the compiler into
`SwiftLexicalLookup`, a library over swift-syntax that answers "which names does
this scope introduce, and which are visible here" with no type checker present.
That is the same boundary this document draws: lookup is one question, and it
should be answerable — and testable — on its own.

### C — four namespaces, five scopes, and the point of declaration

The contract this stage inherits, in the standard's own terms (C17 6.2.1-6.2.3):

- Four **name spaces** (6.2.3): **label**, **tag**, **member**, and **ordinary
  identifiers**. A name in one does not collide with the same name in another.
- Four **kinds of scope** (6.2.1): **block**, **file**, **function** (labels
  only), and **function prototype**.
- **Point of declaration**: a name's scope begins *just after its declarator*
  and *before its initializer*, which is what makes `unsigned char x = x;`
  initialize the new `x` with its own indeterminate value. A label, alone, is in
  scope in the whole function, before and after its own line, ignoring blocks.
  A tag's scope begins immediately after the tag appears, which is what lets
  `struct Node { struct Node *next; };` refer to itself.
- C has **no struct scope**: names declared inside a struct declaration belong
  to the enclosing scope, and only *members* get their own name space.

### Go — the package block, and order-independent declarations

The other answer to "when does a name become visible": Go's file-scope names are
declared in the **package block**, so order does not matter and two functions may
call each other with no prototypes. It costs a collect pass at file scope; it
buys an entire class of annotation that users never write.

### The theory

Néron, Tolmach, Visser and Wachsmuth, *A Theory of Name Resolution* (ESOP 2015)
— scope graphs — is the reference for the shape of the rules rather than of the
code. Its claim is that name binding is a **relation over a scope graph**, and
that a resolution algorithm should be derived from that relation rather than
accumulated. Two practical consequences are taken here: namespaces and scopes
are *data* (a graph that can be printed, queried and cached), and every rule
below is stated as a relation first and as an algorithm second.

## The design

### Where it sits

```
green tree
  --lower-->      AST + item tree + bodies      (src/ast)
  --validate-->   the AST, structurally legal    (src/ast)
  --collect-->    the file scope and its defs    (src/resolve)
  --resolve-->    NameRefs and errors            (src/resolve)
  then sema, which reads all of it and searches no scope
```

Three modules, one boundary each:

| Module | Reads | Produces | Never does |
| --- | --- | --- | --- |
| `src/ast` (lower) | green tree | lowered AST, item tree, bodies | any decision about meaning |
| `src/ast` (validate) | lowered AST | structural errors | any lookup |
| `src/resolve` | item tree, bodies | def map, scopes, `NameRef`s | types, const folding |

### Layer 1 — lowering, `src/ast`

**What the tree cannot give.** The green tree is built for fidelity, and four of
its properties are actively wrong for analysis: it carries trivia interleaved
with code; it wraps unparsed regions in `Error` nodes; its children are a
homogeneous sequence that is searched linearly (`childOfKind`) every time a
consumer wants a named field; and its nodes are **shared and hash-consed**, so
they cannot carry per-occurrence information without breaking the sharing they
exist for. The typed view in `include/syntax/ast.h` fixes ergonomics, not any of
the four.

**What lowering produces.** A `LoweredFile`:

```cpp
struct AstId { FileId file; std::uint32_t index; };   // a value, never a pointer

struct Node {
  NodeKind kind;       // what this is
  Span span;           // where it was written -- what every diagnostic points at
  std::uint32_t firstChild, childCount;   // a slice of the child array
  std::uint32_t extra; // name SymId, literal token index, operator kind...
};

struct LoweredFile {
  std::vector<Node> nodes;      // index 0 is the File node
  std::vector<AstId> children;  // the child-id array the slices point into
  std::vector<Item> items;      // the item tree; see below
  std::vector<Body> bodies;     // one per function body, node range into nodes
};
```

Four properties are deliberate:

- **Ids, not pointers.** `AstId` is `(FileId, index)` like every other identity
  in this compiler (`parser.md` decision 13). It survives a vector reallocation,
  it can be stored in a hash map, it is comparable, and it is revision-scoped
  for free.
- **A flat child array, not a linked structure.** One node is one fixed-size
  value; the tree is walked by index, and the whole thing is cache-friendly
  without any pointer chasing.
- **Every node has a span.** Including `Error` nodes, which is what lets a
  consumer say "this region is not understood" without re-walking the tree.
- **An ordinary `std::vector`, not an arena.** The green tree needs an arena
  because its nodes are *shared*; a lowered AST is deliberately not shared —
  each node has exactly one parent — so the reason for the arena is absent, and
  a per-file vector is simpler and cheaper to drop by revision.

**Lowering decides nothing.** It maps node kinds one to one, drops trivia,
interns every identifier into a `SymId` through `Session::symbols()` (keeping
the span, because a `SymId` alone loses where the name was written), keeps
literal text in place by offset, and stops. No name is looked up, no type is
guessed, no expression is folded, nothing is created that the source does not
contain. Where the source has a `return` with no `fn` above it, lowering says so
and validation decides whether that is legal — not the other way round.

**The item tree is the important part.** The file's items are its *file-scope*
declarations, each reduced to its **spelled signature**: the name, the kind, and
the parameter count — never the types, never the body. Bodies are lowered
separately and referenced by a `BodyId`. That split is what makes the caching
claim in the LSP section true, and it is cheap precisely because a signature
that is not yet type-checked is little more than a name and a span.

### Layer 2 — structural validation, `src/ast`

Well-formedness that needs **no symbol table**: where a construct is legal, and
whether the shapes the parser accepted are shapes the language allows.

| Check | Code | Why the parser could not do it |
| --- | --- | --- |
| `return` outside a function body | `ast-return-outside-function` | Needs the enclosing context, which the parser's event stream does not carry |
| A `let`/`const` with no binding name | `ast-missing-binding-name` | The parser inserts a zero-width missing token rather than inventing an error |
| More parameters than the language allows | `ast-param-limit` | The limit is a language rule, not a grammar rule |
| A `...` parameter that is not last | `ast-variadic-not-last` | Same: the grammar accepts the shape, the language forbids the order |
| A `fn` inside a `fn` (if the language forbids it) | `ast-nested-function` | Legal-looking syntax, illegal context |
| A `break`/`continue` with no enclosing loop (when they land) | `ast-misplaced-break` | Same |

It is a pass of its own rather than code inside the lowerer, for the reason
Rust gives: rustc's guide says in as many words that *AST validation* "is a
separate AST pass that visits each item in the tree and performs simple checks",
that it "doesn't perform any complex analysis, type checking or name
resolution", and that "when this pass is done, the compiler runs the crate
resolution pass" — validation, then resolution, in that order. The checks are
language surface that grows with every construct, while lowering is a mechanical
mapping that should stay boring. It runs on the **lowered** AST, so it never
sees trivia and never has to reason about `Error`-node nesting.

One rule keeps it honest: **a region that the parser already reported is not
reported again here.** Where the lowered tree has an `Error` node, validation
and resolution both skip it. Two diagnostics for one mistake is worse than one.

### Layer 3 — scopes and the def map, `src/resolve`

The data, in the order it matters:

```cpp
enum class Namespace : std::uint8_t { Ordinary, Tag, Label, Member };  // C 6.2.3
enum class ScopeKind : std::uint8_t { File, Function, Block,             // + later:
                                      FunctionPrototype, Loop, Switch };

struct DefId { FileId file; std::uint32_t index; };   // per file, so a header's
struct ScopeId { std::uint32_t index; };              // defs survive a .mx edit

struct Def {
  Span span; SymId name; DefKind kind; ScopeId scope; Namespace ns;
  Linkage linkage;          // external / internal / none -- C 6.2.2
  DefId canonical;          // the identity of the thing declared: `self`, or
                            // the first declaration of a repeated name
  DefId nextRedundant;      // the same name declared again in this scope
};

struct Scope {
  ScopeId parent; ScopeKind kind; Span span;
  std::vector<DefId> names[4];   // one table per namespace, in declaration order
  std::vector<std::uint32_t> index[4];  // SymId-sorted, built lazily
};
```

- **Four namespaces from day one.** Only `Ordinary` is reachable today, and the
  other three cost one array each. Adding a namespace later would reshape every
  scope, every lookup, every diagnostic and every cache key; paying for it now
  is one line and a comment. One of them is not like the others: a **member**
  name is never found by ordinary lookup at all — `a.field` looks in the type of
  `a`, not in the scope chain — so `Member` is a table that exists to be
  *filled*, and the lookup that reads it arrives with `struct`.
- **`DefId` is per file; scopes are per unit.** A translation unit is one file
  plus everything it included, so its scope tree spans several files, but a
  definition belongs to the file it was written in. That is what lets a header's
  definitions stay valid while a `.mx` body changes.
- **`canonical` and `nextRedundant` answer two different questions.** The chain
  is the *sites* — a header included twice declares its functions twice, and the
  IDE wants each one — while `canonical` is the *identity* every lookup answers.
  They point in opposite directions for a non-function redeclaration, which is
  exactly why "which def is this name" cannot be read off the chain. Everything
  below this stage keys its maps on `canonical`, so two declarations of one
  function get one type in `sema` and one `llvm::Function` in `ir`
  ([`extern.md`](extern.md)).
- **Tables are ordered vectors, not hash maps.** Lookup uses the sorted index
  (binary search); *iteration* uses declaration order, which is what diagnostics
  and suggestions need. An `unordered_map` would make the output of a
  diagnostic depend on the hash of an integer, and "the same input reports the
  same errors in the same order" is a property this compiler already promises
  elsewhere.
- **`nextRedundant` is a chain, not an overwrite.** C allows a function to be
  declared many times; a header included twice declares its functions twice. The
  scope table keeps the canonical `DefId` and the chain keeps the rest, so the
  backend can see every declaration and the IDE can show every site.

**Lookup** walks the `ScopeId` parent chain, testing the requested namespace's
table at each step; the first hit is the innermost declaration. It does not stop
there when threading a shadow warning: the *next* hit down the chain is exactly
what `-Wshadow` wants, and it costs one more step that the walk was going to
take anyway.

**The two phases.**

- **Collect** walks the item trees — never bodies — in the order the
  preprocessor read the files (the include record already knows that order, so
  it is reused rather than re-derived). It creates one `Def` per item, inserts
  it into the file scope, and reports redeclaration. Nothing is resolved yet.
  This phase exists because file-scope names are order-independent (see the open
  decision below): a body may call a function written after it.
- **Resolve** walks bodies. Parameters enter the function scope *before* the
  body is walked; the function's own name entered the file scope during collect,
  so recursion needs no special case. Block scopes are pushed as the walk enters
  a block and popped as it leaves, and each `let`/`const` adds its name as the
  walk passes it — block scope is point-of-declaration, so the walk needs no
  pre-scan. Every name use produces one `NameRef`.

```cpp
struct NameRef {
  Span span;
  DefId target;          // kInvalidDef when unresolved
  UnresolvedReason reason;  // NotFound / WrongNamespace / InErrorRegion
};
```

**References live beside the AST, not in it.** `NameRef`s are a parallel array
indexed by body, not fields on `Node`. This is rust-analyzer's decision applied
— *a syntax tree is a value and must not store semantic information* — and it
has three concrete payoffs here: the AST stays hashable, so the item tree can be
compared cheaply; a formatter or refactor can rewrite the AST without dragging
resolution along; and a revision's resolution can be dropped without touching
the tree.

### The item tree, and the invariant that makes the LSP work

The unit's scope is a function of the **set of item trees**, in include order —
nothing else. So:

```
edit a function body
  -> that file's LoweredFile changes, its ItemTree does not
     -> the set of item trees is unchanged
        -> the def map, the scopes and every other body's refs stay valid
```

An edit to a *signature* changes one item tree, so the file scope is rebuilt —
which is correct, because the set of visible names really did change. An edit to
a *header* invalidates every file that included it, which is also correct and is
why the include record is part of the key.

Comparison is by content: `ItemTree` carries a hash and an `operator==`, and the
cache uses **the hash to find candidates and `==` to confirm**. A hash alone
would be a correctness bug waiting for a collision, and this is exactly the kind
of thing that never shows up in testing.

### Errors and recovery

The stage returns values — `errors`, `warnings`, and one `NameRef` per name use
— and never prints, throws, or stops early. The codes, each with one table row
like `pp_error.h`:

| Code | Severity | Means |
| --- | --- | --- |
| `resolve-unknown-name` | error | No declaration in this namespace is visible under that name |
| `resolve-redeclaration` | error | The same scope and namespace declares two incompatible things |
| `resolve-unused-entity` | warning | A declaration nothing refers to |
| `resolve-shadowed-name` | warning | A declaration hides another one that is still in scope |
| `ast-return-outside-function` and friends | error | The structural checks of layer 2 |
| `resolve-limit-defs`, `resolve-limit-scopes`, `resolve-limit-refs` | error | A budget was reached (see below) |

Three rules that are easy to get wrong:

- **No cascades.** An unresolved name is an `Unresolved` target, not an error
  that poisons the expression around it. One name, one diagnostic. What the
  surrounding expression means with an unknown operand is a question for `sema`,
  and `sema` answers it against an `Unresolved` target silently — the reader has
  already been told.
- **Wrong namespace is a note, not a second error.** If `Foo` was not found as a
  value but a tag by that name exists, the one diagnostic carries a note saying
  so. Reporting both "unknown name" and "it is a type" is how a compiler teaches
  people to stop reading its output.
- **Suggestions are best-effort and bounded** (next section).

### Typo suggestions, and why they are bounded

An unknown name is the single most common diagnostic in a language without
`auto`-everything, and rustc's `lookup_import_candidates` — which will even
*load crates it has not loaded yet* to find an import worth suggesting — shows
how far the reference implementations go. The bound is what matters here,
because this runs on every keystroke in an editor:

- candidates are the names **visible** in the failing scope chain, in the
  requested namespace only — never the whole unit, and never a name the user
  could not have used;
- candidates are filtered by length (`|len(a) - len(b)| > 2` is rejected before
  any distance is computed);
- the distance is Damerau-Levenshtein with an early exit at distance 2, so no
  pair costs more than a banded walk;
- the candidate set is capped at `kMaxSuggestionCandidates`;
- ties are broken by declaration order, then by `SymId`, so the suggestion is a
  function of the input and not of the insertion order of a container.

### Determinism

- Diagnostics are emitted in `(file, offset)` order, then by code.
- Scope and def tables iterate in **declaration order**; the sorted index is a
  lookup structure and is never iterated for output.
- No `unordered_*` iteration reaches a diagnostic, a suggestion or a dump.
- Names compare as `SymId` integers, never with a locale-sensitive collation.
- Typing only trivia (whitespace, a comment, a line break) leaves every
  resolution identical. That is a test, not an aspiration.

### Bounds and adversarial input

Same rule as the lexer and the preprocessor: every hazard is bounded by an
always-on limit with a code and a test, checked **before** the allocation or the
insertion, never after. The new ones belong in `support/limits.h` with the rest:

| Limit | Bounds |
| --- | --- |
| `kMaxDefsPerUnit` | A file that declares a million things; also the size of every scope table |
| `kMaxScopesPerUnit` | Deeply nested blocks with a declaration each |
| `kMaxNameRefsPerUnit` | A body that uses names a million times |
| `kMaxSuggestionCandidates` | The cost of one failing lookup in an editor |
| `kMaxScopeDepth` | The depth the scope-chain walk will follow before answering `NotFound` |

Two structural points:

- **The body walk uses an explicit stack, not recursion.** Recursion depth would
  be input-controlled, and a stack overflow is a crash, not a diagnostic. The
  scope chain walk is a loop for the same reason.
- **Lowering may recurse, because it is bounded by construction**: it walks a
  tree the parser already bounded with `kMaxNestingDepth`. The bound is asserted
  at the entry point rather than assumed, so a future producer of green trees
  cannot quietly remove it.

### Cross-platform

This stage touches no filesystem, no locale, no clock and no platform API: it
reads trees and returns values. Its inputs are already-normalized `uint32`
offsets and already-interned `SymId`s, and its only platform-shaped input —
`FileId`, and the path identity behind it — comes from `support`. There is
therefore no platform branch in `src/ast` or `src/resolve`, and nothing here can
make Linux and Windows disagree about which name a program means.

### Layers and files

| File | Owns |
| --- | --- |
| `ast/node.h` | `NodeKind`, `Node`, `AstId` — the lowered representation |
| `ast/ast.h` | `LoweredFile` (nodes, children, bodies), `Item` / `ItemTree`, and the walk helpers |
| `ast/lower.h` · `lower.cc` | Green tree → `LoweredFile`, mechanically |
| `ast/validate.h` · `validate.cc` | The structural checks, and nothing else |
| `ast/ast_error.h` · `ast_error.cc` | `AstErrorCode` + its one table |
| `ast/dump.h` · `dump.cc` | The textual form of the lowered AST and the item tree (for tests and `--ast`) |
| `resolve/def.h` · `def.cc` | `DefId`, `Def`, `DefKind`, `Namespace`, `Linkage` |
| `resolve/scope.h` | `ScopeId`, `Scope`, the name tables and their index |
| `resolve/map.h` | `DefMap`: the unit's scopes, defs and references |
| `resolve/resolve.h` · `resolve.cc` | Both phases: collect the file-scope items, then resolve the bodies |
| `resolve/lookup.cc` | The scope chain, the name tables, and `NameRef`s |
| `resolve/suggestions.cc` | The bounded typo search |
| `resolve/source_to_def.cc` | Syntax node → def, and offset → def: the IDE primitive |
| `resolve/store.h` · `store.cc` | The `(FileId, revision)` cache and its invalidation |
| `resolve/dump.h` · `dump.cc` | The scopes/defs/refs views `mincc resolve` prints |
| `resolve/resolve_error.h` · `resolve_error.cc` | `ResolveErrorCode` + its one table |
| `resolve/resolve_report.h` · `resolve_report.cc` | Both tables → `DiagBag`. The only target here that links `minc_diag` |
| `driver/resolve_command.h` · `resolve_command.cc` | The command line, and only the command line |

Targets: `minc_ast`, `minc_resolve`, `minc_resolve_report`. Same rule as
everywhere else — the stage libraries never link diagnostics, and the driver is
the only code that prints or decides an exit code.

### The `resolve` command

```
$ mincc resolve --refs examples/002_variables.mx
# examples/002_variables.mx  (scopes 2, defs 5, refs 1, 0 error(s), 0 warning(s))

  scopes
    #0  file  22..98  (root)
    #1  function  36..97  parent #0

  defs
    #0  file#0  ordinary  const  true  refs 0  [predefined]
    #1  file#0  ordinary  const  false  refs 0  [predefined]
    #2  file#0  ordinary  fn  main  refs 0  examples/002_variables.mx:2:8
    #3  function#1  ordinary  let  x  refs 1  examples/002_variables.mx:4:7
    #4  function#1  ordinary  let  y  refs 0  examples/002_variables.mx:5:7

  refs
    examples/002_variables.mx:6:10  x  -> defs#3
```

The file scope and the function scope are the two here: a function's parameters
and its body's outermost block share one scope, so a body does not open a third
just to hold its locals. `true` and `false` are declarations too — the resolver
owns "what names are visible", and these are visible without being written — so
they are `[predefined]` defs rather than special cases in every later stage.

Modes, each of which exists to exercise one thing this document claims:

| Flag | Shows | Proves |
| --- | --- | --- |
| default | scopes, defs, counts | the scope tree and the tables |
| `--refs` | every name use and its target | resolution is total: every use has an answer |
| `--unresolved` | only the uses with no target, with reasons | the reason codes |
| `--at FILE:LINE:COL` | the declaration the name at that position resolves to | `source_to_def`, which is the go-to-definition primitive |
| `--ast` | the lowered AST | that lowering is what the document says |

Exit codes follow the rest of the driver: `Failure` when there are errors,
`Ok` when there are only warnings, `Usage` for a bad command line.

## Non-goals

- **No types.** `resolve` never computes, converts, or compares a type. Names in
  *type position* are not name uses yet; when user-defined types arrive they
  join the `Tag`/`Ordinary` lookup, and until then a `Type` node is not touched
  at all.
- **No constant evaluation**, no folding, no `sizeof` arithmetic — `src/sema`.
- **No macro expansion or hygiene.** Macros are the preprocessor's, and a macro
  name has been expanded away before this stage sees anything. Hygiene, when
  macros become expression-level, will need a *rib* per expansion — the
  structure above is shaped for it (a scope is data with a parent), but the rule
  is not implemented and is not claimed.
- **No modules, no visibility.** `pub`/`private` and imports are planned; they
  will add scopes and a second lookup step (a path through a module), not a new
  phase.
- **No overload resolution, no generics, no traits.** Nothing here ranks
  candidates; two visible declarations with the same name in one namespace are a
  redeclaration, not a set.
- **No printing, no exit codes, no file I/O.** Errors are values.
- **No lexer feedback, ever.** `parser.md` decision 9 stands: the parser never
  asks a symbol table whether a name is a type. Where C needs the typedef table
  to *parse*, the answer is bounded tentative parsing (decision 8) and
  resolution confirming afterwards — not a feedback channel that would make
  parsing non-local and un-cacheable.

## Decisions, now fixed

| # | Question | Decision |
| --- | --- | --- |
| 1 | Is there a lowered AST at all | **Yes.** The green tree is for fidelity; analysis gets a compact AST of its own |
| 2 | Identity of a lowered node | `AstId = (FileId, u32 index)` — a value, never a pointer |
| 3 | Storage of a lowered node | Fixed-size `Node` in a per-file `std::vector` + a flat child array. No arena: nothing is shared |
| 4 | Trivia and `Error` nodes | Trivia is dropped; `Error` nodes survive with their span so "not understood" is visible without re-walking |
| 5 | Who interns names | The lowerer, once, through `Session::symbols()`; every later stage compares `SymId` |
| 6 | What lowering is allowed to decide | Nothing. One syntax kind → one node kind; no lookup, no inference, no folding |
| 7 | Where semantics are stored | **Not in the AST.** `NameRef`s are a parallel array, so the tree stays a value and stays hashable |
| 8 | Item tree | Each file's file-scope declarations, reduced to **spelled signatures**; bodies are separate. The summary is stable under body edits |
| 9 | Structural validation | A pass of its own over the lowered AST, not code inside the lowerer and not part of resolve |
| 10 | Double reporting | A region the parser already reported is skipped by both later stages |
| 11 | Phases of resolution | Two: **collect** items, then **resolve** bodies. Never one |
| 12 | Namespaces | C's four (`Ordinary`, `Tag`, `Label`, `Member`) from day one, even though only `Ordinary` is reachable |
| 13 | Scope kinds | `File`, `Function`, `Block` now; `FunctionPrototype`, `Loop`, `Switch` reserved |
| 14 | `DefId` scope | Per file. Scopes are per unit, so a header's defs survive a body edit |
| 15 | Name tables | Declaration-ordered vectors per namespace, with a lazily built `SymId`-sorted index for lookup |
| 16 | Redundant declarations | Allowed for functions (C compatibility) and kept as a chain from the canonical `Def`; two incompatible declarations in one scope are a redeclaration error |
| 17 | Lookup | Innermost-outward over the scope chain, first hit wins; the walk continues one step when a shadow warning was requested |
| 18 | Resolution is total | Every name use gets a `NameRef`, unresolved ones carry a reason. Nothing throws, nothing stops early |
| 19 | Cascades | An unresolved name is not an error for anything around it. One name, one diagnostic |
| 20 | Suggestions | Bounded (visible names only, length filter, banded distance, capped candidates, deterministic ties) |
| 21 | Determinism | Diagnostics in `(file, offset)` order; tables iterate in declaration order; no hash iteration in output |
| 22 | Caching | Keyed `(FileId, revision)`, with the invariant "a body edit invalidates only that body"; the item tree is compared by hash **and** equality |
| 23 | Stack safety | The body walk and the scope walk are iterative; lowering recurses only inside the parser's depth bound, asserted at entry |
| 24 | Limits | Every one is always on, checked before the allocation, with a code and a test |
| 25 | Reporting | `minc_ast` and `minc_resolve` link no diagnostics; `minc_resolve_report` converts both tables |
| 26 | The IDE mapping | `source_to_def` is built here, not later: resolve the parent, then the child. Go-to-definition is this stage's output, not an add-on |

## Decisions the language owns

These are **semantics**, not implementation, so they belong to the language; they
are recorded here *and* in the `README.md` language checklist (section *Scopes
and names*), which is the user-facing copy. Each was open when this record was
written and was decided in favour of the recommendation the algorithm above
assumes, because that algorithm depends on it. The rejected answer is kept beside
each one: it is the reason, and it is what a later reader needs in order to
reopen the question knowingly rather than by accident.

| # | Question | Decision | If the other way |
| --- | --- | --- | --- |
| A | When does a file-scope name become visible? | **Order-independent** (Go's package block): a call may name a function defined further down, and recursion needs no prototype | With C's point-of-declaration, collect collapses into the body walk — but mutual recursion then needs prototypes, and the language inherits C's largest paper cut |
| B | Does a `let` initializer see the new binding or the outer one? | **The outer one** (Rust): `let x = x + 1;` shadows and is unambiguous | With C's rule, `let x = x;` reads an uninitialized variable — a footgun this language rejects elsewhere (implicit octal, `char` signedness) |
| C | May a `fn` be declared inside a `fn`? | **No** (C): the grammar has no nested-function form, so there is nothing to collect and no code to spend | Nested functions add a scope and a closure question this stage would have to answer |
| D | Is shadowing an error, a warning, or silent? | **Allowed, warned on request** (`-Wshadow`) | Silent shadowing loses the diagnosis; an error breaks ordinary block-scoped code |
| E | Do `goto` and labels exist? | Not now; the `Label` namespace is reserved | `Label` becomes a fifth scope kind and function-wide visibility, as C has it |
| F | Does visibility (`pub`/`private`) create a scope? | Not now; it filters lookup rather than nesting scopes | Lookup grows a visibility predicate, and a `Def` grows a visibility field |

Nothing in the tables above changes if A–F are answered differently; only two
loops and one field do — which is why they were decided together with the code
that depends on them rather than left to block it.

## How the claims above are checked

| Claim | Checked by |
| --- | --- |
| Lowering loses nothing that matters | for every example: the set of `NodeKind`s and every node's span round-trip against the green tree; the concatenated leaf spans still tile the file |
| Lowering decides nothing | no test may see a `NodeKind` that has no corresponding `SyntaxKind`, and the mapping table is total over the node kinds |
| Validation is name-free | `minc_ast` does not link `minc_resolve`, and the structural suite uses no declaration at all |
| Every name gets an answer | `--refs` output for the whole corpus: `refs + unresolved == name uses`, asserted rather than eyeballed |
| Every code is reachable | one named input per `AstErrorCode` and per `ResolveErrorCode`, plus a sweep over both tables |
| No cascades | one unknown name in an expression produces exactly one diagnostic, at every nesting depth |
| Scopes nest correctly | a corpus per rule: block in block, parameter shadowed by a local, a name used after its scope closed, a tag and a value with the same spelling |
| The scope chain is a chain | for every `Def`, walking `scope` to the root terminates, and reaches `File` |
| Redeclaration is precise | the same name in two *different* scopes is not an error; two compatible `fn` declarations are not an error; two incompatible ones are |
| Determinism | the same input resolved twice, with a shuffled environment and a different locale, produces byte-identical output; trivia-only edits change nothing |
| The editor invariant | an edit inside one function body leaves the def map, the scopes and the other bodies' refs untouched — asserted by comparing the structures, not by timing |
| The cache is safe | a signature edit invalidates the file scope; a header edit invalidates every file that included it; a stale entry can never be returned |
| Adversarial input is bounded | one case per limit, plus deep nesting and a million-name file under the sanitizer preset |
| Deep input is a diagnostic, not a crash | the deepest accepted program resolves under ASan/UBSan on Windows' stack, like every other stage |
| Suggestions are bounded and sensible | a table of typos with expected suggestions, plus a case where the candidate cap is hit and the answer is still deterministic |

## References

- rustc name resolution — the two phases, namespaces with independent rib
  stacks, ribs pushed on blocks/`let`/expansion boundaries, and the reason every
  block is scanned for items first:
  <https://rustc-dev-guide.rust-lang.org/name-resolution.html>
- rustc compiler overview — where lowering sits, and the IR ladder
  AST → HIR → THIR → MIR:
  <https://rustc-dev-guide.rust-lang.org/overview.html>
- rustc AST validation — "a separate AST pass... doesn't perform any complex
  analysis, type checking or name resolution", run *before* crate resolution,
  implemented as `AstValidator` in `rustc_ast_passes`:
  <https://rustc-dev-guide.rust-lang.org/ast-validation.html>
- Rust RFC 1560 — name resolution as a defined phase with a defined output:
  <https://rust-lang.github.io/rfcs/1560-name-resolution.html>
- rust-analyzer architecture — `ItemTree` as a summary "stable over
  modifications to function bodies", `DefMap` for scopes, the invariant that
  typing in a body never invalidates global derived data, the syntax tree as a
  value type that must not store semantics, and the parent-then-child
  `source_to_def` pattern:
  <https://rust-analyzer.github.io/book/contributing/architecture.html>
- rust-analyzer, Find Usages — how a reference index is built from resolution:
  <https://rust-analyzer.github.io/blog/2019/11/13/find-usages.html>
- Three Architectures for a Responsive IDE — why the derived structures are
  split the way they are:
  <https://rust-analyzer.github.io/blog/2020/07/20/three-architectures-for-responsive-ide.html>
- Salsa — the incremental query engine rust-analyzer builds the def map on, if
  this project ever wants the reuse rather than the shape:
  <https://github.com/salsa-rs/salsa>
- The "mediating query" between a syntax tree and the analysis above it — the
  reason the item tree is a structure of its own:
  <https://internals.rust-lang.org/t/macros-vs-incremental-parsing/9323>
- Roslyn `Binder` — the binder as a distinct phase, scopes, and best-effort
  binding in the presence of errors:
  <https://github.com/dotnet/roslyn/blob/master/src/Compilers/CSharp/Portable/Binder/Binder.cs>
- Roslyn red/green trees — full fidelity, and why the tree is not where meaning
  is stored:
  <https://github.com/dotnet/roslyn/blob/main/docs/compilers/Design/Red-Green%20Trees.md>
- Roslyn's pipeline as `parse -> declaration table -> bind -> emit` and what
  that ordering buys:
  <https://victocore.top/posts/when-your-syntax-tree-mutation-doesn-t-survive-the-pipeline-tracking-roslyn-s-internal-phases>
- TypeScript's binder — a symbol table per node and flow analysis, the IDE-first
  version of the same phase:
  <https://github.com/microsoft/TypeScript/blob/main/src/compiler/binder.ts>
- Swift unqualified name lookup, extracted into a library of its own
  (`SwiftLexicalLookup`), with no type checker present:
  <https://forums.swift.org/t/gsoc-2024-swiftlexicallookup-a-new-lexical-name-lookup-library/75889>
- Swift scope and name lookup, described from the language's side:
  <https://dabrahams.github.io/SwiftRef/chapters/0300%20Scope.html>
- C scopes — block/file/function/function-prototype, nested scopes, and the
  point of declaration rules (`int x = x;`, labels, tags):
  <https://en.cppreference.com/w/c/language/scope>
- C name spaces — label, tag, member and ordinary identifiers:
  <https://en.cppreference.com/w/c/language/name_space>
- C17 6.2.1-6.2.2 — scopes and linkage, as the text this stage implements:
  <https://c0x.shape-of-code.com/6.2.1.html>
- The Go specification — file-scope names in the package block, and the
  order-independence that comes with it:
  <https://go.dev/ref/spec>
- Néron, Tolmach, Visser, Wachsmuth, *A Theory of Name Resolution* (ESOP 2015) —
  name binding as a relation over a scope graph:
  <https://eelcovisser.org/publications/2015/NeronTVW15.pdf>
- Scope graphs, the ongoing body of work this design follows in spirit:
  <https://pl.ewi.tudelft.nl/research/projects/scope-graphs/>
- Zig's `Sema`: ZIR → AIR, with name resolution, comptime evaluation and type
  checking in one pass over a lowered, untyped IR:
  <https://mitchellh.com/zig/sema>
- Zig's incremental compilation internals — the same "analyse the lowered form,
  keep the rest stable" split, from a compiler that had to retrofit it:
  <https://mlugg.co.uk/posts/incremental-compilation-internals/>
- How a Zig IDE could work — why a lowered, resolvable structure is what an
  editor actually needs:
  <https://matklad.github.io/2023/02/10/how-a-zig-ide-could-work.html>
