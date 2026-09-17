# Type aliases — `type Name = T;`

`type Bytes = [8]u8;` introduces **a name for a type that already exists**. It is
not a new type: `Bytes` and `[8]u8` are the same type, in the strongest sense this
compiler has — they are the same `TypeId`, and every question the checker asks
about a type (`sema.md`) answers identically. What is written down here is how
that name is read, where it is visible, what it does to every stage after the
checker, and — the part that decides whether this feature is still standing when
`struct` lands — **which machinery it must not touch**.

This is the first of the *top-level types* in the feature checklist, and it is
deliberately the smallest one: `struct`, `enum` and `union` need identity by
name, which `TypeStore` does not have yet (`modules.md`, seam S4), while an alias
needs nothing from the store at all. It is also the one that is *most* likely to
be reached for while writing the others, so its rules have to be right first.

## What an alias is, and what it is not

Every language in the market has two things that get called "types" in a user's
head, and separates them because they behave differently:

| | C | C++ | Go | Rust | Swift |
|---|---|---|---|---|---|
| name for an existing type | `typedef int T;` (6.7.8) | `using T = int;` | `type T = int` | `type T = i32;` | `typealias T = Int` |
| a *new* type, same shape | — (`struct`/`union`/`enum`) | `struct`/`enum class` | `type T int` | `struct`/`enum` | `struct`/`enum` |
| distinct at the type level? | no | no | **yes** | no | no |

The three facts this project takes from that table, each with its source:

1. **A name for a type does not create a type.** cppreference, on `typedef`: *"typedef
   declaration does not introduce a distinct type, it only establishes a synonym
   for an existing type, thus typedef names are compatible with the types they
   alias."* Go's specification says the same thing with a marker instead of a
   footnote, and it is the marker that is worth copying: `type A = T` is an alias
   and `type T T0` "declares a new type that is never identical to the type on the
   right-hand side" (Go blog, *What's in an (Alias) Name?*). One character apart,
   two different concepts — which is the cautionary tale, not the design.
2. **The `=` is the marker of transparency, and it is one-way.** We adopt
   `type Name = T` for exactly the first row. A nominal declaration, when it
   exists, will be a *different* surface, because inferring "this one is a real
   type" from the absence of one character is how Go's two forms came to be
   confused with each other in the first place.
3. **Being transparent is the whole value.** Rust's reference: *"A type alias
   defines a new name for an existing type in the type namespace of the module or
   block where it is located."* In the type namespace, and for a *name* — not an
   entry in the type table.

## The surface

```
type Bytes = [8]u8;             // a whole type
type Fn    = *str;              // any type the reader already accepts
type Count = size_t;            // ... including the C spellings
type Bytes2 = Bytes;            // and other aliases
```

One production, one new node, and **no type grammar of its own**:

```
TypeAliasDecl := 'type' Name '=' Type ';'
```

The right-hand side is the same `Type` node a signature, an annotation and a cast
already carry (`parser.md`: a type is a *position*, not a token kind). That is
what makes `type A = *[4]i32;` cost nothing: the RHS reader is the one reader,
and a type form added to the language later is accepted here the day it lands.

Where it may be written:

- **File scope**, among the items, beside `fn`/`let`/`const` (`isItemStart`,
  `parseItem` — one more arm in the switch that already dispatches by leader
  token).
- **Block scope**, among the statements. C allows it (`cppreference`: a typedef
  for a VLA "can only appear at block scope"), Rust allows it, and the scope
  machinery is already there; the alternative is a rule that says "a name for a
  type is fine here and not there" for no gain.

What is *not* on the surface, deliberately: no `type A;` (a declaration without a
name for anything is not a declaration), no `type A = distinct i32;` (nominal
identity is a different feature with different machinery — § *Seams*), no
parameters (`type Vec = []T` has no `T` to bind yet).

## Identity: an alias never enters the `TypeStore`

`TypeStore` interns by structure, and that is the answer to "are these the same
type?" (`type.h`). An alias must therefore **not** be representable in it: there
is no `TypeKind::Alias`, no `Type::aliasOf`, and `Type::name` — the field
reserved for a named type — stays untouched. Three consequences, and they are the
reason this is a decision and not an implementation detail:

