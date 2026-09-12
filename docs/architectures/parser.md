# Parser and syntax tree design

Decision record for the syntax layer: `src/parse` (the parser) and `src/syntax`
(the tree). It captures what production front-ends actually do, where they
disagree, what `minc+` takes from each, and every choice that is now fixed — so
the next person can tell a decision from an accident.

The `lexer.md` decision that this builds on: the raw lexer emits **trivia as
ordinary tokens**, so the token stream is lossless and the parser can filter.
This document is the other half of that bet.

## The one thing every reference agrees on

Roslyn, rust-analyzer, Swift, and Clang are written in four different
languages by four different organizations, and they agree on three points:

1. **The tree is lossless.** Concatenating the lexemes of every leaf reproduces
   the source byte for byte. Roslyn calls it *full fidelity*; Swift puts
   "source-accurate" in the first sentence of its README. A formatter, a
   refactoring, and a semantic highlighter all break the moment it is not true.
2. **The tree is semantic-less.** No name resolution, no types, no hygiene.
   rust-analyzer states it as a design goal: the tree describes "strictly the
   structure of a sequence of characters". This is not modesty; it is what lets
   a file parse before anything else does, and what lets a macro expand into a
   tree rather than a special case.
3. **Recovery is a feature, not an afterthought.** Roslyn: "error recovery
   ensures that partially-written code still produces a navigable tree."
   rust-analyzer: "parsing is resilient". A parser that stops at the first
   error cannot power an editor, because a file being typed is *always*
   momentarily invalid.

Everything else below is a disagreement, and the disagreements are the
interesting part.

## What the references do, and where they disagree

### Roslyn — the green tree is built by the parser

