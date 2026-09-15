# File-scope variables

A **file-scope binding** is a `let` or a `const` written at the top of a unit,
beside a function:

```minc
const maxUsers: i32 = 4096;      // a constant of the unit
let   requests:  u64 = 0;        // a mutable object with static storage

extern let environ: *str;        // a name defined elsewhere (the C runtime)
```

This record decides whether they exist, what their initializers may be, what
linkage they have, how a name at file scope is *evaluated*, and everything the
answer forces in the pipeline. It is the last of the three records that were
left as "one line in the roadmap under the same heading" — `memory.md` said a
global is one of the three things that produce an object, `extern.md` said
`extern let x: i32;` was a decision it was not making, and `resolve.md`
decision A said a file-scope name is visible independently of order. Those three
sentences are the constraints this record has to satisfy.

## The question is not "should there be globals"

Every systems language has them, and the reason is not nostalgia: a program that
cannot name storage outside a function cannot declare `errno`, cannot place a
lookup table in the image, cannot name a memory-mapped register, and cannot hold
a counter that is not threaded through every call in between. The question is
*which* globals, and — the part that actually decides the language — **what
their initializers are allowed to be**, because that is what determines whether
the language has an initialization order to get wrong.

### What the market actually does

| Language | File-scope binding | Initializer | Ordering |
| --- | --- | --- | --- |
| **C** | `let`-shaped, plus `static` for internal linkage | a **constant expression** | none — there is nothing to order, so C has no SIOF |
| **C++** | same, plus `constexpr` / `constinit` | constant *or* dynamic | **the static initialization order fiasco**: within a TU, textual; across TUs, unspecified |
| **Go** | `var` and `const` in the package block | any expression | **dependency-ordered**, computed per package; a cycle is a *compile* error; `init()` runs after |
| **Rust** | `static` (has an address) and `const` (inlined, no address) | a **constant expression** for `static` | none — no dynamic initialization exists; `static mut` is `unsafe` and is being retired |
| **Zig** | container-level `var` and `const` | **implicitly `comptime`** | none — the value is known before the program exists |
| **Swift** | `let` and `var` at top level | any expression | **lazily, on first use**, atomically; the order is the order they are first read |
| **Java / Kotlin** | `static` fields, top-level properties | any expression | textual within the class; unspecified across classes |

Three families, and each one pays for what it chose:

- **C, Rust, Zig require the initializer to be constant.** Nothing runs before
  `main`, so there is no order and no bug. The price is that a global cannot be
  computed at startup — `static TABLE: [u8; N] = build();` does not exist in
  Rust, and in Zig the initializer must be `comptime`-known.
- **C++ allows any expression and admits the fiasco**, then spent C++20's
  `constinit` on a way to *opt back out* of dynamic initialization. That is the
  tell: the language added a feature whose entire purpose is to recover the
  property the first family never lost.
- **Go and Swift make the order defined instead of removing it.** Go computes a
  dependency graph over one package and refuses cycles; Swift initializes on
  first use. Both work, and both pay a cost: Go's is a graph and an `init` phase
  before `main`, Swift's is a check on every access to a global.

### The constraint that picks the family

Go's answer is the best one available to Go, and it is **not available here**,
for a reason that is about this compiler rather than about the language:

> Go initializes a package set by compiling it together, so the graph is
> visible to one compiler invocation and the order it computes is the order that
> runs. `minc+` compiles **one translation unit at a time** and hands the objects
> to a linker (a C linker driver — `codegen.md`), so the set of TUs is not known
> to any invocation until the link. A dependency graph over a unit would order
> *within* the unit and leave every cross-unit pair unspecified — which is
> exactly C++'s fiasco, with more machinery serving it.

So the choice is between the first family and the fiasco. It is the first
family, and the language goes one step further than C, Rust and Zig all do, for
the reason in the next section.

## The decisions