- **Everything works for free, in the direction that matters.** `f64` and
  `type Meters = f64` are one id, so assignment, argument passing, `return`, the
  declared-type check, `as`, `sizeof`/`typeof`/`alignof`, array elements,
  pointees, slices and `? :` all accept either spelling *without a line of code
  anywhere*. A feature whose composition is free is a feature whose composition
  cannot be individually broken.
- **It cannot be half-applied.** If the alias were in the store, every consumer
  would need to know to strip it — that is, every consumer would be a place to
  forget. There is one identity, so there is one place to be right.
- **The store's growth stays a function of the program's types.** Declaring a
  thousand aliases adds zero types. That is a stated invariant with a test: a
  unit whose only declarations are aliases leaves `TypeStore::count()` at
  `kFirstInternedType`.

The one thing an alias *does* need beyond a `TypeId` is the **name**, because a
diagnostic and a debugger should show what was written. That is recovered from
the *node* the name was written on, which is where it already is —
`LoweredFile::spellingOf` has carried the source text of every node since the AST
existed. The plan is therefore not "remember the alias somewhere new" but
"publish which alias was written at which type position" (§ *Diagnostics*, § *The
record*).

## Namespaces: a type name lives among the names of types

`resolve` already has C's four namespaces, one of them unused: `Ordinary`,
`Tag`, `Label`, `Member` (`def.h`). C puts a `typedef` name in the **ordinary**
namespace, which is why `typedef int T; int T;` is an error and why every C
library writes the same idiom twice:

```c
typedef struct tnode tnode;   /* one name in the tag space, one in the ordinary */
struct tnode { tnode *left, *right; };
```

We take the other road, and it is the road the *grammar* has already taken: **a
name in a type position can only be a type name, and the parser knows which
position it is reading** (a `Type` node is a run of identifiers; an expression's
name is a `PathExpr`). There is no ambiguity to resolve, so there is no reason to
make a type name and a value name compete:

- **A type name lives in `Tag`**, which this record defines as *the namespace of
  type names* — aliases today, `struct`/`enum`/`union` names when they land,
  sharing it (so `type P = i32;` and `struct P {}` collide, which is a typo two
  errors later if it is allowed to stand).
- **A value name lives in `Ordinary`** (`fn`, `let`, `const`, parameters), as
  today.
- The C-shaped door (`#include`, `cinterop`) is a *different* door: a C header's
  `typedef` names enter `Ordinary` and its tags enter `Tag`, because there the
  point is to reproduce C's rules for C's code. That door does not exist yet;
  what matters here is that this record does not close it (`extern.md`).
- The consequence for users: `type Id = u64;` and `let Id = 1;` coexist. Rust
  works exactly this way for the same reason (types in one namespace, values in
  another, which is why `struct Foo` and `fn Foo()` coexist there too).

