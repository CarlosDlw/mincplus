# IR — `src/ir`

The design record for the stage the pipeline puts between `sema` and `codegen`:
`architecture.md#the-pipeline` says it receives a **typed AST** and returns a
**module**. This document fixes what that module is (LLVM's), what the lowering
may decide (nothing that the front end already decided), how the decisions `sema`
made become instructions, and — the part that matters more than either — the
mechanical rules that make a *future* construct impossible to add without the
compiler telling us, in one place, exactly what it broke.

Research behind it: the LLVM 22 `LangRef` and `ORC` design documents, the
opaque-pointer migration notes and their consequences for every memory
operation, `rustc`'s split between a backend-agnostic codegen layer and
`rustc_codegen_llvm` (and the reason `Layout`/`FnAbi` exist as a *shared* source
of ABI truth), Clang's `CodeGenModule`/`CodeGenFunction` and the `LValue`/`RValue`
distinction it is built on, Swift's `IRGen`, Zig's LLVM backend, and the LLVM
`HowToUseLLJIT` example that the `run` command is modelled on. Every claim below
about what LLVM *does* is quoted from this machine's `LangRef`: the declaration
syntax of `load`/`store`/`alloca` and the fact that the alignment is **explicit
in in-memory IR** ("Overestimating the alignment results in undefined
behavior"), that `icmp` on pointers "compares the address bits ... as if they were
integers", what `inbounds` requires of a `getelementptr`, and that
`llvm.lifetime.start`/`end` take only the pointer. The version installed and
probed here is **22.1.8**, as are the facts that depend on the build
configuration (`LLVMConfig.cmake`, not assumed). The provenance side of the
same `LangRef` — allocated objects, object lifetime, `noalias`, `captures`,
and `ptrtoint` versus `ptrtoaddr` — is read in [`memory.md`](memory.md) and is
deliberately not repeated here; that document owns the rules, this one owns what
they are emitted *as*.

**Status: shipped.** `src/ir` builds an `llvm::Module` and `mincc ir` prints it
for every example in the corpus; the module verifies, and the boundary holds —
`grep` for `llvm/` outside `src/ir` is a test, not a habit. Everything this
document asked of an earlier stage is **implemented and published by `sema`**
(`sema.md`, *What the artifact publishes*): the coercion record and
`ExprInfo::opType` (§ *The fourth fact nobody recorded*, § *The coercion
record*), the guarantee that no node in the artifact carries a deferred literal
type, the target as a canonical **triple**, and — stage one of
[`memory.md`](memory.md), which is settled — the **access record**, one
`AccessObligation` per dereference (§ *The access record*).

That last one changes a line of this document rather than adding to it:
`TypeKind::Pointer` is no longer a kind this stage refuses *by name*. It has a
surface (`*T`, `&x`, `*p`, `p[i]`) and it maps to LLVM's `ptr`. What remains is
the lowering itself, plus the parts of the memory model whose syntax does not
exist yet (the checked layer, `expose`/`with_exposed_provenance`, `unaligned`
and `volatile` accesses) — and the one thing this document owes that document:
the **assumption list**, which is § *The assumption list* below and is enforced
by a scan rather than by a sentence.

## Why LLVM, and what that decides

The IR is LLVM's. Not "ours, with an LLVM backend later": `src/ir` builds an
`llvm::Module` and hands it on. That was decided when the alternative was priced
(`roadmap.md` § 6), and the consequences are worth stating because they are not
all obvious:

- **The CFG is not ours to build.** A structured statement becomes blocks and
  terminators, and the dominance frontiers, the phi placement and the
  optimisation are LLVM's, which is why this stage is *smaller* than the design
  it replaces, not larger.
- **`codegen` stops being a compiler.** It selects a `TargetMachine` from a
  triple, runs a pass pipeline, and emits an object. It contains no instruction
  selection, no register allocation and no ABI classification of its own.
- **`link` stays a driver concern.** Emitting an object via LLVM and invoking
  `cc`/`ld` (or `link.exe`) is the whole of it.
- **The front end does not learn about LLVM.** `support` remains LLVM-free by
  contract, and this stage is the first one that may include `llvm/*`. § *The
  isolation rule, as a test* turns that sentence into something the build
  enforces instead of something a reviewer remembers.
- **Cross-platform becomes a triple.** One module, four triples, four objects —
  already proved by a probe on this machine. Nothing in the lowering knows which
  target it is for; the data layout and the ABI arrive with the triple.

The one thing this buys and the one thing it costs are both real. It buys a
backend that is not ours to get wrong. It costs the freedom to define IR
semantics that are *easier* than LLVM's — and that is the subject of the next
section, because the way this project stays safe on top of LLVM is by never
handing it an instruction whose semantics we have not made true.

## What the stage receives, and what it may assume

### The precondition, enforced rather than trusted

The lowering runs on a typed tree with **zero errors**, and it *checks* that
rather than assuming it. That is not belt-and-braces: `sema`'s poison
(`TypeKind::Error`) is a real type that converts to and from everything
silently, so a lowering that accepted a poisoned tree would emit well-formed IR
for a program whose meaning nobody decided. The rule is one sentence, in one
place:

> **A stage that cannot answer a question does not emit.** If the tree carries a
> poison type, an `ir-internal` diagnostic is produced and no module is built.

Two things follow from the definite-assignment pass, and they are why this
stage can be honest about a question every C compiler has to lie about:

- **No `undef`.** Every read was proved assigned on every path that reaches it,
  so the lowering never needs an undetermined value. `undef` is not merely
  deprecated in LLVM's model; it is the value that lets an optimiser choose,
  which is exactly what this project's "no undefined behaviour in integer
  arithmetic" checklist item exists to refuse.
- **No uninitialised storage read.** `alloca` starts undef by definition; the
  proof that nothing reads it before a store is what makes the `alloca` +
  `mem2reg` scheme below sound rather than merely conventional.

### The three facts the lowering is forbidden to re-derive

The typed tree is a *parallel array* beside the lowered tree (`sema.md`, § *The
typed AST*): `typeOf(node)` for every node, `infoOf(node)` for expressions, and
a function table. It is total on expressions and idempotent.

Three properties of it are load-bearing here, and the lowering must read them
rather than recompute them:

1. **`typeOf` is the type of the value the node produces.** For an arithmetic
   operator that is the *operation* type — C's usual arithmetic conversion
   result. Measured on the tree `sema` produces today:

   ```
   let c: u8 = a + b;          // a, b: u8
     LetStmt    : u8
       BinaryExpr : i32          <- the operation is `add i32`
         PathExpr <a> : u8       <- ..., after a zext
         PathExpr <b> : u8
   ```

   So the lowering never asks "what is the common type of `u8` and `u8`"; it
   reads `i32` off the node and widens the operands to it. The rule that
   produced `i32` lives in `sema/convert.h` (`promote`, `usualArithmetic`) and is
   called once, by `sema`.

2. **A mismatch between a child's type and its consumer's is the conversion.**
   `let w: i64 = a + b;` is an `i32` expression inside an `i64` binding. The
   conversion is derivable from the pair `(i32, i64)`, and it is *required* by
   LLVM, not optional: `add i64 %a, %b` where the operands are `i32` is a
   verifier error, so the seam is not silent — it is a hard failure. See § *The
   coercion record* for the half of this that is derivable and the half that is
   not.

3. **A function's parameters and return type** come from
   `FunctionInfo`/`typeOf`, never from re-reading the declaration's `Type` node.
   The `Type` node is an *identifier run* whose meaning `sema` decided against a
   target's ABI; reading it again is how a cross-compile silently disagrees with
   itself.

### The fourth fact nobody recorded: the operation type

There is a hole, and it is worth showing rather than describing. `sema` types a
compound assignment as its *result* type, and the operation inside it happens at
a different type that appears nowhere in the tree. Measured:

```
let x: u16 = 1;
x <<= 9;                       // accepted
  AssignExpr : u16             <- the store's type
    PathExpr <x> : u16 [lvalue]
```

The operation is `x << 9` at `u16`'s **promoted** type, `i32` — that is why the
count `9` is legal (`sema` checks a shift against the promoted left operand's
width, `sema.md` § *Integer arithmetic at the edges*). A lowering that read
`AssignExpr`'s type and emitted `shl i16 %x, 9` would produce an out-of-range
shift, which is a **poison value** in LLVM: not a crash, not a diagnostic, a
result the optimiser is free to replace. The same hole exists for every
`op=`: `x /= e`, `x %= e` and `x >>= e` on a type narrower than `int` are all
defined at a type the tree does not name.

This is the shape of failure the whole document is about. It is not a bug in
`sema` — the *check* is right, and the *typing* is right. It is a fact the IR
needs and the front end does not currently publish, and the only two ways out
are (a) the lowering re-derives the promotion rule, which is a second
implementation of `convert.h` and therefore a place the two can disagree, or
(b) `sema` records it. (b) is the answer, and it is the first work item.

### The coercion record

The typed file grows one table, and it is the only change this document asks of
an earlier stage:

```
struct Coercion {
  ast::AstId consumer;     // the node applying the conversion
  std::uint8_t operand;    // which operand of it, source order, tokens excluded
  ast::AstId node;         // the expression whose value is converted
  TypeId from, to;         // the pair the rule was applied to
};
```

`consumer` is the key, and the distinction from keying it on the value is not
cosmetic: a consumer is where the conversion is *applied*, and an expression has
one consumer only while the tree stays a tree. `node` is kept beside it so the
pair can be checked against the tree — `from` must equal `typeOf(node)` — and so
a consumer the walk removed (a folded `?:` arm) is visible rather than implied.
The list is sorted by `(consumer, operand)` once, at the end, and indexed per
consumer, so the lowering asks *"what does this consumer do with operand `i`"*
and pays a lookup rather than a scan. `TypedFile::coercionAt(consumer, operand)`
is that question, and `coercionsOf(consumer)` is the whole list for a node.

The record has two producers, both of them `sema`, both of them already visiting
the site:

- **Every implicit conversion** recorded where the conversion applies:
  initializer, assignment, `return`, argument, `?:` arms, binary and shift
  operands to the common type, the operand of a promotion (`-`, `+`, `~`), and the
  store half of a compound assignment. `from` is the operand's own type and `to`
is where the value lands, so the lowering has a pair and no rule. A conversion
  that changes nothing (`from == to`) is **not** recorded — the lowering reads
  "no entry" as "no conversion" — and a pair the language refuses (`bool`, `str`)
is not either, because the checker reports it instead.
- **The operation type of a compound assignment**, which is a property of the
  `AssignExpr` itself and lives in `ExprInfo::opType` rather than in the list
  above. One `TypeId` per `op=` node, and the store's conversion of the result
  back into the target is *implied* by it: `opType != typeOf(expr)` is exactly
  the `trunc` the lowering must insert before the store.

Why a record rather than a shared function call: a shared `promote` would be one
implementation of the rule, which satisfies the letter of this project's
"one implementation, not two" rule, and it is what the alternatives in other
compilers do. It is not enough here, for a reason that shows up the moment the
language grows: **the lowering would still hold arithmetic-conversion logic**,
and any future construct whose typing is *context-dependent* — C's default
argument promotions, an implicit receiver, a later `str`-to-`slice` conversion,
anything resolved by looking at both sides — cannot be recovered from a pair of
types at all. Rust makes the equivalent argument for its own middle end: the
reason MIR exists as a separate IR is precisely so that codegen does not have to
recover what typing already knew. The record makes the lowering a *materialiser*,
which is a property it keeps for the whole life of the language.

To be exact about what is necessary and what is a choice: most pairs *are*
derivable today — an operand's own type against the operation type its parent
names is enough to see that a `zext` belongs there. Compound assignment is not,
and that is the blocker. The rest is recorded anyway, because the difference
between "derivable with care" and "recorded" is the difference between a rule
this stage can get wrong and one it cannot, and because the pairs that stop being
derivable arrive with the features, one at a time, each with its own pull
request.

One more thing the record needed, and it is why the two changes shipped together:
a conversion's ends must be **types the IR can name**, so no operand may still be
a deferred literal when the record is built. `sema` decides them — at the seam,
where the context is known, and then in a sweep down the unit's tree for whatever
a seam did not reach (`1 + 2.0` inside an `f64` binding is two `f64`s, not an
`i32` and an `f64`). Section § *The coercion record* is therefore inseparable from
"the artifact holds no deferred type", which is a property `sema`'s example test
asserts over every node of every example.

The migration is small and mechanical because `sema` already has the rule in one
function and calls it at every seam. The test that keeps it honest is in §
*Tests*: the record must cover every `(from, to)` pair `convertible` permits, and
it is checked by enumeration, not by sampling.

### The access record

[`memory.md`](memory.md) makes the same argument for memory that
§ *The fourth fact* makes for arithmetic, and it makes it from the other side:
every access carries an obligation (inside a live object, at the type's
alignment, over bytes that were written) and the compiler is allowed exactly the
assumptions the program *stated*. The consequence for this stage is a second list
of facts it must **read** rather than recompute — the memory half of the one
above, and four items long:

1. **the alignment of an access** — from the accessed type and the target's data
   layout. Re-deriving it means a second copy of `sema`'s layout table, and LLVM
   makes the stakes explicit: an overestimated `align` is *undefined behavior*,
   not slow code.
2. **whether the access is ordinary, unaligned or volatile** — three different
   emitted shapes (`load`/`unaligned`, `load volatile`) that the tree's type does
   not distinguish;
3. **the type being accessed**, which is where the size comes from — the pointer
   does not carry it, because LLVM's pointers are opaque;
4. **the pointer's provenance**, which is not a fact about the address at all.

`sema` publishes them, per access node, and the shipped shape is:

```
struct AccessObligation {
  ast::AstId place;          // the `*p` or the `p[i]` the lowering is standing on
  TypeId type;               // what is accessed: its size and its alignment
  AccessKind kind;           // ordinary | unaligned | volatile
  ProvenanceKind provenance; // object | foreign
};
```

`TypedFile::accesses()` is the list and `accessAt(place)` is the question, and it
is a **scan of a list that has one entry per dereference in the unit**, which the
lowering asks once per dereference it lowers. That is the same shape as the
coercion index and for the same reason: the record is written where the
information exists and read where it is needed, and neither side holds a rule.

What a record looks like on a real program, from `mincc check --ast` on
`examples/009_pointers.mx`, is worth reading because of what is **not** in it:

```
# accesses 7
  21  ordinary  foreign  i32
  27  ordinary  foreign  i32
  ...
```

Two fields are deliberately narrower than the model allows, because a stage can
only publish what the grammar lets it see, and a value no input can produce is a
value no test can pin:

- **`kind` is `ordinary` alone.** `unaligned` and `volatile` arrive with the
  syntax that asks for them; the enumeration grows with the keyword, and so do
  the two consumers (the `align 1` access shape and the `volatile` access shape).
- **`provenance` is `object | foreign`**, which is the *proof* a syntactic rule
  can carry: `object` for the address of an object this unit named and moved only
  by arithmetic since, `foreign` for everything the compiler cannot name — a
  parameter, a value read from memory, a call's result. The rule is
  **incomplete on purpose and errs in the only safe direction**, which is why
  every `i32` through a parameter above is `foreign` even though the program
  plainly means the caller's object: the compiler cannot see the caller, so it
  assumes nothing. The `derived | exposed | with_exposed` split belongs to the
  operations that join a pointer and an integer (stage three); when they land,
  `foreign` refines into `exposed` and `with_exposed` and nothing else in the
  record moves.

The converse rule is the project's oldest one, and here it has a name: **an
access node with no recorded obligation is a refusal, not a guess.** It is
`ir-missing-obligation`, and it is an **ICE** — a bug in this compiler, not a
statement about the user's program, because a program that type-checked cannot be
missing one. A lowering that emitted an access with a guessed alignment or a
guessed provenance would be exactly the second implementation of `sema`'s rules
that this document exists to prevent.

`ProvenanceKind` earns its place a second time in § *The checked build's guards*,
where it decides *which half* of the checked build can answer at all — the module
can check a `null`, an alignment and an `object`-provenance extent, and only a
shadow memory can check the rest. That is a load-bearing decision hiding in a
two-value enum, so it is stated here as well as there.

## The shape of the module

`src/ir` is not one file. The split follows the pipeline's own seams and is the
same rule the rest of the compiler follows:

| File | What it owns |
| --- | --- |
| `lower.h` / `lower.cc` | the entry point, the precondition check, the per-unit state (`IRUnit`), and the walk over the file's items |
| `types.cc` | the single `TypeId → llvm::Type*` mapper, and the function-type mapper |
| `values.h` | `Value` and `Place` — the two things an expression can evaluate to — and the exhaustive table over node kinds that produces each |
| `declarations.cc` | globals, function declarations, linkage, names |
| `function.cc` | one function: parameters, the entry block, the return, the `main` special case |
| `stmt.cc` | statements: blocks, `if`/`else`, loops, jumps, returns |
| `expr.cc` | expressions: the arithmetic, the coercions, the calls, and every access, read out of `TypedFile::accessAt` rather than decided |
| `runtime.cc` | the four semantic guards (`/`, `%`, shift counts, `INT_MIN / -1`) and the checked build's module-statable access guards (null, alignment, an `object`-provenance extent) |
| `invariants.cc` | the post-lowering scans: the runtime contract, the assumption list, and that every emitted alignment equals the record's |
| `diag.h` | `IRDiagnostic` — this stage's errors as values, like every other stage |

**One `IRUnit` owns everything LLVM for one translation unit: the
`LLVMContext`, the `Module`, the `IRBuilder<>`, the type mapper's cache and the
diagnostic list.** The context is not optional to get right — an `LLVMContext`
is not thread-safe and types are owned by it, so a context shared between two
units is a data race that appears only under the editor's concurrent work. One
per unit, one per worker thread, and nothing here is a global.

The **target is `sema`'s, and it is a triple.** `TargetInfo` now carries the
canonical LLVM spelling (`x86_64-unknown-linux-gnu`) and not a name from a
private enum, so this stage reads the identity it needs out of the typed artifact
and never converts between two spellings of one target. The module's triple and
its data layout both come from there; the target machine does not.

The **target machine does not live here.** `ir` produces a module with a triple
and a data layout string, which is all the lowering needs to be correct; choosing
and owning a `TargetMachine` — the half that is expensive, target-specific and
sometimes unavailable — is `src/backend/llvm`'s. That split is what lets `mincc
check --emit=ir` print IR on a machine with no LLVM target configured for the
one you are compiling for, and it mirrors the front end's own rule that no stage
depends on what a later one does.

## Types: one mapper, total over `TypeKind`

One function, `llvmType(TypeId) -> llvm::Type*`, and it is **total**: the switch
enumerates every `TypeKind` with no `default:`, which under this project's
warnings (`-Wall` includes `-Wswitch`, and CI adds `-Werror`) means a new kind is
a compile error here rather than a silent gap.

| `TypeKind` | LLVM | Note |
| --- | --- | --- |
| `Void` | `void` | only as a return type |
| `Bool` | `i1` | the language's `bool`; `i1` is what `icmp` produces, so no conversion is inserted to make a condition work |
| `Char` | `i8` | always unsigned by decision, so `i8` and *not* `i16` — the width is the point of the type |
| `Int` | `iN` for `bits` | `isize`/`usize` resolve through the target table, so the lowering reads a width and never a name |
| `Float` | `float` / `double` / `x86_fp80` | see below |
| `Str` | `ptr` | one private global per literal, NUL-terminated |
| `Function` | `FunctionType` | parameter list from the store's parameter array |
| `Pointer` | `ptr` | opaque: LLVM 22 has **one** pointer type, so the pointee is not in it (below) |
| `IntLiteral`, `FloatLiteral` | **refused** | a decided type by the time `sema` is done; reaching here is an invariant break, not a case |
| `Error` | **refused** | the precondition in § *What the stage receives* |
| `Array` | **refused by name** | its syntax does not exist yet, and the refusal is `ir-unsupported-type`, not a crash |

Four of those rows deserve more than a table cell.

**`f80` is not a portable type.** It maps to `x86_fp80`, and on a target whose
ABI has no 80-bit float that is either an unsupported type or a target-specific
substitution — LLVM will not paper over it. The mapping is therefore
*triple-aware*, and the honest answer for now is that `f80` is supported where
the triple says it is and refused where it says it is not. The lowering asks the
target (the data layout, which arrived with the triple), not the host.

**A pointer is `ptr`, and the pointee is not in the type at all.** LLVM 22's
pointers are opaque: `*i32`, `*f64` and `*void` are the *same* `llvm::Type`. That
is not a loss of information, it is a relocation of it, and where it goes is the
record again. The pointee survives in exactly three places, every one of them
read from the tree or from `AccessObligation` and none of them read out of the
LLVM type:

- the **element type of a `getelementptr`**, which is an explicit operand
  (`getelementptr i32, ptr %p, i64 1`) precisely because the pointer does not
  carry it — so `p + 1` and `p[1]` are the same instruction with the same element
  type, and the stride is `sema`'s `sizeOf` of it;
- the **index type**, which LLVM wants in the pointer index width — the same
  width `sema` recorded as the conversion target of an index, so the lowering
  reads `coercionAt(indexExpr, 1)` and `toInt64`/`toInt32` is not a decision it
  makes;
- the **access's size and alignment**, which come from `AccessObligation::type`
  and from nowhere else (§ *The access record*).

So `types.cc` is deliberately *not* where pointer semantics live. It is total on
`TypeKind` and says only "a pointer is a pointer"; every question that needs the
pointee is answered by the node the lowering is standing on, which is also what
keeps `*void` from being a special case anywhere below this paragraph.

**`i128` and `-Wconversion`.** The project builds with `-Wconversion` and
`-Wsign-conversion` on, and LLVM's integer APIs are unsigned-heavy: widths,
indices, alignment and address spaces are all `unsigned`, while this compiler's
own width is `std::uint16_t` and its indices are `std::uint32_t`. Every crossing
needs an explicit `static_cast` at the boundary, in `types.cc` and nowhere else,
or the gate fails. That is a feature: the narrowing is *stated* once per call
site instead of being implicit.

**Alignment and `bool` in memory.** `bool` is `i1` in a register, and `i1` is not
a byte. `*bool` is now expressible (`let p: *bool = &flag;`), so the question is
live rather than hypothetical, and the answer is that a `bool` *object* is `i8`
with a normalising store (`store i8 (zext i1)`) and a truncating load.

The reason is `memory.md`'s access rule rather than taste. `store i1` and
`load i1` are both legal, and `load i1` reads "at most one byte" — but what the
*other seven* bits of that byte hold after a `store i1` is something the LangRef
does not state, while the model does: every byte the access covers has been
**written** (access rule 4), and a later `u8` load through a `*u8` is punning the
model defines and must therefore see a defined byte. Storing the zext'd byte puts
that in the instruction instead of relying on a padding rule the IR leaves open.
So `i1` never appears in memory, only in a register; the instruction pair is
`i8`, and `AccessObligation::type` for `*bool` is `bool` — size 1, align 1.

## Values and places

An expression evaluates to one of exactly two things, and the distinction is the
one Clang's `LValue`/`RValue` exists for:

```
struct Value { llvm::Value* v;  TypeId type; };   // a computed value
struct Place { llvm::Value* addr; TypeId type; }; // an address, and what lives there
```

Everything that consumes an expression wants one or the other, and the two are
never interchangeable: a `Value` has no address, and a `Place` has no value until
it is loaded. Making the distinction explicit from the first day — when locals
and parameters were the only places the language had — is what kept `&`, `*` and
`p[i]` from being a rewrite, and it is what will keep arrays and `struct` from
being one: they are new producers of `Place`, not a new concept.

Today the table over node kinds has seven rows, and adding the next producer
(`p.f`, when aggregates land) is a row with a compile error if it is forgotten:

| Node | Evaluates to | How it is produced |
| --- | --- | --- |
| `PathExpr` naming a binding or a parameter | `Place` | its `alloca`; a parameter has one for the reason in § *Storage* |
| `PathExpr` naming anything else (`true`, `false`, `null`, a function) | `Value` | a constant, or the function itself — **not** a place, which is why `&null` never reaches this table with an address |
| `PrefixExpr` with `*` | `Place` | the operand *is* the address: `*p` produces `p`, and what lives there comes from the record |
| `IndexExpr` `p[i]` | `Place` | a `getelementptr` in the pointee's element type, **without `inbounds`** unless the tree recorded a proof |
| `PrefixExpr` with `&` | `Value` | a `Place` read as an address, and **no load**: `&x` is the `alloca` itself, which is why `&x` on a binding no path assigned is legal (`memory.md`, *Access*) |
| `ParenExpr` | either | whatever its operand is, which is why `(*p) = 1` and `&(x)` need no rule of their own |
| everything else | `Value` | computed, and never loadable |

Two rows are the whole reason the distinction was worth paying for on day one,
and both are decided by the record rather than by this table: **`*p` and `p[i]`
are the only nodes that perform a memory access**, so they are the only two that
ask `TypedFile::accessAt(node)` — and every other `Place` is reached without
asking, because the language already proved it is there. A `Place` is never
loaded implicitly: a consumer that wants a value asks for the load, and the load
takes its **alignment from the obligation**, not from the pointer's type nor from
LLVM's ABI default (§ *The assumption list*).

## Storage: `alloca` plus `mem2reg`

A local is an `alloca` in the entry block; a read is a `load`, an assignment a
`store`, and `mem2reg` turns them into SSA. That is the conventional choice and
it is worth saying *why* it is the right one here rather than the lazy one:
building SSA by hand means phi insertion, which means dominance frontiers, which
means the first place this project would have to implement an algorithm LLVM
already has — and a phi placed wrongly is a *silent* wrong answer, the failure
mode this document is organised against. `mem2reg` is the same algorithm, with
forty years of bug reports behind it, and it runs at `-O0`..`-O3` as part of the
pipelines below.

The cost is one pass and the fact that `-O0` output has an `alloca` per local,
which is also what every C compiler emits and what a debugger expects. The
`alloca`s go in the **entry block** and not where the declaration is, because a
conditional `alloca` grows the frame on every execution.

Three properties of the stack layout come from `memory.md` and not from LLVM's
conventions, and each is stated because getting it wrong is invisible until an
optimisation runs:

- **One `alloca` per binding, each its own object**, and the alignment is
  **stated explicitly**. LLVM's `alloca` takes an optional `align` and says that
  without one "the target can choose to align the allocation on any convenient
  boundary compatible with the type" — i.e. not necessarily the number `sema`'s
  `alignOf` promised. The `alloca`'s alignment is the type's, from the store.
- **A binding whose address is taken gets a `llvm.lifetime.start`/`end` pair**,
  so LLVM can color disjoint storage together (`memory.md`, *Lifetime*). In LLVM
  22 the intrinsics take only the pointer — `declare void @llvm.lifetime.start(ptr)`,
  with no size operand — and an `alloca` whose pointer is passed to
  `lifetime.start` is **initially dead** until that call executes, so the marker
  goes immediately after the `alloca` in the entry block and its `end` where the
  binding's scope ends.
- **The checked build does not emit the `end`.** The marker is what creates
  LLVM's dead-stack-object rule, and `memory.md` refuses that rule in both
  directions: once the object is dead a load may fold to poison, which would
  delete the load *and* the guard whose whole job is to report it. The checked
  build keeps the object live so the check stays observable; the release build
  takes the marker pair and the coloring.

**LLVM may still merge two bindings' storage**, and `memory.md` says that is safe
*because provenance is per allocation* — a pointer into the first binding keeps
its provenance after the second is placed there, so a later access is a violation
rather than an aliasing surprise. That is also the precise reason no `inbounds`
may be emitted from arithmetic that is only *probably* in range (§ *The
assumption list*): the pointer is dead-but-defined, and a promise it does not
hold turns it into poison instead.

## The runtime contract, as code

`sema.md`'s integer table is not advice; it is a specification of instructions
the lowering must not emit. `memory.md` adds a second specification of the same
shape, and the two are **not the same class of thing**, so this document keeps
them apart:

| Class | What it is | When it is emitted |
| --- | --- | --- |
| **Semantic guard** | the language *defines* the result — a trap — so a program without it would not mean what the language says | **every build**, at every optimisation level |
| **Checked-build guard** | the language calls the situation a *violation*; the guard is a diagnostic, not a definition | `-O0` and `-fcheck` only; the release build emits nothing |

Conflating the two is the expensive mistake in both directions: a division guard
left out of an `-O3` build changes the meaning of a correct program, and an
access guard kept in the release build makes every pointer operation pay for a
diagnostic nobody asked for. Class one is below, class two is
§ *The checked build's guards*, and that is the one allowed to disappear.

Three of `sema.md`'s rows are LLVM behaviours that must be *taken away* rather
than inherited:

| Language rule | What LLVM gives | What the lowering emits |
| --- | --- | --- |
| Signed/unsigned overflow **wraps** | `add nsw`/`nuw` promise it may not, which turns a defined wrap into a poison value | plain `add`/`sub`/`mul`/`shl`, **never** with `nsw` or `nuw` |
| `x / 0`, `x % 0` at runtime | `sdiv`/`udiv`/`srem`/`urem` by zero is not a defined result | a zero test and a branch to `llvm.trap` (or a `select`-free guarded block) |
| `INT_MIN / -1` | the quotient is not representable | an explicit test for `(MIN, -1)` and a trap |
| `INT_MIN % -1` | `srem` defines it as `0` | a bare `srem`, no guard |
| shift count negative or ≥ the width | `shl`/`lshr`/`ashr` with an out-of-range count is poison | an explicit range test and a trap |

Four functions, in `runtime.cc`, called by `expr.cc`:

```
Value checkedDiv(Value lhs, Value rhs, TypeId type, bool isRemainder);
Value checkedShift(Value value, Value count, TypeId opType, bool left);
```

and no call site is allowed to emit the bare instruction. This is a *rule about
the shape of the code*, which means it needs a mechanism rather than a comment,
and it has two:

- the operators that need a guard have no other implementation — `expr.cc`'s
  arithmetic table maps them to these functions, and the mapping is the only
  place the opcode appears;
- `invariants.cc` walks the finished module and reports `ir-unguarded-op` if it
  finds an `sdiv`/`udiv`/`srem`/`urem`/`shl`/`lshr`/`ashr` that did not come
  from one of those functions. In debug builds and in CI it is always on. It is
  the same idea as the `sema` tests that assert *one* diagnostic per mistake: an
  invariant that is not checked is a sentence in a document.

Note the asymmetry with `sema`, and that it is deliberate: division by a
**constant** zero is already `sema-division-by-zero`, an error, so the runtime
guard exists for the divisor the compiler cannot see. A lowering that emitted a
guard for `x / 0` as well would be emitting unreachable code and hiding a
diagnostic.

Traps kill the process, and the alternative — a compiler that is fast on the
paths that are correct and undefined on the others — is the one this project
turned down when it chose a language with no undefined behaviour. The runtime
*message* for a trap is a `runtime` module's business (`roadmap.md` § 11), not
the lowering's: the lowering emits the trap, and a future `-fsanitize`-style or
panic-handler mode replaces the callee behind it.

### The checked build's guards

`memory.md` states one obligation per access and then says, honestly, that
violating it puts the *access* outside the model rather than making the program
arbitrary. Those two facts decide this: a guard is not a semantics, it is the
compiler reporting a violation at its site, and it lives behind a flag.

Where each guard can come from is not a matter of taste — it is decided by the
**provenance the record holds**, which is the second job that enum does:

| Violation | Who can answer | What the check is |
| --- | --- | --- |
| `memory-null` | the module | the address against zero, for any non-zero access — the instruction already holds it |
| `memory-misaligned` | the module | the address against `alignOf(access.type)`; both numbers are in the instruction |
| `memory-out-of-object`, provenance `object` | the module | the base and the extent are the *tree's* (`&x`, `sizeOf`), and so is the reached offset |
| `memory-out-of-object`, provenance `foreign` | the shadow memory | the module has no base to compare against |
| `memory-uninitialized` | the shadow memory | a write map keyed by allocation |
| `memory-dangling` | the shadow memory | the same map, plus the `lifetime` interaction in § *Storage* |
| `memory-restrict-overlap` | the shadow memory | the annotation's meaning, checked at the call |

The first three are `runtime.cc`'s, emitted **from the obligation** — which is
the only way `expr.cc` may emit an access at all, so there is no path by which an
access reaches the module unguarded. The rest need state a module cannot carry,
which is why `memory.md` assigns them to the runtime; this stage's whole
contribution is to *not delete* the shadow calls and to keep the object live.

One asymmetry is deliberate: **the guard is emitted from the record, not proved
away by this stage.** An access whose provenance is `object` and whose index is a
constant in range is one the compiler *could* prove safe; the checked build
guards it anyway, and the release build drops every guard at once. A checker that
deletes the checks it can prove is a checker that stops reporting the day the
proof has a bug — and `sema` has already shown the alternative works, since its
static half (`check_flow.cc`) is a proof and this is not.

## The assumption list, and the scan that reads it

`memory.md` § *Not undefined* says the compiler "never hands the optimizer an
assumption it did not state," and that the list of assumptions it *is* allowed is
closed, lives in one file, and is read by a scan. This is that file and that
list, and it is short on purpose — every row is something this stage may
**never** emit, with the one exception named:

| Assumption | Status | Why not |
| --- | --- | --- |
| `!tbaa`, `!alias.scope`, `!noalias`, `!invariant.group`, `!nontemporal` | **never** | memory has no effective type; aliasing is untyped, and each of these is an inferred promise (`memory.md`, decisions 3 and 9) |
| `noalias` (the parameter attribute), `captures(...)` | **only from source** | a written `restrict` is the sole producer; today the syntax does not exist, so the module contains none. Never inferred. `captures(...)` waits for a proof scan over the body |
| `inbounds` (and the `nusw` it implies) | **only with a recorded proof** | today: never. See below — this is the row with a mechanism to design |
| `nsw`, `nuw` | **never** | the language defines wrap; class one of § *The runtime contract* |
| `dereferenceable`, `dereferenceable_or_null`, `!nonnull`, `!noundef`, `range`, `nnan`, `ninf` | **never** | each is a promise that something is well-formed; the language's rules are what make it so, and a promise on top of a proof is a promise that outlives the proof |
| `fast`, `reassoc`, `nnan`, `ninf`, `nsz`, `arcp`, `contract` on float ops | **never** | the language defines its float results; a fast-math flag licenses reassociation of a value the language named |
| `undef` and `poison` as values | **never** | `memory.md`, decision 13 — "uninitialized" is a violation, not a licence |
| `align N` on `load`/`store`/`alloca` | **always present, and scanned for equality** | not an assumption but a *claim*, and an overestimated one is UB (LLVM's own words); the scan compares every one against the record |

**The `inbounds` row is the one that needs a mechanism rather than a ban.**
`memory.md`, decision 10 says "no `inbounds` unless proved", and its § *How the
claims above are checked* says "no `inbounds` without a **recorded proof**". The
proof's home is therefore the typed tree, not this stage — the tree knows the
object (`&x`), its extent (`sizeOf`) and a constant index (`hasIntValue`), and
that is exactly the shape of a per-expression fact, like `opType`. Until that
fact is published, the scan's rule is literally **"no `inbounds` in the module"**,
which is today's honest state: with no aggregates and no arrays, the only
provable `getelementptr` would be a zero offset, and LLVM already says a
`getelementptr` with all-zero indices is inbounds by definition — so the promise
would buy nothing and is not worth a second implementation of the analysis.

Two mechanisms, and they fail differently:

- **the table above is the enumeration.** `invariants.cc` exposes
  `allModuleAssumptions()` in the shape of `sema`'s `allAccessKinds()`, and a
  test asserts the scan visits every row — so *adding* an assumption is a
  two-file change with a test, which is exactly the friction it should have;
- **the scan runs on every module**, in debug and CI always and as `--verify-ir`
  in release, because a rule about the shape of the emitted code that is not
  checked is a comment.

And the rule underneath both: **this list is the only place a new assumption may
be written down.** A future feature that wants one adds a row, an argument for
why the language states it, and the scan; it does not add an attribute.

## Signedness comes from the type

LLVM has no signed types. `i32` and `u32` are the same type, and the instruction
is what differs: `sdiv` vs `udiv`, `srem` vs `urem`, `ashr` vs `lshr`, and
`icmp slt` vs `icmp ult` (and `sgt`/`ugt`, and the `s`/`u` forms of `<=`/`>=`).
One table, in `expr.cc`, keyed by the operator and by **the signedness of the
operation type the tree named** — never by the opcode alone, and never by a
guess. The table is exhaustive over the operator tags with no `default:`, so a
new operator is a compile error.

Two consequences that are easy to get wrong and are stated here so they are not:

- **Comparisons produce `i1`.** `bool` *is* `i1`, so a comparison's result needs
  no conversion to be used as a condition, in a `&&`, or as a `bool` value.
- **The count of a shift is not converted to the left operand's type.** LLVM
  allows the two operands of `shl` to differ in width, and C's rule is that the
  count is promoted, not converted. The lowering passes what `sema` typed.

## Statements and blocks

Every block ends in a terminator; that is LLVM's requirement and it is also the
one place a structured tree has to be turned into something LLVM accepts:

| Statement | Blocks and terminators |
| --- | --- |
| `return e;` | evaluate, coerce to the return type, `ret`. The block ends here |
| block | the statements in order; no block of its own unless it is a loop body |
| `if c A else B` | one block for the test (`br i1`), one per arm, one join. The join is where the arms meet — with `alloca` storage, no phi is needed |
| `if c A` (no `else`) | the same, with an empty arm |
| `while c S` | test, body, exit; `continue` branches to the test, `break` to the exit |
| `for i; c; s S` | the initializer's block, then a `while` whose body is `S` followed by `s` |
| falling off the end | `ret` for a `void` function; otherwise unreachable, because `sema` refused it (`sema-missing-return`) |
| unreachable code | see below |

**Unreachable code after a `return`.** The checker warns
(`sema-unreachable-code`) and, with an outer `while true`, declines to analyse
what follows it. The lowering still has a block to terminate, so the rule is:
statements the checker marked unreachable are lowered into a block that is
emitted with an `unreachable` terminator and no predecessors, or skipped when
they produce nothing. Emitting them is the safer half of that choice — a
statement that is *dropped* leaves whatever it names unlowered, which is exactly
the kind of omission that does not show up until someone adds a
side effect — and LLVM deletes the block when it is not reached.

**One loop stack**, and a `break`/`continue` is a branch to the target at the
top of it. That stack is the third thing a lowering would otherwise duplicate
from the checker (which has its own, for `loopsForever`), and the project's rule
applies: it is the *same statement nesting* question, so the two are read from
one place where possible and kept to the same rule by a test that walks a nested
loop and asserts both agree about which loop a `break` belongs to.

## `str`, globals, and the ABI question

A `str` literal becomes **one private global per distinct spelling**, of type
`[N x i8]`, NUL-terminated, with the value being a `ptr` to it. Two identical
literals share a global; that is a module-level cache keyed by the interned
symbol, not a per-literal decision.

That sharing is now a **contract rather than an optimisation**, and the change is
worth naming: while `str` was the only indirect value and there was no `&`, two
identical literals being one object or two was *unobservable*. With `&x` in the
language the addresses are visible, so the deduplication is a promise the
lowering has to keep, and a test asserts the module holds one global for two
identical literals (`memory.md`, *Objects*).

The global is an object like any other, so it is one of the three things that
produce an object, it is `align 1` (its element type's alignment), and a global
with no initializer is zero — the `.bss` rule of `memory.md`, decision 12, which
today only the `str` globals exercise.

`str` is where the language met the ABI before it had a pointer type, and the day
`*` landed changed nothing here: LLVM's pointers are opaque, so `ptr` was already
a complete answer and no pointer *type* had to enter `.mx` for the IR to have one.
A `str` is a `ptr` to a private `[N x i8]`, `*u8` is the same `ptr` (`types.cc`
says so), and the difference between them is entirely in which access rules the
tree states about them.

**What is deliberately not designed here: aggregates and the C ABI.** Passing a
`struct` by value, returning one, variadics and `va_list` all need a calling
convention layer that knows the triple's ABI — Clang's `CGFunctionInfo` and
`ABIInfo` are a few thousand lines of exactly that, and Rust's decision to put
`Layout`/`FnAbi` in a backend-agnostic crate is the same lesson: the ABI is
computed *once*, above the backend, or every backend computes it differently.
That layer belongs to `src/cinterop` and gets its own record. What this document
decides is only that the *seam* is ready: `declarations.cc` builds a function's
LLVM type from `FunctionInfo`, so an ABI-aware mapper replaces one function
rather than restructuring the module.

## Debug information

Source locations are the one thing the IR carries that changes nothing about
behaviour and everything about whether the compiler's output can be debugged, so
they are built here rather than bolted on later: once instructions exist without
locations, adding them is a rewrite of every construction site.

- **`DIBuilder`**, one `DICompileUnit` per unit, one `DIFile` per source file, one
  `DISubprogram` per function, and a `DILocation` on every instruction that has a
  span. The line/column lookup is the same `support/line` index the diagnostics
  use, so a caret in a diagnostic and a breakpoint in `gdb` agree by
  construction instead of by two implementations of "which line is this byte".
- **Off unless asked.** No `-g` means no metadata is built at all, because
  metadata is not free: it is nodes in the module, a cost in every pass, and a
  cost in the object file. The flag is the driver's, and this stage reads it.
- **A debug-info verification failure does not fail the module.**
  `verifyModule` takes a `BrokenDebugInfo` out-parameter (`llvm/IR/Verifier.h`)
  precisely so a broken location scope can be reported separately, and the answer
  here is to *strip* the metadata and carry on rather than to refuse to compile a
  correct program because its line table is malformed. Losing a breakpoint is
  not a reason to lose the build.
- **Nothing here is platform-specific.** DWARF and CodeView are chosen by the
  triple (`DwarfDebug`/`CodeView`), so the same metadata is a DWARF section on
  Linux and macOS and a PDB-compatible record on Windows without `ir` knowing
  which it produced. That is the cross-platform argument for using the triple as
  the source of truth, applied to the one artifact that looks like a
  platform detail and is not.

## The verifier, and our own invariant pass

`verifyModule` runs on every module this stage produces. Two details are worth
writing down because both are classic traps:

- **It returns `true` on failure** (`llvm::verifyModule(M, &errs) != false`
  means the module is broken), which is the opposite of every other boolean in
  this codebase. It is wrapped once, in `invariants.cc`, so the polarity is
  stated in one place.
- **A failure is an ICE, not a diagnostic.** The verifier's output describes IR,
  which the user never wrote. It is reported as `ir-internal` with the module
  dumped beside it, and it means *this compiler is wrong*. That is the same
  posture `sema` takes with its poison: a bug in the compiler is not a mistake
  in the program, and saying otherwise teaches the reader to ignore
  diagnostics.

Additionally, always on in debug and CI and available as `--verify-ir` in
release: `invariants.cc`'s own scan for § *The runtime contract*. The verifier
proves the module is well-formed; the scan proves it is *ours*. Neither replaces
`mincc`'s own output check, which is that a module is never produced when there
is an error.

## Budgets

Every other stage bounds what untrusted input can make it allocate; this one
does the same, for the same reason. The AST bounds cap the *number of nodes* a
unit can have, so the lowering is bounded by construction — but a lowering can
turn one node into many instructions, and the number of blocks, globals and
debug records is not a function of the node count in an obvious way. So: the
module's counts are checked **before** the entry is created, exactly like
`kMaxDefsPerUnit`, and hitting one is a diagnostic (`ir-limit-*`) rather than an
allocation failure.

The depth guard is already paid for: the tree depth is bounded by the parser, and
the lowering recurses exactly as deep. It does not add a second recursion of its
own, and it does not recurse into LLVM structures.

## Failure: user errors, and internal errors

Two kinds, and they must not be confused:

- **`ir-unsupported-*`** — a construct the language has that this stage does not
  lower *yet*. It is a user-visible diagnostic with a code, a span and a
  sentence, it is listed with a test input in the reachability table (every code
  in every stage here has one), and it exits non-zero without emitting. Today it
  is how `Pointer`/`Array` types and the reserved node kinds behave, and it is
  what a future `switch` would do on the day its syntax lands and its lowering
  does not.
- **`ir-internal`** — a violated precondition or a failed verification. A bug in
  this compiler. It is reported with the node and the tree dumped, and it is
  what the coverage tests exist to make unreachable from any program the grammar
  accepts.

The distinction matters because they have opposite fixes: one wants a feature,
the other wants a bug report, and a compiler that reports both the same way
trains its users to mistrust both.

## Cross-platform: finding and linking LLVM

The root `CMakeLists.txt` already detects LLVM without requiring it, and states
the rule that nothing outside the backend may include `llvm/*`. Making the
dependency real is a bounded piece of work with three decisions in it:

- **How it is found.** `find_package(LLVM CONFIG REQUIRED)` and the imported
  targets (`LLVM::Core`, `LLVM::IRReader`, …) rather than
  `llvm_map_components_to_libnames` plus manual include paths: the exported
  targets carry the definitions that have to match the library, and § below is
  what happens when they do not.
- **What has to match.** LLVM's *build configuration* is part of its ABI, and
  three settings bite: `LLVM_ENABLE_ABI_BREAKING_CHECKS` (a mismatch is a link
  error, or worse, silent disagreement about struct layout),
  `LLVM_ENABLE_RTTI` (a mismatch is an ODR problem across the boundary) and
  `LLVM_ENABLE_EH`. This machine's 22.1.8 has assertions and ABI-breaking checks
  **off**, RTTI **on**, exceptions **off** — a configuration this project is
  compatible with as it stands (it uses neither `-fno-rtti` nor `-fno-throws`).
  The check is not "trust the packager": the CMake module asserts the three
  variables agree with what the compiler is being built with and fails with a
  sentence naming the mismatch.
- **Which version.** The front end has no LLVM dependency, so an older or newer
  LLVM is a *backend* question — but a silent one, because the IR API moves
  (opaque pointers removed an entire class of overloads). A minimum version is
  checked (`MINC_LLVM_MIN_VERSION`), the tested one is recorded here (22.1.8),
  and a CI job builds against a second version so the drift is caught in CI
  rather than by a user.
- **Static or shared.** `libLLVM-22` as a shared library is what this machine
  has and what a distribution package gives; a static LLVM makes a
  self-contained `mincc` at the cost of a much larger link. Both are supported
  by finding it through the CMake package; the choice goes in a `MINC_LLVM_LINK`
  option, and neither changes a line of `src/`.

macOS and Windows notes, since "cross-platform" is a claim this project has to
keep: LLVM is not on the default search path for either (Homebrew's `llvm`
keg, `LLVM_ROOT` on Windows), `llvm-config` is not the mechanism to use, and
MSVC's `/utf-8` (already set project-wide) applies to LLVM's headers too.

## The isolation rule, as a test

`architecture.md` says `src/support` is LLVM-free by contract and only the
backend may include `llvm/*`. With `src/ir` in the picture that sentence needs
one amendment — **the rule becomes "nothing up to and including `sema`", which
is a property of the pipeline's order rather than of one directory** — and it
needs a mechanism, because a rule that only exists in prose is a rule that is
true until the first convenient inclusion.

The mechanism is a test, and it is cheap: a unit test that walks the source tree
and fails if `#include <llvm/` or `#include "llvm/` appears outside `src/ir`,
`src/backend` and `tests/`. It runs in CI with the rest, it has no dependencies,
and it means a front-end module that reaches for LLVM fails in the job before
anyone has to notice it in review. This is the same *checkable rule* shape as
`sema`'s "one input per diagnostic code" table: an architectural claim, turned
into something that fails.

## `mincc run`: ORC

The executable path is `build` → object → link. The *fast* path, and the one
that finally gives this project something it has never had — an **oracle** — is
`run` over the ORC JIT, and it is worth designing now because it changes what
"tested" means for every later feature:

```
auto jit = llvm::orc::LLJITBuilder().create();     // host triple, in-process
jit->addIRModule(llvm::orc::ThreadSafeModule(std::move(module), ctx));
auto sym = jit->lookup("main");
int result = sym->toPtr<int (*)()>()();
```

- **`LLJIT`**, not `MCJIT`, and not `LLLazyJIT` yet: eager compilation on lookup
  is what a test harness wants, and laziness is an optimisation for a REPL.
- **The process's symbols are visible** through
  `DynamicLibrarySearchGenerator::GetForCurrentProcess`, which is what lets
  JIT'd code call `libc` — the mechanism that makes `printf` possible before
  `src/cinterop` exists.
- **`LLVMContext` ownership transfers into the `ThreadSafeModule`.** After
  `addIRModule` the module belongs to the JIT, and the unit's context with it.
  A `run` that keeps using either is a use-after-move waiting for a bad day.
- **`run` is the host triple only.** Cross-compiling is `codegen`'s job with a
  `TargetMachine`; a JIT that claims to run a Windows object on Linux is a lie.
- **What it is for.** The end-to-end test shape this project has been missing:
  compile the program, execute it, and compare the value against a *reference* —
  the same source translated to C and compiled by the system compiler, for the
  subset where both languages mean the same thing. That is a differential oracle,
  and it is how the runtime contract in § *The runtime contract* stops being an
  assertion about IR text and becomes a statement about arithmetic that either
  holds or does not.

## Optimization levels and the pass pipeline

`codegen` maps `-O0`..`-O3` (and `-Os`) onto LLVM's own pipelines through
`PassBuilder`, and `ir` does not run passes of its own. Two things follow, and
one of them is a safety property worth naming:

- **We never hand-roll an optimisation.** Every pass this project could write is
  a pass LLVM has, with a test suite behind it.
- **The optimiser cannot invent the undefined behaviour we refused, because it
  is not in the IR we hand it.** This is the real payoff of "no `nsw`/`nuw`" and
  of the trap guards: LLVM's optimisations are licensed by the semantics the
  module *states*, so a module that states wrap and states a trap is a module
  whose optimisations preserve the language's meaning. The contract in
  § *The runtime contract* is what makes the `-O3` build of a program mean the
  same as the `-O0` build.

LTO and `-flto` need the bitcode path (`EmitBC`) and are a later decision, not
a consequence of this one.

## How this stays correct when the language grows

This is the section the rest of the document exists to support. The question is
not "does the first version work" — it is "when methods on types, builtins,
arrays, structs and `switch` arrive one at a time over the next year, what stops
the IR from being quietly wrong". Six answers, and every one of them is
*mechanical* rather than a matter of care:

1. **A new node kind cannot be ignored.** The lowering switches over node kinds
   with no `default:`, and `-Wswitch` under `-Werror` turns a missing case into a
   build failure. Adding `SwitchStmt` to the grammar breaks `stmt.cc` on the
   next build, with a line number, before anyone can forget it.
2. **A new type kind cannot be ignored.** The same for `TypeKind` in `types.cc`.
   `Pointer` has landed and is a body (`ptr`); `Array` is still a reserved kind
   whose case exists today and produces `ir-unsupported-type`, so the day its
   syntax lands the change is a *body*, not a `case`.
3. **Coverage tests over the enumerations.** `parse::allNodeKinds()` exists for
   exactly this purpose (a kind added to the enum without a table row is caught
   by a test), and the same shape applies here: a test that walks every node kind
   the grammar can produce and asserts it is either lowered or refused *by name*.
   `-Wswitch` catches the ones the grammar can produce; the enumeration catches
   the ones the switch cannot see. Both exist because they fail differently.
4. **A new operator cannot pick a wrong opcode.** The operator table is
   exhaustive and keyed by type signedness, and the conversion table is total
   over the pairs the language permits. § *The coercion record* is what makes the
   second half true without a second implementation of the conversion rules.
5. **A new callee kind changes exactly one place.** `CallExpr` dispatch is where
   builtins, methods (an implicit receiver is a prepended argument), `extern` and
   a `str` library call all arrive. They add an entry to one table; they do not
   reach into expression lowering, and an entry that is missing is a refusal by
   name rather than a wrong call.
6. **A new memory rule cannot be informal.** An access's alignment, kind and
   provenance come from one record with one reader (§ *The access record*), and
   the assumptions this stage may hand the optimiser are a closed, enumerated,
   scanned list (§ *The assumption list*). A memory rule that is not in one of
   those two places is a rule this compiler does not implement — which is the
   only state in which a rule can be *added* safely, because adding it is then a
   row plus a test rather than an attribute nobody audits.

And the one rule underneath all five, which is the project's oldest invariant
applied to a new stage:

> **The lowering never decides a language question.** It materialises decisions
> `sema` recorded, and when a decision is missing it refuses. A decision that
> *cannot* be recorded — because it depends on context no pair of types carries —
> is a bug in the typed tree, not a puzzle for the lowering.

What is *not* prepared, and is named so it is not mistaken for an oversight:
aggregate layout and ABI (§ *`str`, globals, and the ABI question*), unwinding
and destructors, `goto` and its block structure, exception-like mechanisms, and
anything that needs a value *not* derived from the typed tree.

## Tests

The ladder, in the order it should be built:

1. **The verifier always.** Every module a test produces is verified. It costs
   microseconds and it is the difference between a wrong module and a compiler
   bug report.
2. **Structural assertions on the IR text** where the property *is* textual: the
   module contains no `nsw`/`nuw` anywhere; every division is preceded by its
   guard; no `inbounds`, no `!tbaa`, no `noalias` and no `undef`/`poison`
   anywhere; every `load`/`store`/`alloca` carries an explicit `align`; two
   identical `str` literals compile to one global. These are the sentences of
   § *The runtime contract* and § *The assumption list* turned into string checks
   over `Module::print`. They are brittle about *shape* and that is acceptable,
   because what they assert is a shape.
3. **The coverage tests** of § *How this stays correct*: every node kind, every
   type kind, every operator tag, every conversion pair the language permits.
   These are the tests that make a future addition fail loudly instead of
   silently.
4. **The differential oracle**, which is the one this project has been missing
   since the lexer: execute the program (JIT), execute the same program written
   in C compiled by the system compiler, and compare. It covers exactly the
   subset both languages mean the same way, which is most of the arithmetic, all
   of the control flow and every edge case in `sema.md`'s integer table. Without
   it, every rule in this compiler is only self-consistent.
5. **The refusal tests.** An `Array` type, a reserved node kind, a poisoned
   tree, an access node with **no recorded obligation**: each produces its named
   diagnostic and **no module**. "No module" is part of the assertion, not an
   implementation detail — it is the property that stops a half-built module
   from reaching a linker.
6. **The target table against LLVM's own.** `sema`'s ABI table is stated by rule
   from the triple's components and cannot link LLVM to check itself; this stage
   can. One test walks every stated triple, builds an `llvm::Triple` and a
   `DataLayout` from it, and asserts the pointer size agrees with
   `TargetInfo::pointerBits`,   that `long double`'s width agrees with the data
   layout's float layout, and that the component names this stage parsed are the
   ones LLVM parses. It is the one place the front end's target model can be
   wrong without any test here noticing, so it is worth the dependency. It is
   extended to `sizeof`/`alignof` for every type in the store, because the
   pointer width is what `isize`, `usize` and every pointer access depend on.
7. **The access record's enumeration test**, in the shape of `coerce_test.cc`:
   one program per access kind and per provenance kind, an assertion that the
   emitted shape is the recorded one, that `accessAt` answers `nullptr` for a
   node that denotes no access, and that a `Place` denoting a binding is reached
   without a record at all. Plus the property tests `memory.md` names: `p + n -
   n == p` inside an object, `p[i] == *(p + i)` for every type in the store, and
   `p1 - p2` consistency.
8. **The scan's own tests**: a hand-built module that contains one banned
   assumption, or a `load` whose `align` disagrees with the record, makes the
   scan fail — and the enumeration test makes it fail if a *row* is added without
   a check. A scan nothing can trip is a scan that stopped running.
9. **The checked build's traps**, one per row of § *The checked build's guards*,
   at `-O0` **and** with `-fcheck` at `-O2` — because a check the optimiser can
   delete is not a check — plus the converse assertion that the release build's
   module contains none of them.

What is deliberately *not* a test: **golden IR files**. IR here is an internal
format whose text changes when LLVM changes, a target changes or a comment
changes, and a golden file that is regenerated whenever it fails is a test that
has stopped testing. The property worth freezing is behaviour, and 4 is where it
is frozen. This is the one place this project departs from "one input per code
table" and it is because the artifact is not a stable interface in the way the
other stages' artifacts are.

## Decisions

| # | Decision | Why |
| --- | --- | --- |
| 1 | The IR is LLVM's; `src/ir` builds a `Module` | Two pieces (an IR and a backend) written by hand before a program can run, neither of them better than LLVM's |
| 2 | The lowering runs only on a tree with zero errors, and it checks | The poison type converts silently; a poisoned tree lowers to well-formed IR for a program nobody decided |
| 3 | A conversion is a *recorded* fact, not a re-derivable one | A shared `promote()` still leaves arithmetic-conversion logic in the lowering, and context-dependent typing cannot be recovered from a pair of types at all |
| 4 | A compound assignment records its **operation type** | `x <<= 9` on a `u16` is defined at `i32`; the tree names only `u16`, and reading it wrong is an out-of-range shift — a poison value, not a crash |
| 5 | `bool` is `i1`; a `bool` *object* would be `i8` with a normalising store | `i1` is what `icmp` produces, and it is not a byte; the memory representation is decided before the first pointer needs it |
| 6 | Locals are `alloca` in the entry block, with `mem2reg` | Hand-built SSA means phi insertion and dominance frontiers, and a misplaced phi is a silent wrong answer |
| 7 | `Value` and `Place` are distinct types from the first day | Assignment, `&`, `*`, arrays and aggregates are new *producers* of `Place`, not a new concept |
| 8 | No `nsw`/`nuw`, ever, and the module is scanned for them | The language defines wrap; the flag would trade a defined result for a poison value, and the scan is what makes "ever" true |
| 9 | Division, remainder and shift guards are four functions plus a module scan | A rule about the shape of the code needs a mechanism, or it is a comment |
| 10 | `verifyModule` on every module, always, and its failure is an ICE | It returns `true` on *failure* (stated once, wrapped once), and a verifier message describes IR the user never wrote |
| 11 | The target machine belongs to `codegen`, not `ir` | `ir` needs a triple and a data layout; the expensive, target-specific half is not the lowering's, and `--emit=ir` works without it |
| 12 | One `LLVMContext` per unit, per worker; no globals | An `LLVMContext` is not thread-safe and owns the types; sharing one across an editor's concurrent units is a race, not a convenience |
| 13 | `src/ir` may include `llvm/*`; nothing up to and including `sema` may | The pipeline's order is the boundary; a test greps the tree, so the rule fails in CI rather than in review |
| 14 | The LLVM build configuration is asserted, not assumed | ABI-breaking checks, RTTI and EH are part of LLVM's ABI; a mismatch is a link error or a silent struct-layout disagreement |
| 15 | `run` is `LLJIT` on the host triple, and it is the oracle | Eager, in-process, `libc` visible; cross-compiling is `codegen`'s job, and executing a program is the only thing that can prove the integer table |
| 16 | No golden IR files; behaviour is the contract | IR text changes with LLVM, the target and comments; a regenerated golden file has stopped testing |
| 17 | An unsupported construct is a *named refusal*, never a guess | `ir-unsupported-*` is a feature not yet built; `ir-internal` is a bug in this compiler — opposite fixes, so never the same message |
| 18 | Debug metadata is built only under `-g`, and a broken line table is stripped rather than fatal | Metadata is a cost in the module and in every pass; and refusing to compile a correct program because its scope chain is malformed trades a breakpoint for a build |
| 19 | No deferred literal type survives `sema`; a sweep decides whatever a seam did not | A deferred type has no width, so it has no LLVM type at all. A sweep rather than a per-seam promise, so a path nobody has written yet cannot break the property |
| 20 | A constant is materialised at its type's width, and the language defines that width's arithmetic to wrap | Truncating a folded constant to its type is the defined semantics, not a lossy shortcut; it is what makes `ConstantInt::get` safe to call with the operand's stored value |
| 21 | The target's identity is the **canonical LLVM triple**, stated by `sema` | It is the string a `TargetMachine` is built from, so a private name for it would be a second spelling to translate and a place for the two to disagree; and the ABI facts derive from its components **by rule**, with a triple the table does not state refused rather than defaulted |
| 22 | **The access record is the fourth fact the lowering may not re-derive** — alignment, kind, accessed type, provenance — read from `TypedFile::accessAt(node)` | The memory half of decision 3. An alignment re-derived here is a second copy of `sema`'s layout table, and an overestimated `align` is not slow code, it is UB (LLVM's own wording) |
| 23 | **A pointer is `ptr`; the pointee is not in the LLVM type** — it survives only as the element type of a `getelementptr`, the index type, and the record's accessed type | LLVM 22's pointers are opaque, so the pointee has to live *somewhere*; putting it in the record is what keeps `types.cc` from becoming a second type system |
| 24 | **`*p` and `p[i]` are the only nodes that consult the record**, and the `Place` producer table is exhaustive with no `default:` | They are the only two nodes that perform an access; a `Place` from a binding needs no permission because the language already proved it, and a new producer is a build error rather than a silent gap |
| 25 | **Every `load`/`store`/`alloca` states its alignment explicitly**, from the record, and the overloads that omit it are banned | In-memory IR always carries one; omitted means "the ABI alignment for the target", and overestimated means UB. "Underestimating may produce less efficient code" is the safe direction, and it must be a decision |
| 26 | **Two classes of guard: semantic and checked-build** | A guard the language *defines* (a trap) belongs in every build; a guard that *reports a violation* belongs to `-fcheck` (`-O0`). Conflating them either changes the meaning of an `-O3` program or taxes every release pointer operation |
| 27 | **`ProvenanceKind` decides which half of the checked build can answer** | The module can check a null, an alignment and an `object`-provenance extent; the rest need a shadow memory. That is why `foreign` is not merely "the conservative answer" — it is the answer that moves the check into the runtime |
| 28 | **No `inbounds` without a recorded proof**; today the scan's rule is "no `inbounds` at all" | The analogue of no-`nsw`. With no aggregates the only provable offset is zero, and LLVM already treats an all-zero-index `getelementptr` as inbounds — so the promise buys nothing and is not worth a second copy of the analysis |
| 29 | **The assumption list is closed, enumerated in one file, and scanned**, and adding a row is a two-file change with a test | "No undefined behavior" is a slogan; a closed scanned list is a guarantee that fails in CI, and the friction is the point |
| 30 | **`llvm.lifetime.start`/`end` for address-taken bindings; the checked build omits the `end`** | The markers let LLVM color disjoint storage; the marker is also what creates LLVM's dead-stack-load rule, which `memory.md` refuses, so the build whose job is to *report* the violation must keep the object live |
| 31 | **`str` literal deduplication is a contract, not an optimisation** | `&x` makes two literals' addresses observable, so "one object or two" stopped being invisible. Stated now, because the day it matters is the day it is already wrong |
| 32 | **A missing obligation is an ICE** (`ir-missing-obligation`), not a statement about the program | A program that type-checked cannot be missing one, so the only reading is "this compiler is wrong" — the same posture as a verifier failure and `sema`'s poison |

## Non-goals

- **An IR of our own**, and with it a hand-written backend. Closed in
  `roadmap.md` § 6–7.
- **A second representation of the typed tree.** The IR is built straight from
  the lowered AST plus `sema`'s parallel arrays, and a third tree would be a
  third thing to keep in step.
- **A middle end.** There is no new IR between the typed tree and LLVM's:
  `alloca` + `mem2reg` is where a middle end would go, and LLVM's is the one that
  is there.
- **Aggregates, variadics and the C calling convention**, which belong to
  `src/cinterop` with their own record (§ *`str`, globals, and the ABI
  question*).
- **The shadow-memory half of the checked build.** This stage emits the guards a
  *module* can state (null, alignment, an `object`-provenance extent) and keeps
  the object live so the rest stays observable; the write map, the liveness map
  and the `restrict` overlap check are the runtime's, and `roadmap.md` § 11 owns
  them.
- **Unwinding, destructors and `goto`.** Each needs a block structure this
  structured lowering does not have, and each arrives with its own design.
- **A JIT for cross targets**, and a REPL, which is a `driver` question that
  this stage makes *possible* by producing a module.
- **Debug-info fidelity beyond line-accurate locations** — variable location
  ranges and inlining scopes are LLVM's to improve once the metadata exists.