| # | Decision | Why |
| --- | --- | --- |
| **1** | **Both `let` and `const` are file-scope bindings**, with the shape they already have in a block | `extern let` is *required* for interop: naming `errno`, `environ`, `stdout` needs a declaration of mutable storage this unit does not define. And `fn`/`extern fn` already established that a declaration and a definition are one entity, so the definition form is the counterpart, not a second feature |
| **2** | **A file-scope initializer must be an initializer constant expression** (`ICE`). There is no dynamic initialization at file scope — ever | The compilation model above: order across units is not knowable, so the language does not have an order. See below |
| **3** | **ICE evaluation is dependency-ordered, and a cycle is an error** | `resolve.md` decision A already made file-scope names order-independent, so `const a = b + 1; const b = 41;` must work. Go's algorithm, restricted to constants, is the only one consistent with a decision already shipped |
| **4** | **No initializer means zero-initialized**, and a `const` without one is an error | `memory.md` decision 12 (it is the C ABI's `.bss`, and an unspecified value would make every `extern` global ambiguous); `validate` already refuses `const` with no initializer in a block, and the file scope is the same rule |
| **5** | **Both `let` and `const` have external linkage, and `static` narrows either to internal. Visibility is not a linkage default — it is the module system's** | A file-scope binding is a member of the unit's namespace, and a constant is a *value a module exports*: `const maxUsers: i32 = 4096;` in one `.mx` is meant to be importable by another, exactly as a function is. Encoding that in a linkage default would make `pub` and linkage two spellings of one decision, and the split would have to be undone the day modules land. `static` is the word that means "this unit's, not the linker's", and it is C's word — this *is* a linkage question. See below |
| **6** | **A global's LLVM `GlobalVariable` is never `isConstant`**, whatever the source said | `memory.md` decision 15: `const` protects a *name*, not memory. Inferring `readonly` from it would put a `const` binding's object in `.rodata` and make it a different object from a `let`'s — which the ABI and the reader both see |
| **7** | **`#define` is not the constant, and the constant is not the `#define`** | Different scopes, different times. See below |
| **8** | **`extern let` / `extern const` are declarations**, and link to the definition in another unit or a C library | Same word, same meaning as in `extern fn`: *external linkage, the definition is elsewhere* |
| **9** | **The ICE's extent is closed and named**: integer folding via `support::consteval`, plus a literal of any scalar type, plus a reference to another file-scope `const`. Float *expressions* are refused by name | The value must be materialisable by the lowering without it recomputing anything (`ir.md`), so it must be a value the checker can *publish*. See *The value record* |

### Decision 2, in full

The rule is not "the initializer should preferably be constant". It is:

> **No code runs at file scope.** The bytes of a global's initializer are in the
> object file, placed there by the compiler; the loader maps them. Nothing is
> emitted into `.init_array`, there is no constructor, and there is no function
> whose ordering a linker could get wrong.

Everything else follows. The **static initialization order fiasco is not
avoided by discipline here — it is unrepresentable**, because a fiasco requires
two pieces of dynamic initialization and this language has none. That is a
stronger statement than `constinit` makes, and it is the same strength C, Rust
and Zig have — except that this language states the *reason* rather than leaving
it as a property of how the initializer happens to be written.

The cost is real and belongs in the language's own words: **a global cannot be
computed at startup.**

```minc
// Allowed: the value is known before the program exists.
const mask:  u32  = (1 << 8) - 1;
const scale: f64  = 2.5;
let   count: u64  = 0;
let   root:  *T   = null;

// Refused: this is work, and file scope is not a time at which work happens.
let table: *T = alloc(1024);          // sema-global-not-constant
```

The fix is spelled, and the diagnostic says it: initialize at the top of `main`,
or make the object a function-local `static` when that lands. There is no third
option that preserves a defined order.

### Decision 5, in full — a constant is a module's, not a header's

The tempting move is to decide this by analogy with C and C++, because that is
where the words `static`, `extern` and file scope come from. It is the wrong
analogy, and the reason is a decision the language has already made elsewhere:
**`minc+` is going to have a module system**, in which constants, functions and
the top-level types still to come are *importable by name from other `.mx`
files* (roadmap, *Declarations and modules*; `resolve.md` decision F has already
reserved the visibility predicate).

C's problem is not "file-scope declarations" — it is that C has **no module
system**, so the only way to share a declaration is to textually include it, and
the only way to stop N inclusions from becoming N definitions is to bend linkage
(`static const`) or a language default (C++ making namespace-scope `const`
internal). Both of those are compensations for the absence of a module system,
and neither is a statement about what a constant *is*.

So this record does not adopt either compensation:

- A file-scope binding has **external linkage**. It is a real object with a real
  symbol (`memory.md` decision 15 already settled that a `const` is an object and
  not an inlined spelling, so it has an address and therefore a symbol).
- **Visibility is a separate axis**, and it belongs to the module system:
  `pub`/`private` filters *lookup*, which is what `resolve.md` decision F
  decided, and it does not rewrite linkage.
- **`static` is the word that narrows linkage** — internal to this unit — and it
  is the honest answer today, before modules exist, for a shared `.mx` file
  included by two units. It is the same word C uses for the same job, which is
  the one place copying C is right: `static` is a *linkage* word, and this is a
  linkage question.

The pay-off of separating the axes is that **nothing here has to be reopened when
modules land.** A module operation filters lookup; the symbol set of a unit is
unchanged, so the object layout and the ABI are unchanged. Had `const` been
internal by default, a `pub const` would then have needed a way to *widen* it — a
second mechanism whose only purpose was undoing the first.

### Decision 2 and 3, and why the module system makes them stronger

A module system would ordinarily raise the cross-module initialization
question again: if module B imports a value from module A, when is A's value
built? Here the answer is already `never — it is a constant`. And because
decision 2 makes every file-scope initializer a constant, **an exported
constant's value is part of the module's interface**: an importer folds the value
and never has to wait for anything to run. The module system does not weaken
decision 2; it is the reason decision 2 costs almost nothing.

One forward note, recorded so it is not discovered late: `resolve.md` decision A
makes file-scope names order-independent "as in Go's package block", and a
package block is **per package, not per file**. The file is the unit of
*compilation* here; when modules arrive it will not be the unit of *visibility*,
and the order-independence rule should then be read as per-module. That is a
change of scope, not of rule — one collected set instead of one file's — and the
item tree already keys on the file, so the cache continues to work.

### What the module system will need from this record

`minc+` is going to have a module system, and this record was written to survive
it. Three constraints are worth stating here rather than leaving in that
record, because they are *this* record's to keep:

- **The symbol is a function of the declaration, never the declaration's
  spelling.** Two modules may each define `maxUsers`, so the linkage name cannot
  stay the source name once modules exist. `Lowering::linkageName` is already the
  one place a symbol is spelled — that is the seam, and it must stay the only
  one.
- **`extern` is the escape hatch, and it always means the bare symbol.** A
  declaration written `extern` names a symbol somebody else chose, so it is never
  mangled. `extern let environ: *str;` must keep resolving to the flat
  `environ` whatever happens to `minc+`'s own names — the same split `extern "C"`
  makes in Rust and C++, and Zig's `extern`/`export`.
- **The file scope is the module scope, one file wide.** A file is a module of
  exactly one file today; nothing else about the rule changes when that stops
  being true.

The rest — the constant-only initializer, the dependency order, zero-
initialization, `const` not being read-only memory, `#define` not being the
constant, `extern` and `static` — is independent of the module system and will
not be reopened by it. The forward record is
[`modules.md`](modules.md), which lists the seams and the questions.

One of those questions touches this record directly and is deliberately left to
it: **whether an exported constant's value is folded by an importer.**
`memory.md` decision 15 settled that a `const` is an object with an address, not
an inlined spelling, so an exported one needs a symbol and cannot be
instantiated per importer the way a Rust `const` is. The *value* is still
available for folding (`globals.md`, decision 2), and which side of that an
importer takes is an optimization decision rather than a language one — the
object must exist either way.

### Decision 7, in full — why not "just `#define` and `const`"

The question "is `#define` plus a top-level `const` enough?" has a shape that
makes it feel like a choice between two ways to write a constant. It is not:
they are two different things that happen to both name a value.

| | `#define` | file-scope `const` |
| --- | --- | --- |
| When it applies | preprocessing, textually | type checking and after |
| Is it a value? | no — a token sequence | yes — a typed object |
| Does it have a type? | no | yes |
| Does it have an address? | no | yes (and the language can take it) |
| Debugger / `-g` | invisible — no symbol, no line | visible, with its type |
| `source_to_def` / LSP | invisible | an ordinary definition |
| Can two of them share a name safely? | no — it is global text | yes — scoped, shadowable |
| Is it importable by another unit? | no — it is text in *this* translation unit | yes — it is a member of the unit's namespace, and the module system is what exports it |
| Can it be an aggregate? | only as a spelling | later, yes, as an object |
| Its scope | from the `#define` to the end of the TU | the unit, order-independent |

So `#define` cannot be the constant, because a constant is a *value with a
type*, and five of those rows are questions a type checker asks. And `const`
cannot be the `#define`, because a `#define` is *text*, and the jobs it does
that no typed entity can do are the ones the preprocessor exists for: include
guards, conditional compilation, token pasting, and a spelling that must be
substituted into a declaration's syntax rather than into its meaning.

The rule that falls out of the table is the one already used for the type
system: **`#define` is for things that must be seen before the grammar;
`const` is for things the grammar and the type checker must see.** `const
maxUsers: i32 = 4096;` and `#define MAX_USERS 4096` are not two spellings of one
idea — the first is a value in the program, the second is a word in the source.

## The surface

```minc
// A definition. Both are external: a file-scope binding is a member of the
// unit's namespace, and the module system is what will decide who may import
// it. `static` is the word that makes one internal to this unit.
const maxUsers: i32 = 4096;
let   requests: u64 = 0;

// Annotation optional, inference as in a block.
const limit = 100;               // i32
let   ratio = 1.5;               // f64

// The type is the annotation's or the initializer's; an integer ICE of another
// width is the annotated type, exactly as `let small: u8 = 10;` already is.
const small: u8 = 10;

// Aggregates are not here yet; when they are, a table is a definition:
// const primes: [8]i32 = [ 2, 3, 5, 7, 11, 13, 17, 19 ];

// A declaration: the definition is elsewhere — another unit, a library, the C
// runtime. Same word and same meaning as in `extern fn`.
extern let   environ: *str;
extern const errnoMacro: i32;

// Zero-initialized: a `let` with no initializer is the C ABI's `.bss`.
let   flag: bool;
```

Two shapes, and both already exist in a block: this record adds a *position*,
not a grammar.

## The value record

The pipeline's rule (`ir.md`) is that a stage materialises what the stage above
recorded and **decides nothing**; a decision it cannot read is a failure, not a
guess. A file-scope initializer is therefore not "an expression the lowering
folds" — it is **a value the checker publishes**.

The pieces are already in the tree:

- `support::consteval` is the shared constant core — `ConstInt` (bits plus
  signedness, wrap-around, `nullopt` for the two operations that are not a
  value) and the literal readers. `pp` uses it for `#if` and `sema` for
  literal range checking, and this record is the third caller, not a fourth
  implementation. `#if`'s arithmetic and a file-scope initializer's arithmetic
  are the same arithmetic.
- `sema::ExprInfo` already carries `isConstant`, `hasIntValue` and `value`, and
  `sema` already records a `const` binding's integer value in
  `defConstValues_` / `defHasConstValue_` when it checks a `ConstStmt`.

What is added is that the ICE is *evaluated in dependency order and memoised by
`DefId`*, so a reference to a constant defined further down is answered by the
constant's own value rather than by a second walk of its initializer. An
in-progress mark is what turns `const a = a + 1;` into `sema-global-cycle`
instead of a stack overflow.

The one hole is **floating point**, and it is a hole the language already
documents rather than a new one: `sema` deliberately does not fold floats
("the value would need a float parser whose rounding this stage cannot verify",
`typed_ast.h`), and `src/ir` reads a float literal's bits with LLVM's `APFloat`
because it is the only reader in the compiler that can. So:

- a float **literal** (`const pi: f64 = 3.14159;`) is a value, because its bits
  are read from the spelling by the lowering exactly as a local's already are;
- a float **expression** (`const half = pi / 2.0;`) is refused
  (`sema-global-not-constant`, naming the operation), because folding it needs
  a rounding `sema` cannot verify. The fix is a `support/consteval` float reader
  that mirrors `ConstInt` — a *support* addition that would be reused by `#if`
  and by the checker at once, which is why the refusal is honest rather than
  permanent.

A string constant is the same shape: the object is the private NUL-terminated
`[N x i8]` global the lowering already builds for a literal (`ir.md`), and a
`str` global's initializer is a pointer to it.

## Where it lands

| Stage | Change |
| --- | --- |
| `lex` | nothing: `extern`, `let` and `const` are already `KwExtern`, `KwLet`, `KwConst` |
| `parse` | `isItemStart` gains `KwLet`/`KwConst`; `parseItem` dispatches to the binding productions, which already exist as `parseLetStmt(false/true)`. The file-level binding builds the **same `LetStmt`/`ConstStmt` node** a block-level one does — one production, two positions |
| `syntax` | nothing new: `VariableStmt::cast` already reads either node kind |
| `lower` | `collectItems` accepts the two binding kinds. `Item` gains the binding's meaning: its **`span` ends at the type annotation** (or at the name, with no annotation) and its **`body` is the initializer**, so the *editor invariant* holds — editing a constant's value does not invalidate the unit's scopes, exactly as editing a function's body does not |
| `validate` | the file scope inherits the block scope's rules: a `const` with no initializer is refused, a file-scope `let` with no initializer is legal (zero) |
| `resolve` | `collectItems` inserts a binding as `DefKind::Variable` (`let`) or `DefKind::Constant` (`const`), with `Linkage::External` (decision 5) and the same canonical-chain machinery as a function. It is the *same* collect pass, so file-scope order-independence and the redeclaration rules come for free |
| `sema` | the file-scope walk checks each binding with the existing `LetStmt`/`ConstStmt` path, then adds the three file-scope rules: the initializer must be an ICE (2), it is evaluated dependency-ordered with a cycle check (3), and the resulting value is published per `DefId`. `extern` bindings are typed from their declaration with no initializer |
| `ir` | a `declareGlobals()` beside `declareFunctions()`, in the same all-declarations-then-all-bodies shape: `GlobalVariable` with the mapped type, the def's linkage, `isConstant = false` (6) — which the assumption scan then *keeps* true rather than merely relying on (`ir.md`, the `ConstantObject` row) — and an initializer materialised from the published value — `zeroinitializer` (4), an integer constant, an `APFloat`, the string global's pointer, or a null pointer. The conversion into the object's type is read from the coercion record, never derived from the two types (`ir.md`, decision 34) |
| `backend` | nothing: a global is data, and the object writer already emits `.data`/`.bss` |
| `driver` | nothing: `mincc ir` prints the module, so a global is visible there; no new command |
| `cinterop` | `extern let` / `extern const` are one of the boundary's two directions, and this record decides only the *form*. The type mapping stays `cinterop`'s (roadmap §8) |

## What this makes impossible

The list is the argument for the record, so it is stated as an inventory:

- **The static initialization order fiasco.** There is no dynamic
  initialization, so there is no order for a linker to choose.
- **`constinit` / `constexpr` as an opt-in.** C++ needed a keyword to *ask* for
  what this language does not need to be asked for. There is no second mode.
- **A global that depends on the environment at load time.** `environ`, a
  `TZ`, a locale — a global cannot read them, because reading is work. It reads
  them in `main`, which is where a program starts.
- **The "static initialization is a hidden cost" class of startup.** Nothing
  runs, so nothing is slow.
- **`static` meaning a storage duration.** At file scope there is only one
  storage duration, so `static` stays a *linkage* word: it narrows a binding to
  this unit. It cannot mean "survives the call" — a file-scope binding already
  does — which is the reading that makes `static` confusing in C's function
  bodies.

## What is deliberately not decided here

| Question | State |
| --- | --- |
| `static` (internal linkage for a binding) | **decided and shipped** with this feature: without it, a `const` in a shared `.mx` file included by two units is two external symbols. It is a lexer keyword, `resolve` writes `Linkage::Internal` for it, and `src/ir` reads that linkage for a function and an object alike (`ir.md`) |
| Thread-local storage | reserved: `memory.md` § *Concurrency* reserves the concurrency model, and `thread_local` is a storage-duration question for that record |
| Module-level visibility (`pub`/`private`) | `resolve.md` decision F: it filters lookup rather than rewriting linkage. When modules land, the order-independence of decision 3 becomes per-module |
| Runtime initialization (`.init_array`, a constructor, a Go-style graph) | **closed for the language**, open only as a future *feature with its own record*. It cannot be added by relaxing decision 2 quietly, because relaxing it is what reintroduces the fiasco |
| Aggregates, arrays, and a `const` table | waiting on arrays. The ICE and the linkage rule are already shaped for them |
| A `readonly` annotation that *would* permit `.rodata` | `memory.md` decision 15 says the compiler may not infer it. An explicit annotation is that record's question, not this one's |
| `sizeof`/`static_assert` inside an ICE | they arrive with those operators (`builtins.md`); they are constants by construction, so they join the ICE without a decision |

## How the claims above are checked

| Claim | Checked by |
| --- | --- |
| A file-scope binding is *one* production | the syntax suite parses the same spelling in a block and at file scope and asserts the **same node kind** |
| The linkage is external for both, and `static` narrows it | a resolve test reads `Def::linkage`: `External` for a plain `let` and a plain `const`, `Internal` for a `static` one — the regression test for a future "`const` should be internal" reflex |
| A module's exported constant needs nothing to run | every `GlobalVariable` in the module for a file-scope binding has an **initializer that is a constant**, so no global's value depends on another global's initialization: asserted over a module with two constants where one names the other |
| Order-independence and dependency order | `const a = b + 1; const b = 41;` evaluates to `42`; the pair is also written in the other order and gives the same answer |
| A cycle is a diagnostic, not a hang | `const a = b; const b = a;` and `const a = a;` produce `sema-global-cycle` once, with no second sentence |
| The fiasco cannot be written | a corpus that tries `let x = f();`, `let x = alloc(4);` and a call in a `const` initializer, each with its own `sema-global-not-constant` at the call, and the fix named |
| The object is not `readonly` | the module's globals are asserted `isConstant == false` — the private `[N x i8]` behind a `str` included, since LLVM's own `CreateGlobalString` sets the flag — and the rule is **scanned**: `ir.md`'s assumption list has a `constant` row, and `invariants_test.cc` flips the flag on a module this compiler built and asserts the scan reports it. That pair is the regression test for a future "optimization", and it fails on the reflex rather than on the review |
| Zero-initialization is `.bss` | an `ir` test asserts `zeroinitializer` for an uninitialized `let`, and an end-to-end `build`/`run` asserts the program observes `0` |
| The initializer is not re-decided by the lowering | a test walks every `GlobalVariable`'s initializer and shows it was materialised from `defConstValues_`/the published record: an integer, a float, a pointer or zero, and **never** an instruction |
| A float literal works, a float expression is refused by name | one of each, asserting the accepted one's bits and the refused one's code and message |
| The editor invariant | editing a constant's *value* leaves the `ItemTree` equal and the file scope reused; editing its *type* or name does not — the same pair of tests the function path already has |
| A global is the thing `memory.md` said it is | an end-to-end `&x` on a file-scope binding takes an address, that address is stable across calls, and `*p = 1` through it is observed by the next read |
| Interop names a global the unit does not own | `extern let` compiles to a `GlobalVariable` that is a declaration only (no initializer), and the linked program reads the symbol the C runtime defines |
| Adversarial input is bounded | a unit with the `kMaxSymbols`-sized pile of constants, and a chain of N constants each naming the next, both under the sanitizer preset |

## References

- The Go specification, *Package initialization* — "proceeds stepwise, with each
  step selecting the variable earliest in declaration order which has no
  dependencies on uninitialized variables", and a dependency cycle is a
  compile-time error: <https://go.dev/ref/spec#Package_initialization>
- The Zig language reference, *Container Level Variables* — "the initialization
  value of container level variables is implicitly `comptime`; if a container
  level variable is `const` then its value is `comptime`-known": the same
  requirement this record makes, expressed in Zig's own terms:
  <https://ziglang.org/documentation/master/#Container-Level-Variables>
- cppreference, *Static initialization order fiasco* — the ambiguity this
  record removes by construction:
  <https://en.cppreference.com/w/cpp/language/siof>
- C++20 `constinit` — a language adding a keyword to recover constant
  initialization, which is the strongest available argument for requiring it:
  <https://en.cppreference.com/w/cpp/language/constinit>
- Rust Reference, *Static items* and *Constant items* — a `static` has one
  address, a `const` is inlined, both initializers are constant expressions, and
  `static mut` is `unsafe`:
  <https://doc.rust-lang.org/reference/items/static-items.html>
- Rust RFC 246, *`const` vs `static`* — the address distinction, stated as a
  design decision: <https://rust-lang.github.io/rfcs/0246-const-vs-static.html>
- The Swift Programming Language, *Global and Local Variables* — global
  constants and variables are always computed **lazily**, which is the third
  answer and the one this language cannot take because it compiles a unit at a
  time: <https://docs.swift.org/swift-book/documentation/the-swift-programming-language/>
- Doug Gregor, *Swift for C++ Practitioners, Part 8: Global Variables* — the
  lazy-initialization model written down for a C++ audience, including why it
  avoids the fiasco: <https://www.douggregor.net/posts/swift-for-cxx-practitioners-global-variables/>
- The Rust Reference, *Linkage* — a `const` has no linkage and a `static` does,
  the distinction decision 5 bends to a header's needs:
  <https://doc.rust-lang.org/reference/linkage.html>