**A built-in name may not be redefined.** `type i32 = i64;` is refused: `i32` is
a *word the reader knows* (`typespec.cc`'s table), and a unit-level binding that
silently changed its meaning would make every later `i32` in the file mean
something else — a language-wide rename dressed as a one-line declaration. The
message names the conflict. (`long`, `unsigned` and the rest of the C words are
in the same table and are refused the same way.)

**Redeclaration** in the same scope is refused, with the first declaration named
in a note, exactly as the other declarations of this language are.

**Shadowing** across scopes follows `let`/`const`: allowed, warned by
`-Wshadow`. A block-scope alias that hides a file-scope one is the same rule
applied to a different namespace, and having two shadowing rules would be two
things to remember for no gain.

C11 allows an identical typedef to be *repeated* (6.7.8: a name "may be
redefined to denote the same type as it currently does"), and clang measures it
for us:

```console
$ clang -fsyntax-only -std=c11 tdup.c          # typedef int T;  typedef int T;
$ clang -fsyntax-only -std=c11 tdup2.c         # typedef int T;  typedef long T;
tdup2.c:2:14: error: typedef redefinition with different types ('long' vs 'int')
tdup2.c:1:13: note: previous definition is here
```

The first command printed nothing at all: the repetition of the same type is
accepted without a warning, which is the rule being declined.

We decline the allowance in `.mx`: one name, one declaration. The rule exists for
C so that two headers may both declare the same typedef, and that problem belongs
to the C door — where C's rule will be applied *because* the file is a C header.
Inside `.mx` the allowance would only hide a copy-paste, and `import` (when it
lands) is a name-based system that never needs the same name twice.

## Scope, order, and the file as a whole

- **File scope is order-independent.** `type A = *B;` before `type B = i32;` is
  legal, and there is no forward-declaration ritual. The precedent is already in
  the language: file-scope `let`/`const` are checked in dependency order with an
  explicit stack and a three-colour mark, so a forward reference folds
  (`globals.md`, and `Checker::collectGlobals`/`checkGlobals` in `src/sema/global.cc`).
  A type name is a *name*, not storage, so the same treatment costs nothing and
  buys the module-shaped code (an interface naming a type declared further down)
  that C needs two declarations to express.
- **Block scope is in source order**, like `let`, because a block is read top to
  bottom and a name introduced halfway down is not visible above it.
- **The whole file is declared before any body is checked**, as it is today: the
  alias pass is part of the same file-scope collection that already gathers
  functions and globals, so a function parameter may be named with an alias
  declared after the function.

## Cycles: every one is an error, and the reason is the transparency itself

`type A = B; type B = A;` has no meaning, and the compiler must say why rather
than run out of stack. The rule is not "aliases may not be recursive" — that
would be an over-claim that the arrival of `struct` would falsify. The rule is:

> **An alias is an abbreviation. An abbreviation that contains itself has no
> expansion.** Recursion needs a type that *names itself* rather than standing for
> something else, and until `struct`/`enum` exist there is no such type in this
> language.

That distinction is the whole of the future-proofing, and it has a visible
consequence today: `type P = *P;` is refused **too**, even though the pointer
would break the cycle for a nominal type (`struct Node { next: *Node; }` is fine
in every language in the market, and will be fine here). Under structural
identity there is no "P" for the pointer to point at until P exists, and P exists
only when its target does. Refusing it with a sentence that says *that* is what
keeps the refusal from looking like a limitation of the language rather than a
property of names.

The mechanism is the one `checkGlobals` already uses, and it is deliberately the
same shape rather than a second invention:

- an **explicit stack**, because a chain of aliases is as long as the unit has
  declarations and a unit is somebody else's file;
- **three marks** (unresolved / in progress / decided), so a name that is reached
  while it is still in progress *is* the cycle;
- **the path is remembered**, and the message names it: *"the type `A` is defined
  in terms of itself: `A` → `B` → `A`"*, with the note pointing at the `=` of the
  declaration that closed it. `globals.md` already reports value cycles this way;
  the two passes should read alike.
- **no arbitrary depth constant.** The bound is the unit's own number of type
  declarations (and, behind it, `kMaxTypesPerUnit`), which is a bound the program
  wrote rather than a number this compiler picked.

## Diagnostics: the written name survives, and says what it stands for

Expanding an alias is right for *identity* and wrong for *reading*. The market's
best behaviour here is clang's, measured:

```console
$ clang -fsyntax-only talias.c
talias.c:7:8: error: incompatible pointer to integer conversion initializing 'Deep'
      (aka 'int') with an expression of type 'char[4]'
```

Two things at once: the name **the user wrote** (`Deep`, the outermost alias at
that position), and **what it stands for** (`aka 'int'`). We adopt that idiom —
the written name first, the expansion in parentheses, once per message, only when
they differ — and the doc's rule is that a diagnostic about a type prints what
was written and expands it rather than silently replacing the reader's word with
a number of bits.

The facts the formatter needs are already in the compiler: the type position's
node carries its own spelling, and the alias table says whether that spelling is
an alias. Three rules fall out:

1. **A mismatch names both.** `expected 'j64', found 'i32'` where `j64` is an
   alias prints `expected 'j64' (aka 'i64')`.
2. **A type name where a value is expected is a sentence, not a lookup failure.**
   `let x = Bytes;` says *"`Bytes` names a type, so it has no value"*, with a note
   at its declaration — **landed**, and the two spellings of the mistake get one
   answer: a word of the language (`x = (i32);`, which was already the better
   sentence) and a name this unit declared. Neither is a name that could have been
   declared and was not, so "unknown name" would send the reader looking for a typo
   that is not there. The lookup stays the ordinary one; the *sentence* that reports
   its failure is the one thing that asks the type namespace, and the reference is
   recorded as `UnresolvedReason::WrongNamespace` — the reason reserved for exactly
   this — rather than as a use of the declaration, so nothing counts a value read
   that never happened.
3. **A duplicate names the first one.** `note: previous declaration of 'Bytes' is
   here`, the same shape as clang's typedef-redefinition note above.

## Debug information: a `DW_TAG_typedef` per alias

Without this section the feature would be *invisible to the debugger*, which is
the difference between a language you can compile and a language you can debug.
clang's output for `typedef int MyInt;` (measured with `llvm-dwarfdump` on a
`-g -O0` object, and read back with gdb):

```text
0x0000002e:   DW_TAG_typedef
                DW_AT_type      (0x00000036 "int")
                DW_AT_name      ("MyInt")
                DW_AT_decl_file ("/tmp/talias2.c")
                DW_AT_decl_line (1)
```

So the bar is: **one `DW_TAG_typedef` DIE per alias**, with the target type, the
name, and the declaration's file and line. That is `DIDerivedType` in LLVM's
terms, and it belongs in `src/ir/debug.cc` beside the type DIEs that already
exist — the stage that owns debug info, not the checker.

Where clang *stops* is worth measuring too, because it is the fork. For a function
`void f(MyInt x, Ptr p)` with `typedef int MyInt; typedef int *Ptr;`, the formal
parameters' own DIEs name the **base** type, and gdb answers accordingly:

```text
(gdb) whatis x      $ MyInt          ← the typedef, found in the CU
(gdb) ptype  x      $ int            ← the resolved type
(gdb) ptype  MyInt  $ int
```

So the typedef DIE alone is what makes `whatis` print the programmer's word, and
the *declaration's* DIE is what `ptype` reads. Our step past that bar is to point
a declaration's type DIE at the typedef DIE when the declaration wrote the alias,
so both commands answer with the name the source used. That needs one more
published fact — which alias was written at a given `Type` node — which is also
exactly what hover and go-to-definition need later (§ *The record*). **Built, and
measured with the same debugger clang was measured with:**

```text
$ mincc build -g -o tal.bin tal.mx && gdb -batch -ex 'break main' -ex run \
      -ex 'whatis d' -ex 'whatis r' tal.bin
type = Meters      ← `let d: Meters`, an alias of f64
type = Rec         ← `let r: Rec`, an alias of [4]i32
$ llvm-dwarfdump --debug-info tal.bin | grep -A3 DW_TAG_typedef
DW_TAG_typedef  DW_AT_type ("i32[4]")  DW_AT_name ("Rec")  DW_AT_decl_line (2)
DW_TAG_typedef  DW_AT_type ("f64")     DW_AT_name ("Meters") DW_AT_decl_line (1)
```

The step past clang is the second line of that session: clang's parameter DIEs name
the base type and only the typedef DIE carries the word, while here the *binding*
points at the typedef — so a declaration that wrote a name is described with that
name. One DIE per *declaration*, cached by the declaration's index, so ten
positions are one node (a test counts them); the DIE is rooted in the compile unit
and not in the block that happened to declare a variable first, because a name for
a type is visible over a region of text and not over a region of instructions; and
the scope choice is why a block's name still works — the block's declaration is a
second index and therefore a second DIE, not a collision.

## The record: what `sema` publishes, and who reads it

One table, in the shape `GlobalInfo` already has:

```cpp
struct TypeAliasInfo {
  ast::AstId decl;                 // the `TypeAliasDecl`
  ast::AstId nameNode;             // the `Name`, for the note in a diagnostic
  ast::AstId target;               // the `Type` node, for go-to-definition on the RHS
  TypeId type = kInvalidType;      // the expansion, after the pass has run
};
```

published as `TypedFile::aliases()` (source order, deterministic) plus
`TypedFile::aliasAt(typeNode)` — the per-position answer, in the spirit of the
existing `coercionAt(consumer, operand)`: the stage that decided the fact records
it where the consumer will look for it, so no consumer has to re-derive it.

Readers, and what each one does with it:

| reader | uses it for |
|---|---|
| `sema::dumpTypedFile` / `dumpTypeStore` | the alias section of `mincc check`'s dump |
| `resolve --dump` | the definition's kind (`type`) and its namespace |
| `src/ir/debug.cc` | the `DW_TAG_typedef` DIEs, and the per-position pointer |
| `-Wshadow` / notes | the first declaration, and the name to print |
| the future LSP | hover, go-to-definition, rename — the *reason* the alias is a real `Def` and not a store entry |
| the future module interface | which aliases a unit exports (a name, so exporting one is ABI-free — unlike a nominal type) |

## The walk through the pipeline

In order, with the test each step has to come with. Nothing here is speculative:
every seam named is an existing function.

1. **`lex`** — `type` becomes a keyword (`KwType`, one row in the keyword table in
   `src/lex/token_kind.cc`). It stops being an identifier; that is a breaking
   change for a program that used it as a name, and the right one to take now,
   because a contextual keyword is a rule every later reader of a type position
   would have to remember. Tests: `type` is a keyword, `typex` is an identifier.
2. **`parse`** — `SyntaxKind::TypeAliasDecl`, `isItemStart` learns the word,
   `parseItem` gets its arm, a block-statement arm beside `let`/`const`, and
   `parseTypeAlias` reads `'type' Name '=' Type ';'` with every token kept (the
   tree stays lossless and a formatter still round-trips). Recovery: a missing
   name, a missing type, a missing `=`, a missing `;`, and `type X == i32;` each
   get their own sentence. Tests: shape, every type form on the RHS, block scope,
   each recovery, and that the leaves concatenate back to the source.
3. **`ast`** — lowering (`src/ast/lower.cc`) and the structural rules
   (`src/ast/validate.cc`): exactly one `Name`, exactly one `Type`, the `=` and
   the `;` present, nothing else. Tests: a valid declaration lowers; each
   violation is refused with its code.
4. **`resolve`** — `DefKind::TypeAlias`, the `Tag` namespace, the file-scope
   collection (order-independent) and the block walk (in order), duplicate
   detection with the first declaration's note, and `-Wshadow` through the same
   path as `let`. Tests: the def's kind/namespace/origin; duplicate refused with
   the note; shadowing allowed and warned; the built-in refusal
   (`type i32 = i64;`); a forward reference at file scope is one definition, not
   two.
5. **`sema`** — the alias pass (dependency order, marks, cycle path), the reader
   gaining a name lookup, the published table, the dumps, and the diagnostics of
   § *Diagnostics*. The reader's new parameter is a **span of
   `{spelling, TypeId, decl}`**, defaulted to empty so no existing call site
   changes and `typespec` stays a table plus a function — the aliases of a unit
   are *data* the caller passes, not a hook the reader calls back into.
   Tests: identity in every type position (one table-driven test that would fail
   if a consumer ever special-cased an alias); alias of alias; cycles (self,
   mutual, through a pointer, through an array, through a slice) with the
   message and the path; `type A = [_]i32;` refused (the count comes from an
   initializer and a type name has none); the store does not grow; the dump text;
   `(aka ...)` in a real diagnostic; a type name in a value position.
6. **`ir`** — nothing at all without `-g`, and that is a test: the module for a
   program with aliases must be **byte-identical** to the module for the same
   program with the aliases spelled out. With `-g`, the typedef DIEs and the
   per-position pointer, checked with `llvm-dwarfdump` in `tests/unit/ir/debug_test.cc`.
7. **`driver` / CLI** — `check`'s summary line and dumps, `ir`, `build`, `run`:
   one end-to-end test per command, plus an `examples/019_type_aliases.mx` that
   `make examples` checks, dumps and runs like every other example.
8. **docs** — the site's `language/types.md` gains the alias section (identity,
   order, cycles, `aka`), the feature checklist marks it, and the roadmap item
   points here. Documentation is part of the feature, not a follow-up.