The parser builds immutable **green** nodes directly (`StartNode` / `FinishNode`
around `ParseXxx` methods). A green node stores *kind, width, children, and
diagnostics* and pointedly **no absolute position and no parent** — because an
immutable, shareable node cannot store either. The public **red** wrappers add
`Span` and `Parent` on demand. Because green nodes are position-free, "a method
declaration that was at position 10,000 can be reused at position 10,050 after an
earlier edit"; the green graph is a DAG, and a node cache makes the sharing real
(55% hit rate on the C# codebase). Incremental reparse happens in the *lexer*
blender, which interleaves old nodes and new lexed tokens.

- Take: position-free, parent-free, interned green nodes; the node cache; the
  red layer as a lazy view.
- Leave: the parser being welded to the node builder, and the incremental
  blender (it needs a text-diff input we will not have until the LSP).

### rust-analyzer — the parser emits *events*, a separate sink builds the tree

`parser` produces a flat `Vec<Event>` (`Start{kind, forward_parent}` / `Finish`
/ `Token{kind, n_raw_tokens}` / `Error{index}`); a `TreeBuilder` consumes the
events plus the token stream and builds a rowan green tree. The two live in
different crates and *neither depends on the other*. Trivia is in the tree but
not visible to the parser. **Errors are not in the tree** — they are a side
list. Abandoned nodes become `TOMBSTONE`. Left-recursive constructs are handled
by `CompletedMarker::precede`, implemented with `forward_parent` (see below).

- Take: the event split, `precede`/`forward_parent`, errors outside the tree,
  the trivia-blind parser, the typed `AstNode` view over an untyped tree.
- Leave: `Arc`/`Rc`/TLS — those are Rust-idiom answers to Rust's ownership, and
  our arena answers the same questions differently (see "memory model").

### SwiftSyntax — the tree is the macro system's substrate

The tree is explicitly the backbone of Swift's macros: a macro receives syntax
nodes and returns syntax nodes to splice in. Typed nodes are generated from a
grammar. This is the strongest evidence that a *lossless, homogeneous* tree is
what a future macro system wants, and it is why node kinds are reserved for an
unexpanded macro call in this design.

### Clang — recursive descent, and the ambiguity answer

Clang's parser is hand-written recursive descent, and it does **not** use a
lexer hack. The lexer reports `mytype` as an identifier both before and after it
is known to be a type; the parser asks `Sema` whether the identifier is a type
name, and Clang caches the answer by rewriting the token into an *annotation
token* so a backtracking reparse does not repeat the lookup. Class-member
declarations are handled by *delaying* inline bodies.

- Take: the policy of no lexer feedback; if a name ever becomes ambiguous, the
  answer is a bounded parser-side lookup plus a cached annotation, never a
  second token kind.
- Leave: `Sema`-in-the-parser. Our type names are already in dedicated positions
  (see "ambiguity policy"), so the C problem does not arise the same way.

## The design

Three layers, two modules, one direction of flow:

```
TokenStream (lossless, from lex)
      │
      ▼
┌─────────────┐   events + errors    ┌──────────────┐   green tree   ┌─────────┐
│   parse     │ ───────────────────▶ │   syntax:    │ ─────────────▶ │ cursor  │
│ (no tree,   │  Vec<Event>          │ TreeBuilder  │  (arena)       │ + AST   │
│  no diag)   │  Vec<ParseError>     │              │                │         │
└─────────────┘                      └──────────────┘                └─────────┘
        │                                    │
        │ errors → diagnostics               │ stored in Session, keyed (FileId, revision)
        ▼                                    ▼
   parse_report                          SyntaxTreeStore
```

The parser never builds a node, never writes a diagnostic, and never allocates
anything but its two vectors. That is the same bet `lexer.md` made: the stage
with the rules is testable without a `Session`, and the stage that knows about
spans and severity is separate.

### Why events instead of building the tree directly

It costs one extra vector and buys four things that are hard to retrofit:

1. **The grammar is testable without a tree.** A test can run the parser with a
   trivial sink that records `Start`/`Finish`/`Token` and assert the *shape* it
   intended, with no arena, no offsets, and no tree storage.
2. **The tree representation can change without touching the parser.** This is
   rust-analyzer's stated reason and it is the one that matters most for us: the
   memory model below is the part most likely to be revised.
3. **`precede` / `forward_parent` handles left recursion in a left-to-right
   parse.** A marker is opened, completed, and then *re-opened as the parent* of
   something already emitted:

   ```text
   a + b + c
   events:  Start(BinExpr) Token(a) ...        <- 'a + b' completes here
            Start(BinExpr) forward_parent=+2   <- the new node adopts the old one
            Token(+) Token(c) Finish
   ```

   The consumer walks `forward_parent` and enters the parent nodes before the
   child, so the second `+` ends up under a node that also contains `a + b`.
   No tree rewriting, no re-walking.
4. **Abandonment is free.** A node that a tentative parse opened and then
   rejected becomes `TOMBSTONE` and is dropped by the builder, so speculative
   parsing does not corrupt the tree.

### Why green/red on top

The green tree is what gets shared and cached; the cursor is what everything
else uses. The split exists for exactly one reason, stated by Roslyn and true
here: **an immutable node that can be shared cannot store a parent or an
absolute offset**, because the same node has different parents and offsets in
different places. So the shared node stores only `kind` and children, and the
cursor computes parent and offset on the way down.

This is also what makes the tree *incrementally reparseable later*: a node with
no position can be reused at a new position. We are not writing the incremental
reparse now, but the tree shape it needs is the one being built.

### Memory model — arena, not `Arc`

rust-analyzer's green nodes are `Arc`-counted so a tree can be freed piecemeal
and shared across threads. `minc+` has a bump `Arena` and a single-threaded
pipeline, so it uses the arena:

- Every `GreenNode` / `GreenToken` is arena-allocated and **never freed
  individually**; `Arena::rewind()` reclaims the whole revision at once.
- Sharing is still real, via a **node cache** (see below) that returns an
  existing pointer for an identical `(kind, children)`. The arena does not
  prevent the DAG; it just makes the DAG cheaper: shared nodes are shared
  pointers, not shared refcounts.
- Nothing walks the tree recursively without the depth guard (see "stack
  safety"), because an arena tree has the same recursion shape as any other.

`GreenNode` and `GreenToken` are trivially destructible, which is exactly the
`Arena` contract ("destructors do not run"). A `GreenToken` stores an interned
text id and a width, so it is a fixed-size value; a `GreenNode` is a header plus
a contiguous child array, allocated as one block like rowan's DST.

### The tree is untyped, the AST is a checked cast

The green tree has exactly one node type with a `SyntaxKind` tag. There is no
`BinaryExpr` class with `lhs`/`op`/`rhs` fields. The typed layer is thin wrappers
that `cast` from a node and return `std::optional` for every field:

```cpp
struct FnDecl {
  static std::optional<FnDecl> cast(SyntaxNode node);
  [[nodiscard]] SyntaxNode syntax() const;
  [[nodiscard]] std::optional<TypeNode> returnType() const;
  [[nodiscard]] std::optional<Name> name() const;
  [[nodiscard]] std::optional<ParamList> params() const;
  [[nodiscard]] std::optional<Block> body() const;   // missing while typing
};
```

Every field is optional **on purpose**. A half-written function has a name and
no body, and the AST must be able to say so without an exception or a null
dereference. This is why the untyped tree is the substrate: it can represent
invalid syntax; a typed tree with non-optional fields cannot.

Why homogeneous rather than a typed node hierarchy: error nodes, arbitrary
trivia, and future macro token trees all have to fit *somewhere*. A homogeneous
tree accommodates them without a new base class, and it makes generic traversals
(highlighting, "find all nodes in range") possible without a visitor per node
type. The typed API is then a convenience layer, not the storage.

## Layers in `minc+`

### 1. Token source — `parse/token_source.h`

The parser consumes `TokenKind`s, not tokens. The interface is deliberately
small, and it is the seam a macro expansion will plug into:

```cpp
class TokenSource {
public:
  virtual SyntaxKind current() const = 0;      // significant token only
  virtual SyntaxKind nth(int n) const = 0;     // bounded lookahead
  virtual void bump() = 0;                     // consume; never past EndOfFile
  virtual support::Span spanOfCurrent() const = 0;   // for error ranges
  virtual bool atEnd() const = 0;
  virtual ~TokenSource() = default;
};
```

The only implementation today walks `TokenStream::significantIndices()`. A
second implementation over a macro *token tree* is what the interface is for;
the parser must not need to know which one it has.

**The parser is trivia-blind.** Trivia is never returned by `current()`, and
`bump()` never consumes it. That is not an optimization; it is what keeps the
grammar free of whitespace rules.

### 2. Parser — `src/parse`

Produces `Vec<Event>` and `Vec<ParseError>` over the token source. Files:

- `parser.cc` — the core: markers, `expect`, recovery, the item loop.
- `expression.cc` — precedence climbing and the operator table.
- `statement.cc` — statements and blocks.
- `declaration.cc` — items (`fn`) and the `let`/`const` forms.
- `type.cc` — the type grammar.

Splitting is not cosmetic: the operator table, the synchronization sets, and the
node-kind list are each one thing in one place, which is what the lexer's
keyword table already does and what makes the "add one row" change safe.

Contract:

- **Pure and deterministic.** Given the same tokens it emits the same events.
- **No tree, no diagnostics, no I/O, no printing.** Errors are values in a
  vector; a separate target turns them into diagnostics.
- **Terminating and progress-making.** Every loop either consumes a token, or
  emits an error and consumes a token, or stops. There is no path that loops
  without progress, and that is asserted by test, not by reading.
- **Bounded in depth and in errors** (see "stack safety" and "limits").

### 3. Syntax — `src/syntax`

- `builder.cc` — the `TreeBuilder`: consumes events *and* the full token stream.
  It is the **only** component that sees trivia: before emitting an event's
  token, it flushes the trivia that preceded it, into the node currently open.
  This is the whole mechanism that makes "parser trivia-blind, tree lossless"
  work, and it is worth stating once, clearly, because it looks like a bug until
  it does not.
- `green.cc` — node/token allocation and the node cache.
- `tree.cc` — `SyntaxTree`: root, token stream, errors, `FileId`, revision.
- `node.cc` — the cursor (`SyntaxNode` / `SyntaxToken`): parent, offset, range,
  children, traversal.
- `ast*.cc` — typed accessors.
- `dump.cc` — pure formatting for `mincc parse`; writes nothing.

### 4. Report — `src/parse/parse_report.cc` (target `minc_parse_report`)

Turns `ParseError` into `Diagnostic`. `minc_parse` does not link `minc_diag`, so
"the parser does not report" is guaranteed by the build graph, exactly as for
the lexer.

### 5. Session — the store the request asked for

The lexer work put `Session` in `support/session`. The syntax layer adds:

- `SyntaxTreeStore` — owns each `SyntaxTree` keyed by `(FileId, revision)`,
  mirroring how `SourceManager` owns files. A bumped revision invalidates rather
  than repairs, because byte offsets do not survive an edit.
- **The node cache lives in the store, per revision, not per tree.** Roslyn
  keeps one per compilation so the `()` of one method is the `()` of another;
  that cross-file sharing is the point, and a per-session store is where it
  belongs.
- The **interner already in `Session`** is what green tokens point at, so a
  token's text is one `SymId` and common spellings (keywords, punctuation,
  indentation runs) are one entry regardless of occurrence count.

No new global state, no singletons: the parse of a file is a value in the store,
and dropping the revision drops it.

## Error recovery

This is the part that separates a toy parser from a professional one, so it is
specified rather than left to taste.

### Four mechanisms, used in order

1. **Missing token (insertion).** `expect(Semicolon)` where the token is absent
   records an error and inserts a **zero-width missing token**. The node's
   shape is then identical whether the user typed the `;` or not, which is what
   every typed accessor and every tree-walk relies on. Roslyn and rust-analyzer
   both do this; a tree whose shape depends on what is missing is unusable for
   tooling.
2. **Error node (wrapping).** An unexpected token is wrapped in an `Error` node
   rather than dropped, so losslessness holds and the offending text still has a
   place in the tree. The message goes in the side list; the node marks *where*.
3. **Synchronization.** At a recovery point the parser skips tokens until one in
   the construct's **follow set** — for statements, `Semicolon`, `RBrace`,
   `KwLet`, `KwConst`, `KwReturn`, `KwFn`; for items, the start of a new item or
   end of file. Skipped tokens go into one `Error` node, so a run of garbage is
   one error, not one per token.
4. **Bail-out.** After `kMaxParseErrors` the parser stops descending and
   consumes the rest of the input into a single `Error` node. Pathological input
   therefore costs bounded work instead of quadratic error cascades.

### Invariants a recovered tree still satisfies

Recovery is only trustworthy if the result is still a tree a tool can walk:

- Every byte of the source is under exactly one leaf (lossless), including the
  bytes inside error nodes.
- The tree contains no cycles and every child's range is inside its parent's.
- Delimiters are balanced **as nodes**: an unclosed `{` is closed at end of file
  (with an error), and a stray `}` is an `Error` node, so no consumer ever has
  to handle a half-open brace.
- Depth never exceeds `kMaxNestingDepth`, even on adversarial input.

`SyntaxTree::validate()` checks these and is called by the tests, the same way
`TokenStream::lossless()` is.

### Delimiter recovery detail

A consumed-but-unterminated `{` closes at end of file. An unterminated `(` in an
expression closes at the first token that cannot continue the expression. Both
cases produce one `expected '}'` / `expected ')'` error and one node that still
has a matching close, because the alternative — a node with no close — pushes
the invariant burden onto every downstream consumer.

## Expressions — precedence climbing with one table

One algorithm, `parseExpr(minPrecedence)`, that on each step asks a single
table what the current token binds as:

```cpp
struct OpInfo {
  SyntaxKind kind;
  std::uint8_t precedence;  // larger binds tighter
  Assoc assoc;              // Left | Right | None
  Fixity fixity;            // Prefix | Infix | Postfix
};

std::span<const OpInfo> operatorTable();
```

Why this and not N levels of recursive functions: with seventeen precedence
levels, one function per level is seventeen functions whose *call order* is the
grammar, and a misplaced call is a silent misparse. With a table, precedence is
data, and the table is tested directly. Precedence climbing and Pratt parsing
are the same algorithm (documented at length by Oil shell and by Norvell), so
this is not a simplification — it is the production choice, and it is the same
"one table, one place" shape as the lexer's keyword table and the parser's
synchronization sets.

The C table, low to high, with associativity — fixed as a decision, because it
is what makes `.mx` expressions unsurprising to anyone who knows C:

| Level | Operators | Associativity |
| --- | --- | --- |
| 1 | `= += -= *= /= %= &= \|= ^= <<= >>=` | right |
| 2 | `?:` | right |
| 3 | `\|\|` | left |
| 4 | `&&` | left |
| 5 | `\|` | left |
| 6 | `^` | left |
| 7 | `&` | left |
| 8 | `== !=` | left |
| 9 | `< <= > >=` | left |
| 10 | `<< >>` | left |
| 11 | `+ -` | left |
| 12 | `* / %` | left |
| 13 | prefix `- + ! ~ * &` | right |
| 14 | postfix `() [] . ->` | left |

Left-associative chains become left-nested nodes through `precede` and
`forward_parent`; the parser does not build a right-nested tree and then rotate
it.

## Ambiguity policy

C's ambiguity is famous and worth stating explicitly, because it is the reason
`lexer.md` refused to make primitive type names keywords and this document has
to live with that choice.

**What we get for free.** Types appear only in *type positions* in `.mx`:
after `fn` (return type), after `:` in `let`/`const`, and (later) in parameter
lists. A variable declaration is introduced by `let`, not by a bare type name,
so the C statement/declaration ambiguity — `i32 x;` could be a declaration or a
misuse of two expressions — **does not exist here**. That is a direct
consequence of the `let`/`const` decision and it removes the single largest
source of C parser complexity.

**The type position grammar is therefore a decision, not a guess.** After `:`
or after `fn`, the parser enters *type position* and consumes a sequence of
identifier/keyword tokens that name a type — which is what makes the multi-word
C spellings (`long long int`, `unsigned long long int`) parse at all. It stops
at the first token that cannot be part of a type (`=`, `;`, `)`, `,`). This is
the one place where "type names are identifiers" has a cost, and the cost is
that a bisected token stream cannot tell you whether a token is a type name
without position — which is why `SyntaxKind` has no `TypeName` kind, exactly as
the lexer has none.

**Hazards that remain, and their policy:**

| Hazard | Policy |
| --- | --- |
| C cast `(T)x` vs `(a) * b` | If casts are added, resolve with a **bounded tentative parse** (marker + rollback), not by asking a symbol table from the parser. No lexer feedback, ever. |
| Generic/angle `a < b > c` | Decide the syntax before implementing; if angle tokens are used, the disambiguation is explicit (an annotation token like Clang's), never a heuristic over arbitrary lookahead. |
| `*` in declarators vs multiplication | Our declarator syntax is not yet decided; the parser will only accept `*` as a pointer in type position, never as a guess. |
| Statement vs expression at the start of a line | Already removed by `let`/`const`/`return` being keywords. |

**Backtracking policy.** Tentative parsing exists but is *bounded*: a marker is
opened, at most a fixed number of tokens are examined, and if the guess fails
the marker is abandoned (tombstoned) and the tokens re-parsed. Unbounded PEG
backtracking (exponential on nested failures) is rejected. Where a single-token
lookahead decides the parse, that is used instead, because it is cheaper and
cannot fail.

## Stack safety — the cross-platform part that is easy to get wrong

A recursive-descent parser on adversarial nesting — `((((...))))`,
`{{{{...}}}}` — overflows the stack. That is not a hypothetical: it is a recurring
CVE class in parsers, and it is a hard crash, not a diagnostic.

The stack it overflows is platform- and build-dependent:

| Context | Typical stack |
| --- | --- |
| Linux / macOS main thread | 8 MiB |
| Windows main thread | 1 MiB |
| A sanitizer build | a larger frame per call, so *less* effective headroom |

So "it works on my machine" is exactly the wrong test. The design:

1. **A single depth guard.** A `DepthGuard` is taken in every recursive entry
   (expression, statement, declaration, type, block). It increments a depth
   counter, and refuses past `kMaxNestingDepth` (256, the value Clang uses for
   `-fbracket-depth`). Refusing means: record one error, consume the rest of the
   construct into an `Error` node, do not recurse. The counter is checked in one
   place, so it cannot be forgotten in a new parse function.
2. **The limit is sized for the worst case, not the best.** 256 must survive an
   ASan build on Windows' 1 MiB stack, not just a release build on Linux. There
   is a test that nests to the limit plus one and asserts a diagnostic instead
   of a crash; the sanitizer preset in CI is what exercises the worst-case frame
   size.
3. **Iterativity wherever it is free.** Binary operator chains are already
   iterative (precedence climbing); statement and item lists are loops, not
   recursion; blocks recurse because nesting is real. Recursion is used only
   where the syntax is genuinely nested, which keeps the constant small.
4. **A large-stack thread is not the defense.** Running the front end on a
   thread with an explicit stack size is a legitimate technique (some compilers
   do it), but it is platform-shaped and it turns "too deep" into a crash at a
   different threshold rather than into an error. The depth guard is the
   guarantee; a bigger stack would only be an optimization on top of it.

The same reasoning applies to the cursor and the dump: neither may recurse
without a guard, because a tree produced by the parser can be handed to them.

## Node identity, retention, and what is stored where

Because green nodes are shared and position-free, a pointer is **not** an
identity: the two `x + 2` expressions in a file may be the same green node. So
identity is `(FileId, byte range)`, which is also what an LSP request speaks.
rust-analyzer states this rule directly ("comparing nodes from different trees
does not make sense" / a node is stored as `(FileId, Range)`); an editor that
stored pointers would break the first time two subtrees were shared.

What is stored, and why:

| Stored | Where | Why not derived |
| --- | --- | --- |
| `SyntaxKind` on every node/token | green tree | it *is* the data |
| Children (contiguous) | green tree | it *is* the data |
| Interned token text id | green token | text is the only thing a leaf holds |
| Parent | cursor, computed | an immutable shared node cannot store it |
| Absolute offset/range | cursor, computed | same reason; would go stale on reuse |
| `ParseError` list | `SyntaxTree` | rust-analyzer keeps errors out of the tree |
| Node cache | `SyntaxTreeStore` | enables sharing; per-revision so an edit resets it |

Derived rather than stored, explicitly:

- **`SyntaxNode → Span`** is `offset + width`, where `width` is cached on the
  green node (that is the one derived value that *is* cached, because it is
  summed constantly).
- **Trivia extent, blank lines, leading whitespace** are read off the token
  stream, exactly as `lexer.md` decided for the lexer: storing them would create
  a second source of truth that a formatter must remember to fix.
- **Node → token mapping** is a walk; if profiling ever shows it hot, it becomes
  an index in the store, not a field on the node.

## Forward compatibility: macros, LSP, attributes

These are not implemented, and the point of this section is to say which
decisions keep them possible — and which would foreclose them.

- **Macros.** The parser never expands anything. An unknown `name!` is parsed as
  a *macro call* node whose child is a **token tree**, and expansion is a later
  layer that produces another token stream fed back through a `TokenSource`.
  This is why `TokenSource` is an interface and why the tree is homogeneous:
  a macro produces a tree, not a special node type. Node kinds for the macro
  call and the token tree are **reserved now** so that adding them is not a
  grammar redesign.
- **Attributes and doc comments.** Reserved node kinds; doc comments stay
  `LineComment` trivia in the lexer and are reclassified by a doc layer, so the
  parser and the tree need no change to support hover.
- **LSP incrementality.** The prerequisite is already the design: position-free
  green nodes (so a subtree can move), a node cache (so an unchanged subtree is
  the *same pointer*), and identity as `(FileId, range)`. The remaining work —
  a text-diff-driven reparse that reuses an unchanged prefix/suffix — is a
  later layer that consumes the tree, not a change to it.
- **Semantic tokens / highlighting.** They walk the homogeneous tree and the
  token stream and need no typed AST, which is why the untyped tree is the
  primary representation and the AST is a view.

## Decisions, now fixed

| # | Question | Decision |
| --- | --- | --- |
| 1 | Tree model | **Lossless, homogeneous, untyped green tree** + cursor + typed AST view. Not a typed node hierarchy. |
| 2 | How the parser builds the tree | **Events**, consumed by a separate `TreeBuilder`. The parser never allocates a node. |
| 3 | Trivia | **In the tree** (consistent with the lexer). The parser never sees it; the builder attaches it. |
| 4 | Parser errors | **Outside the tree**, in a side list; an `Error` node marks *where*, the list says *what*. |
| 5 | Missing tokens | **Inserted zero-width**, so a node's shape does not depend on what the user has typed yet. |
| 6 | Expression grammar | **Precedence climbing** over one operator table. C precedence and associativity. |
| 7 | Left recursion | `precede` via `forward_parent`; no tree rewriting, no re-walking. |
| 8 | Backtracking | Bounded and marker-based. No unbounded PEG backtracking. |
| 9 | Type names | **Never a token kind.** Types are recognized by *position*, never by asking a symbol table from the parser. |
| 10 | Recursion | Every recursive entry takes a `DepthGuard`; `kMaxNestingDepth` = 256. |
| 11 | Error cap | Past `kMaxParseErrors` the parser bails out into one `Error` node. |
| 12 | Memory | `Arena`, not `Arc`; sharing through a per-revision node cache. |
| 13 | Node identity | `(FileId, byte range)`, never a pointer. |
| 14 | Diagnostics | `minc_parse` has no diag dependency; `minc_parse_report` converts errors. |
| 15 | `SyntaxKind` | **One** u16 tag space covering tokens and nodes; the token range is pinned to `TokenKind` by a `static_assert`. |
| 16 | Grammar source of truth | One declarative grammar file drives the node-kind list and a consistency test; accessor code generation is a follow-up, not day one. |

## `SyntaxKind` — one tag space, pinned to the lexer

The tree needs a single tag for "what is this element", and it must cover both
leaves (tokens) and interior nodes, or every generic traversal grows a special
case for leaves.

```cpp
enum class SyntaxKind : std::uint16_t {
  // [0, kFirstNodeKind) are exactly lex::TokenKind's values, in its order.
  kFirstNodeKind = 256,
  File = kFirstNodeKind,
  Error,
  FnDecl, ParamList, Param, Block,
  LetStmt, ConstStmt, ReturnStmt, ExprStmt,
  Type, Name,
  Literal, Path, ParenExpr, PrefixExpr, BinaryExpr, ConditionalExpr, AssignExpr,
  // Reserved for the features above; not reachable until their syntax is decided:
  MacroCall, TokenTree, Attribute,
};
```

- `toSyntaxKind(TokenKind)` is the identity cast; a `static_assert` pins
  `kFirstNodeKind` above the last `TokenKind`, so adding a token kind cannot
  silently collide with a node kind.
- `toString(SyntaxKind)` and the kind list come from **one macro list**
  (`MINC_NODE_KINDS`), so the enum, the names, and any future generated accessor
  cannot drift — the same single-table shape as `flagInfos()`.
- The reserved kinds are listed and marked so "we planned for macros" is a fact
  in the code rather than a claim in a document.

## The `parse` command

`mincc parse <files...>`, wired into the same command table as `lex`:

```
$ mincc parse examples/002_variables.mx
== examples/002_variables.mx  (98 bytes, 46 tokens, 25 nodes, 0 errors, depth 4)

File@0..98
  FnDecl@0..98
    KwFn@22..24 "fn"
    Whitespace@24..25 " "
    Type@25..28
      Identifier@25..28 "i32"
    ...
```

- The tree dump is **pure formatting** (`dump.cc`), colored by the same
  `ColorMode` rules, and **never prints an address** — only kinds, offsets, and
  lexemes — so the output is byte-identical across runs and platforms and can be
  a golden file.
- `--no-trivia` hides trivia, which is what makes the dump readable for a
  grammar test and what the golden files use.
- A summary line: bytes, tokens, nodes, errors, max depth.
- Errors to **stderr** as diagnostics (via `minc_parse_report`) with the same
  `file:line:col` and caret as `lex`, and per-stream color, so a redirected
  stdout stays clean.
- `-` reads stdin, in binary mode on Windows, exactly as `lex` does.
- Errors exit `1`; usage errors exit `2`.

## How the claims above are checked

| Claim | Checked by |
| --- | --- |
| The tree is lossless | `SyntaxTree::validate()` reconstruction: concatenating leaf lexemes equals the source bytes |
| A recovered tree is still a tree | `validate()` on every golden file, including the malformed ones |
| The parser terminates and makes progress | exhaustive over all 1- and 2-byte inputs; deterministic byte soup; a hang is a test failure |
| Deep nesting is a diagnostic, not a crash | a test that nests past `kMaxNestingDepth`, run under the sanitizer preset |
| One table really is one table | every operator, every synchronization token, and every kind is exercised by a test that reads the table |
| Errors are the ones intended | golden files: `tests/parse/data/*.mx` + `*.tree` (`--no-trivia`) + `*.errors` |
| Grammar edits are caught | `examples/*.mx` must parse with zero errors, as for the lexer |
| Precedence is C's | table-driven tests per level, plus associativity tests (`a - b - c`, `a = b = c`) |
| The engine survives memory *and* UB bugs | `cmake --preset sanitize` (ASan + UBSan) in CI |

Golden files are the core of it: the input and the expected tree live side by
side, a mismatch prints a diff, and updating an expectation is an explicit act
rather than an accident.

## Non-goals for the parser and the tree

- No name resolution, no types, no const evaluation — that is sema.
- No macro expansion — that is a later layer over the token stream.
- No incremental reparse yet — the tree is *shaped* for it, but the diff-driven
  layer comes with the LSP.
- No source reading or encoding validation — that is `SourceManager`.
- No diagnostics and no printing — `minc_parse` and `minc_syntax` stay free of
  `minc_diag`, and the driver does the I/O.
- No C++-style complexity until the syntax is decided: the parser implements the
  syntax that is fixed, and reserves kinds for the rest rather than guessing.

## References

- Roslyn red/green trees — position-free immutable nodes, the DAG and node
  cache, full fidelity, lazy red nodes:
  <https://github.com/dotnet/roslyn/blob/main/docs/compilers/Design/Red-Green%20Trees.md>
- rust-analyzer syntax — events, lossless semantic-less trees, trivia models,
  the cursor and typed AST layers, node identity:
  <https://github.com/rust-lang/rust-analyzer/blob/master/docs/book/src/contributing/syntax.md>
- rust-analyzer `event.rs` — `Start`/`Finish`/`Token`/`Error`, tombstones, and
  `forward_parent` for left recursion:
  <https://github.com/rust-lang/rust-analyzer/blob/master/crates/parser/src/event.rs>
- SwiftSyntax — the tree as a macro system's substrate, generated typed nodes:
  <https://github.com/swiftlang/swift-syntax>
- Clang and the type/variable ambiguity — no lexer hack, positional type lookups,
  annotation tokens, delayed inline bodies:
  <https://eli.thegreenplace.net/2012/07/05/how-clang-handles-the-type-variable-name-ambiguity-of-cc>
- Pratt parsing and precedence climbing are the same algorithm:
  <https://www.oilshell.org/blog/2016/11/01.html>,
  <https://www.engr.mun.ca/~theo/Misc/pratt_parsing.htm>
- Resilient recursive descent — resilience and full fidelity as the two
  requirements that shape the parser:
  <https://thunderseethe.dev/posts/parser-base/>
- Unbounded parser recursion as a stack-overflow/DoS class (why the depth guard
  is a hard requirement, not a nicety):
  <https://github.com/webonyx/graphql-php/security/advisories/GHSA-r7cg-qjjm-xhqq>