### As built

The eight steps landed, with five places where the implementation is more specific
than the plan above. Each is a fact a later change has to keep, so they are written
down rather than left in the diff:

- **The block scope is the *same production*, and that decided two things.** The
  parser needed no block arm beyond the statement dispatch recognizing `KwType` as
a statement leader — which it must be anyway, or a stray `type` would end recovery
  and cascade. The plan's paragraph about "a block-statement arm beside
  `let`/`const`" is therefore one line in `isStatementStart` and one call, because a
  second production is a second place for a type form to be forgotten.
- **The published name table is a stack, and the reader searches it backwards.** A
  block may declare a name the file already has, so "first match wins" would answer
  the *file's* name inside the block. The pass pushes a row per decided name, a block
  drops the rows it pushed when it ends, and `findTypeName` reads from the end —
  which is the whole of the shadowing rule, and the reason `checkBlock` remembers one
  size. A row carries the index of the declaration that published it, so a type
  position records *which* declaration answered it even when two spellings are one.
- **`readType` has to carry the "broken name" flag through.** `readType` wraps
  `readTypeSpec` (it applies the `*`/`[N]` constructors), and it used to fold `ok`
  with no type into the budget answer — which turned every use of a failed alias into
  "the unit has too many distinct types" without lying about nothing else. The flag
  travels with the result now, and both call sites that report on a type position
  (`resolveTypeNode` and the array-literal reader) stay silent for it.
- **What is refused for a *reserved* name is the resolver's sentence alone.** The
  pass asks the same table (`support::isTypeNameWord`) and only makes sure the name
  cannot become usable; a second message would be two diagnostics for one mistake.
  The name is then deliberately **not** published, so `i32` goes on meaning `i32` for
  the rest of the unit — a refused declaration must not redefine the language for the
  lines after it.
- **A block `type` is a no-op statement in `ir`** (`stmt.cc`), beside the empty
  statement: a name for a type is not storage and not an instruction. The `-g` half
  is built from the positions that *wrote* the name, so a declaration no position
  used produces no metadata at all, and one that was used produces it where needed.

One deliberate gap, small and named: a use *above* a block's declaration says "not a
 type" and its "did you mean" note is suppressed when the near miss is the very same
spelling. The better sentence — "declared below, and a block is read in order" —
needs the position of the declaration this use precedes, which the checker does not
have; it is the next improvement here and not a hole in a rule.

## Seams: what this must not close

Four things have not been built yet, and each of them is a reason a decision
above is written the way it is.

- **Nominal types (`struct`/`enum`), and `modules.md` S4.** Aggregate identity is
  a name plus the declaring unit, which means `TypeStore` grows a nominal path.
  An alias must not be in that path, and the alias table must not be *inside* the
  store: the day nominal types land, the store gains a kind and the alias code
  does not move. The `Tag` namespace is where a struct's name will live, and it
  is where an alias's name lives today — one namespace, so `type P = ...; struct
  P { ... }` is a collision and not two types with one spelling.
- **Modules.** An alias is part of a unit's interface. Because it is transparent,
  exporting one is *ABI-free* (a name for `i32` is `i32` in the importer), which
  is the opposite of a nominal type and worth stating so nobody "fixes" it into
  one. What the module system must carry is the alias's *declaring unit*, for the
  same reason Rust does: a diagnostic should say which unit's alias it is.
- **Generics.** `type Vec<T> = []T;` needs a substitution step, and Go is the
  evidence that it is separable and late: aliases arrived in Go 1.9, generics in
  1.18, generic aliases in 1.24. Because identity here is structural and an alias
  carries none, instantiation adds no identity problem — only spelling, which is
  the LSP's and the debugger's problem again. The only thing to do now is leave
  the *parser* able to grow a parameter list between the name and the `=` (the
  production above is written with that slot in mind), and to keep the reader
  taking a *table of names* rather than a single name, so a future instantiation
  is one more table entry and not a new reader.
- **A C header's `typedef`** (`cinterop`, `extern.md`). Different door, different
  rules: C's names are in the ordinary namespace and C's identical-redefinition
  allowance applies there. Nothing in this record prevents that; the one thing it
  must not do is make the `.mx` rule do double duty.

Two smaller notes, for completeness. `type A = [_]i32;` is refused, because an
inferred count comes from an initializer and a *name* has none — the sentence
says so and points at the count. And aliases compose with the preprocessor for
free: `#define T i32` followed by `type X = T;` works today, because macros are
expanded before the parser ever sees the declaration, which is worth a test
rather than a paragraph.

## Decisions

| # | Decision | Why |
|---|---|---|
| 1 | **`type Name = T;` is a transparent alias and the `=` is what says so** | C, C++, Rust and Swift all separate "name for a type" from "new type"; Go marks the difference with one character and its blog calls the alias form *"a new name for an existing type without introducing a new type that has a different identity"*. A nominal declaration later gets a word, not a missing character |
| 2 | **An alias never enters `TypeStore`** | The store's answer to "same type?" is structural identity, and an alias has no identity of its own. Putting it in the store would make every consumer a place to remember to strip it; leaving it out means assignment, calls, casts, `sizeof` and the rest accept either spelling with no code |
| 3 | **The name lives in the `Tag` namespace, which this record defines as the namespace of type names** | The grammar already decides by *position* whether a name is a type or a value, so the two can never be confused; C's ordinary-namespace rule is why C needs `typedef struct T T;`. A struct's name will live in the same namespace, so two types cannot share a spelling |
| 4 | **A built-in type word cannot be redefined** | `type i32 = i64;` would silently change the meaning of every later `i32` in the unit — a rename of the language wearing the clothes of a declaration |
| 5 | **File scope is order-independent; block scope is in order** | The value bindings already work this way (`globals.md`: a forward reference folds), so this is one rule and not a new one; and `type A = *B; type B = i32;` is what module-shaped code needs |
| 6 | **Every cycle is refused, including through a pointer, and the message says why** | An alias is an abbreviation and an abbreviation containing itself has no expansion; recursion needs a nominal type, which does not exist yet. `type P = *P;` is refused *because there is nothing yet for P to name*, not because aliases may never recur — the difference is what keeps the rule true when `struct` lands |
| 7 | **Redeclaration in one scope is refused, rather than C's identical-redefinition allowance** | One name, one declaration; the allowance exists so two C headers can repeat a typedef, and that problem belongs to the C door. `import` is name-based and never needs it |
| 8 | **A diagnostic prints the written name and expands it: `'Deep' (aka 'int')`** | Measured from clang, and the reason to copy it: expanding is right for identity and wrong for reading, and the reader wrote the name. The formatter needs facts that are already recorded (the node's spelling, the alias table) |
| 9 | **One `DW_TAG_typedef` per alias, with name, target, file and line; and the declaration that wrote an alias points at it** | Measured from clang's own output — and measured further: clang points a parameter's or local's DIE at the *base* type, so gdb's `whatis x` prints `MyInt` and `ptype x` prints `int`. The typedef DIE is the bar; naming it from the declaration is the step past it, and the test is a gdb session |
| 10 | **The reader takes a *table of type names*, defaulted empty** | `typespec` stays a table plus a function with no knowledge of `resolve`; the aliases of a unit are data. A defaulted parameter means no existing call site changes, and a future generic instantiation is one more entry |
| 11 | **The alias is a first-class `Def` with a published record** | Redefinition rules, shadowing warnings, dumps, debug info, the module interface and the future LSP all need one answer to "what is this name?" — the same reason `let` and `fn` are defs and not side tables |

## What is deliberately not done here

- **No nominal alias** (`distinct`/newtype). It needs identity by name and unit
  (S4), and shipping it as "an alias that happens to be distinct" would be the
  confusing half-measure Go warns about.
- **No alias with parameters**, and no associated types. The parser leaves room;
  nothing else does.
- **No `pub`**. Visibility is the module system's, which is not designed yet;
  when it lands, an alias is one of the things it exports, and exporting it is
  free.
- **No C `typedef` reading.** That is `cinterop`'s door, with C's rules.
- **No `type` on a `_` count, no variably modified aliases.** This language has
  no VLAs by design (`arrays.md`: the count is a folded value read once, and a
  product that does not fit is refused), so the C99 VLA-typedef corner does not
  exist here and does not need a rule.

## References

Read while writing this, and quoted where quoted:

- cppreference, *Typedef declaration* (C): "does not introduce a distinct type";
  typedef names share the ordinary name space; the incomplete-type and
  VLA-at-block-scope notes; the `typedef struct tnode tnode` idiom; C11 6.7.8.
- The Go specification, *Type declarations*, and the Go blog *What's in an (Alias)
  Name?* (Griesemer, 2024): alias declarations, the `=` marker, `type T T0` as a
  new type, and the 1.9 → 1.18 → 1.24 timeline for aliases, generics and generic
  aliases.
- The Rust Reference, *Type aliases*: a new name in the type namespace of the
  module or block, and what an alias may not be (a constructor, a bounds
  carrier).
- Measured locally, not quoted from anywhere: clang's `'Deep' (aka 'int')`
  diagnostic; clang's `DW_TAG_typedef` DIE with `DW_AT_decl_file`/`decl_line`;
  clang's formal-parameter DIEs naming the base type while gdb's `whatis` names
  the typedef and `ptype` does not; clang's identical-vs-different typedef
  redefinition pair.
- This project: `sema.md` (the type model and the identity rules), `type.h`
  (interning), `globals.md` (order-independence and cycle reporting),
  `resolve.md` (namespaces, duplicates, shadowing), `modules.md` (S2, S4 and what
  the module system must carry), `parser.md` (types are positions, not kinds),
  `arrays.md` (`[_]` and why there are no VLAs), `ir.md` / `codegen.md` (debug
  info), `extern.md` / `cinterop` (the C door).
