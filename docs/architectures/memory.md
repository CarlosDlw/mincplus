# The memory model

**Status:** settled, and **stage one implemented**. The surface below (`*T`,
`&x`, `*p`, `p[i]`, the stepping, the comparison, `null`, `*void`) type-checks,
and the per-access record `sema` publishes (`src/sema/access.cc`) is what the
lowering will materialise instead of re-deriving. Still **blocking `src/ir`** for
the rest: nothing that takes the address of an object may be *lowered* until the
emitted shape obeys the assumptions list at the end of this document, because
every question below is a question the *optimizer* asks. A wrong answer there is
not a diagnostic; it is a program that runs differently at `-O2` than at `-O0`.

This is the seventh design record, and it sits *under* the two next to it.
[`sema.md`](sema.md) decides what arithmetic means, this decides what **memory**
means, and [`ir.md`](ir.md) decides how both are emitted. Where they meet, this
document wins, because the other two were written expecting it: `ir.md` asked for
the record the lowering may not re-derive, and for a "scan that enforces the
closed list of assumptions". This file is that list, and that scan is now the
section `ir.md` calls *The assumption list* — which is also where
`TypeKind::Pointer` stopped being a kind the lowering refuses by name and became
one it maps.

## The model in one page

If you read nothing else:

1. **A pointer is an address plus a provenance.** Provenance is the permission to
   access a particular allocation, for a particular time. It is not the address.
2. **An object is an allocation**, identified by a base and a byte size. There is
   no "effective type" of memory: storing an `i32` and loading an `f32` from the
   same bytes is defined.
3. **An access is `[p, p + size)` must lie inside a live object that `p`'s
   provenance covers.** That single sentence is the whole access rule.
4. **Aliasing is untyped unless the source says otherwise.** No type-based alias
   analysis, ever. The only `noalias` in the module comes from an annotation the
   programmer wrote.
5. **A pointer is not an integer.** `expose` (`ptr → int`) and
   `with_exposed_provenance` (`int → ptr`) are *named operations*, and they are
   the only place provenance is lost or regained.
6. **No `inbounds` unless the compiler proved it**, exactly as there is no `nsw`
   on arithmetic the language defines to wrap.
7. **An access must satisfy the alignment its type states.** An unaligned access
   is a different, spelled operation.
8. **Reading unwritten memory, out-of-object access, and use after an object's
   lifetime are contract violations, not undefined behavior.** The difference is
   the subject of § *What minc+ guarantees, and what it does not* — and it is
   smaller than the slogan "no undefined behavior" makes it sound, so it is
   stated precisely rather than sold.
9. **The language never hands the optimizer an assumption it did not state.** The
   list of assumptions is closed, lives in one file, and a scan reads that file —
   so a new assumption is a code change plus a test, not a comment.
10. **The C ABI is the floor; the semantics are ours.** Layout, alignment,
    calling convention and object sizes are byte-identical to C. Provenance,
    aliasing, lifetime and what counts as a violation are the language's own, and
    none of them crosses the FFI boundary in either direction.

## The one idea: three ways to discharge one obligation

Every memory access in this language carries the same obligation, stated in
point 3 above. The three "layers" people ask about — safe code, raw pointers,
FFI — are **not three models**. They are three *ways of discharging the same
obligation*, and they differ only in who is responsible:

| Layer | Who discharges the obligation | What the compiler knows |
| --- | --- | --- |
| Checked surface (planned) | the **compiler**, by construction: a pointer carries the length and the permission in its type | everything, and no assumption reaches the optimizer |
| Raw surface (`*T`, first to land) | the **programmer**, at each `*` | nothing, so it assumes nothing |
| FFI (`src/cinterop`, reached through `extern fn`) | the **ABI**: C's own rules apply at the boundary and stop there | the ABI's facts — layout, alignment, calling convention — and only those |

The `extern` *declaration form* is the language's, not the boundary's: it says
"this function is defined elsewhere" and nothing about types crossing a
boundary. The ABI facts are what the layer below adds, and the two are kept apart
in [`extern.md`](extern.md) for that reason.

This is why the model is written before the surface: it holds whatever the
surface turns out to be, and it is the reason a checked layer can be added later
without reopening a single rule below.

## What the references do, and where they disagree

### C — the model is a Technical Specification, not the standard

ISO/IEC 9899:2024 (C23) contains **no normative pointer-provenance model**. On
2025-05 WG14 published **ISO/IEC TS 6010, *A provenance-aware memory object
model for C*** (draft N3231), and the wording is still being argued in the C2y
working draft (N3886). So in this area "what C means" has been decided by
**implementations** since about 2013 — GCC 4.6.4 and clang 3.4.1 both implement
provenance — while the text said a pointer is an integer. A language that wants
the question settled cannot take C's answer by citation, because there is no
citation to take.

Four specific C decisions this language refuses:

- **Effective type** (6.5p6–p7): the object's type is retroactively the type of
  its last store. It makes punning UB *by accident* and is the reason `memcpy` is
  the blessed way to reinterpret bytes.
- **Strict aliasing / TBAA**: an optimization license derived from the type
  system, which LLVM will tell you is not even valid for its own unadorned IR
  (§ *LLVM* below).
- **Relational comparison of pointers to different objects** (6.5.8p5) is
  undefined. Two addresses are two numbers; ordering them is defined here.
- **`restrict`**: an unchecked promise with famously hard wording. Here it is
  checked where the language can check it and is never inferred.

### Rust — the closest design, and the one with the honest gaps

Rust is the only widely-used systems language that has attempted to *specify*
this, and the most useful thing about its documentation is what it admits. From
`std::ptr` itself: *"The precise rules for validity are not determined yet"*, and
the aliasing rules are *"not decided yet"*. The shape is settled; the text is
not. That is the state of the art, and it is what this section borrows from.

What we take, because it is right and because it is also what LLVM already
enforces:

- **The allocation axioms.** An allocation has a base address, a size and a set
  of addresses. The base is non-null; `size <= isize::MAX`; the base plus the
  size never wraps the address space; `a - base` never overflows an `isize`.
- **Provenance has spatial, temporal and mutability components**, and
  **no operation ever grows a pointer's permissions** — derived pointer
  arithmetic, casts and subobject projection may shrink them, never enlarge them,
  and two adjacent provenances cannot be recombined.
- **Every stack variable is its own allocation.** This is why
  `&x` arithmetic can never reach `y`, and it is the rule our stack layout must
  keep true.
- **Validity is per access, not per pointer.** "Is this pointer valid" is not a
  question; "is it valid for a read of 8 bytes" is.
- **Comparison ignores provenance.** Equality and ordering read the address, so
  they are always defined, even on dangling pointers.
- **Strict versus exposed provenance** (RFC 3559): `with_addr`/`without_provenance`
  (which keep or invent provenance without touching the address) are separate from
  `expose_provenance`/`with_exposed_provenance` (which go through an integer and
  therefore give up the fine-grained answer). Our § *Provenance* is this split,
  with the names it earns in a language that has no borrow checker.

What we do not take: the borrow checker. It is not a memory-model rule; it is a
*discharge mechanism* for the checked layer, and it is the largest single feature
a language can commit to. The model below is deliberately independent of it.

### LLVM — the floor we cannot argue with

The IR is LLVM's (`ir.md`, decision 1), so LLVM's rules are not a design choice;
they are the surface the language has to be expressed on. Quotes are from the
LangRef of the LLVM this project builds against (22.x), read on the machine, not
recalled:

- *"LLVM IR does not associate types with memory. The result type of a `load`
  merely indicates the size and alignment of the memory from which to load, as
  well as the interpretation of the value. Consequently, type-based alias
  analysis, aka TBAA, aka `-fstrict-aliasing`, is not applicable to general
  unadorned LLVM IR."* — **The untyped-memory decision is not radical; it is what
  LLVM already is.** Clang adds TBAA as *metadata*; emitting none is the absence
  of an assumption, not a lost optimization opportunity.
- *"Any memory access must be done through a pointer value associated with an
  address range of the memory access, otherwise the behavior is undefined."* —
  and "based on" is transitive, with `inttoptr` *"based on all pointer values that
  contribute (directly or indirectly) to the computation of the pointer's
  value."* This is the access rule, and it is LLVM's, not ours.
- *"The result value of the `getelementptr` may be outside the object pointed to
  by the base pointer. The result value may not necessarily be used to access
  memory though, even if it happens to point into allocated storage."* — pointer
  arithmetic is defined out of object; **the access** is not.
- *"It is undefined behavior to access an allocated object that isn't alive, but
  operations that don't dereference it such as `getelementptr`, `ptrtoint` and
  `icmp` return a valid result."* — and, for stack objects specifically:
  *"loading from a stack object outside its lifetime is not undefined behavior and
  returns a poison value instead. Storing to it is still undefined behavior."*
  That exception is exactly the kind of inherited answer this document refuses
  (§ *Lifetime*).
- *"`no allocated object may cross the unsigned address space boundary`"* and
  *"the size of all allocated objects must be non-negative and not exceed the
  largest signed integer that fits into the index type."* — a **contract on the
  allocator**, and a real one on 32-bit targets (§ *Cross-platform*).
- `noalias`: *"memory locations accessed via pointer values based on the argument
  or return value are not also accessed, during the execution of the function, via
  pointer values not based on the argument or return value"*, and *"this
  definition of `noalias` is intentionally similar to the definition of `restrict`
  in C99."* Note that it is a **write-since** guarantee, not a blanket one.
- LLVM 22 also has explicit **capture** semantics (`captures(address)`,
  `captures(address, provenance)`, `captures(none)`), `ptrtoint` *"always captures
  address and provenance"*, and `ptrtoaddr` *"always captures the address but not
  the provenance"*. That last distinction is new and directly useful: an
  address-extraction that must not disturb provenance is `ptrtoaddr`, and the
  operation that gives up provenance is `ptrtoint`. Our two named operations map
  onto exactly these two, one-to-one.

Three consequences for this language, stated once:

1. **We cannot choose to not have provenance.** Every `getelementptr`, even
   without `inbounds`, is "based on" its base pointer. The choice is *specify it*
   or *inherit it silently*, and the second is what C did.
2. **The parts of the model that are forced are short.** Access-within-a-live-
   object, based-on, no-object-crossing-the-address-space-boundary. Everything
   else is ours to decide, and this document is mostly those decisions.
3. **The parts that are *not* forced are where a language can be better.** LLVM
   says nothing about how a language spells alignment, initialization, `volatile`
   or the difference between "violation" and "undefined". Those are the rows
   below with a reasoning column that is longer than the rule.

### Targets with capabilities — the one thing `int ↔ ptr` cannot do

LLVM models CHERI and similar targets with **non-integral pointers**, and the
LangRef is explicit: *"the `inttoptr` instruction does not recreate the external
state and therefore it is target dependent whether it can be used to create a
dereferenceable pointer"*; on CHERI, `inttoptr` yields a capability whose validity
tag is zero, so *any dereference traps*. So a language whose model *requires*
round-tripping through an integer is a language that cannot target capability
hardware, and this is decided now rather than discovered later.

Our answer: `expose`/`with_exposed_provenance` are **named, counted, and
documented as not portable to capability targets**, and the checked build is what
finds a use that would trap there. The rest of the model — objects, provenance,
typed-free aliasing, alignment, lifetime — is *more* natural on hardware that
enforces provenance, not less.

### What the checked languages do

One paragraph of calibration, because the "why not just check everything"
question is fair:

- **Go and Swift** check most accesses and pay for it, with a runtime that owns
  lifetime. They can, because they own the allocator and the object graph.
- **C# and Java** do the same behind a JIT and a GC.
- **Ada** checks by default and lets the checks be turned off.
- **D** has `@safe`, a function-level attribute, with `@trusted` as the escape.
- **Zig** deliberately does not: out-of-bounds is undefined in a release build,
  and its answer to integer UB (checked operators, wrapping spelled) is the same
  *style* of answer this project already chose for arithmetic in `sema.md`.
- **Clang's bounds-safety attributes** (`__counted_by`, `__sized_by`) move the
  length into the type, which is the same idea as the checked layer's slice — in
  C, retrofitted onto a language that had already given the length away.

The pattern: a language with its own allocator and GC can check by default; a
language that must interoperate with C's memory cannot, and therefore has to
decide **where** the checks are. That is the whole of § *Cost*.

## The model

### Objects

An **object** is a region of memory produced by an allocation, with three
properties:

- a **base address** (never null),
- a **size** in bytes, `size <= isize::MAX` on the target, and
- a **type** and an **alignment**, both fixed at allocation.

Objects are produced by exactly three things, and the list is closed:

1. a **local binding** (`alloca`) — each binding is its own object, so no pointer
   arithmetic can walk from one local to the next, even when the stack layout
   puts them adjacent;
2. a **global** (a file-scope `let`/`const` -- `globals.md` -- and the private
   `[N x i8]` global behind a `str` literal);
3. an **allocator call** — `alloc`, declared in the runtime's own header with the
   object obligations of this section as its contract.

**No object may cross the unsigned address-space boundary**, and no object may be
larger than `isize::MAX`; both are LLVM's own requirements for an allocated
object, and on a 32-bit triple the first one is reachable by real code
(§ *Cross-platform*). This is the allocator's obligation, and it is stated in the
allocator's signature rather than in prose.

**Memory has no effective type.** The bytes of an object may be written as one
type and read as another, and the language defines that. This is not a permissive
choice: LLVM does not associate types with memory, so a language that *claimed*
punning was illegal would be claiming something the IR cannot express — and every
optimizer would then be free to ignore the claim. The type of an access is a
property of the access, and the only rules that follow from a type are **size and
alignment**.

There is one arithmetic relationship between objects and it is negative:
reaching a second object through pointer arithmetic on the first is a contract
violation, *even when the two are adjacent in address space*. The reason is in
LLVM's own rule above ("may not necessarily be used to access memory ... even if
it happens to point into allocated storage"), and the practical reason is that
otherwise separate stack variables and separate heap blocks would alias, and the
compiler could assume nothing about anything.

### Provenance

A pointer value carries two things, and they are not interchangeable:

```
ptr = (address, provenance)
```

**Provenance** is the permission to access a set of addresses for a span of time.
It is *inherited*: every operation that computes a new pointer from an old one —
`getelementptr`, pointer arithmetic, an explicit pointer-to-pointer cast, a
subobject or slice projection — derives the new pointer's provenance from the
old one's. Provenance is never guessed and never grown.

The operations that change the *shape* of provenance are enumerated, and that
enumeration is the model:

| Operation | Effect on provenance |
| --- | --- |
| `p + n`, `p - n`, `p[i]`, `p.f`, `p->f` | derived from `p`; same allocation; may shrink to a subobject |
| cast between pointer types | derived from `p`; unchanged |
| `p1 - p2` | defined only if both are derived from one allocation; the result is a **value**, not a pointer |
| comparison (`==`, `!=`, `<`, …) | reads only the address; provenance is ignored, so it is always defined |
| `expose(p)` — the source-level `ptr → int` | **gives up** the fine-grained answer: the result is an integer, and the provenance is recorded as *exposed* |
| `with_exposed_provenance(n)` — `int → ptr` | a pointer with the address `n` and permission over **every allocation whose provenance has been exposed**, and no other |
| `address_of(p)` (a `ptrtoaddr`, when a future feature needs the address without disturbing aliasing) | address only, provenance untouched |

Two consequences, both deliberate and both testable:

- **An `int` is not a pointer and a pointer is not an `int`.** Neither direction
  is an implicit conversion; both exist only under their own names. This is what
  makes the compiler able to reason at all, and it is also what makes the design
  portable to a capability target (above).
- **Exposed provenance is a real weakening, and it is named so it can be
  counted.** The moment one pointer is exposed, *every* exposed allocation
  becomes a candidate for every `with_exposed_provenance`. This is the honest
  cost of admitting `malloc` and `dlsym`, and the checked build (§ *Cost*) is
  what keeps it from being invisible.

`expose` is a **lint-visible** operation rather than a keyword-gated one. There is
no `unsafe` block in this language, because the language is unchecked everywhere:
adding a keyword would gate nothing, and the README's "raw pointers need no
keyword" is kept. What the compiler does instead is *count*: `-Wprovenance`
names every site, and the checked build reports the ones that actually mattered.

### Access — the definition

For a pointer `p` with provenance `P`, an access of size `S` at `p` is
**defined by the language** when all four hold:

1. `P` covers every byte of `[p, p + S)`;
2. those bytes lie inside a **single object** that is **alive**;
3. `S = 0` **or** `p` satisfies the alignment its type states;
4. the bytes are **written** by then, for a read.

That is the whole rule. Everything else in this document is either a consequence
of it or a decision about what happens when it is violated.

Four boundary cases, each decided rather than left to the implementation:

- **`S = 0`.** Every pointer with an address — including null — is valid for a
  zero-sized access, and a zero-sized access requires no provenance. This mirrors
  Rust's rule and it is what makes `&arr[len]` and empty slices well-defined.
- **One past the end.** `p + len` for an object of `len` bytes is a valid
  *pointer* (it is derived, in range, comparable and subtractable) and is never a
  valid *access*. The distinction is permanent: it is the same distinction
  between "defined arithmetic" and "defined access" as the `getelementptr` quote.
- **Null.** Null is a valid pointer value: it can be compared, stored, passed,
  `expose`d, and arithmetic on it (producing another number) is fine. It is never
  valid for a non-zero access. There is no nullable/non-null type in the model;
  that belongs to the checked layer, whose non-null type carries exactly this
  obligation in its type.
- **Unwritten bytes.** Reading a byte that was never written is a violation
  (point 4). The static half of this is already shipped: `src/sema/check_flow.cc`
  refuses a read of a `let` no path assigned. The dynamic half is the checked
  build. What is *not* allowed is the LLVM shortcut — a load from a dead stack
  object returning poison is LLVM's rule, not this language's (§ *Lifetime*).

### Arithmetic

- **Scaling is by the pointee, not by the byte.** `p + n` advances `n * sizeof(*p)`
  bytes, and the addition is in the **pointer index width** of the target, with
  two's-complement wrapping on that width. `isize` is the difference type.
- **Wrapping is defined; access is not.** `p` may be moved far outside its object
  — wrapping is fine, and the result is still a pointer with `p`'s provenance.
  This is what makes pointer tagging and sentinel values work, and it is why the
  lowering emits `getelementptr` **without `inbounds`** unless the compiler has a
  proof.
- **`p1 - p2` is defined iff both are derived from the same allocation**, and its
  value is the element count (so it is a multiple of `sizeof(*p)`). Subtracting
  pointers from different allocations is a contract violation, not a number.
- **`inbounds` is emitted exactly when the compiler proved the offset stays
  inside the object at every step** — which, for a `p[i]` whose `i` is a constant
  and `p` is a known-size object, it can. This is the exact analogue of `nsw` in
  `sema.md`: a promise, emitted only when proved, and scanned for.

### Aliasing

**Aliasing is untyped.** There is no `!tbaa`, no `!alias.scope`, and no
`noalias` except the one line below. The rule that makes this affordable is the
object rule: two pointers alias iff their provenances overlap, and provenance is
created by allocation.

Concretely, the language states these, and *only* these:

1. Two pointers derived from the same object **may** alias.
2. Two pointers derived from different objects **never** alias, and the compiler
   may assume it.
3. A pointer derived from an object **may** alias another pointer derived from the
   same object regardless of the types involved. `u8*` and `char*` are not
   special — *every* pointer aliases every other pointer of the same object,
   because memory has no effective type (above).
4. The only way to get more than that is to **write it**:

```
fn copy(dst: *u8, src: *const u8) i32
```

The annotation's meaning is stated as the guarantee it gives the optimizer, in
one sentence, and that sentence is a `noalias`:

> During the call, no memory location modified through `dst` is accessed through
> any pointer not derived from `dst`.

That is LLVM's `noalias` definition verbatim in spirit, and it is narrower than
the folklore around C's `restrict` in two ways worth keeping:

- it is a **write-since** guarantee (read-only uses of the same memory are not
  forbidden), and
- it is a guarantee about **this call**, not about the pointer's lifetime.

And it is **not inferred, ever.** Not from a local whose address does not escape,
not from a parameter that is only read, not from a fresh `alloc` in a loop. Every
inference of that kind is a place a future change to the language silently turns a
correct program into a miscompile, and the history of `noalias` in LLVM — including
the Rust regression that forced rustc to *stop* emitting `noalias` for six years
(rust#31681) — is the evidence that the conservative version is the correct one.

Two mechanisms make the conservative version bearable:

- a **debug-build check**: with the annotation in force, overlapping accesses are
  detected and reported, so a wrong `restrict` is a caught bug rather than an
  `-O2` surprise. C's `restrict` is unchecked by construction; this one is not.
- `captures(...)`, which LLVM 22 supports and which lets a *read-only*,
  non-escaping argument be described without a write-since promise — but it is
  emitted only from a **proof over the function body** (the parameter is never
  stored, never returned, never `ptrtoint`-ed, never passed to a call that could
  do any of those). The proof is a scan in `src/ir`, and the attribute is not
  emitted while the scan does not exist.

### Alignment

Each type has an alignment from the target's data layout, and `sema`'s type store
already computes it (`alignOf`). The rules:

- An object is aligned to its type's alignment. `alloc` takes the alignment as an
  argument and must honor it.
- A load or store of type `T` requires the address to be a multiple of
  `alignof(T)`. **This is a real requirement, not a hint**: LLVM's `load`/`store`
  carry `align N`, and the language must state what happens when it is not met.
- **An unaligned access is a different operation and must be spelled.**
  `unaligned load` / `unaligned store` (spelling owned by the parser record)
  compile to `align 1` accesses and are defined for any address. They are not a
  fallback the compiler inserts; inserting them silently is how a program gets
  slower everywhere to keep one site legal.
- **The compiler never lowers an ordinary access with a weaker alignment than the
  type states**, and never with a stronger one than it proved. The one exception
  is deliberate and is the next bullet.
- **An access to a byte-slice may be emitted as byte accesses** when the
  underlying object's alignment is not known — that is what a `memcpy`-shaped
  lowering is for, and it is chosen by the lowering, not by the language.

Alignment is a property of *objects and accesses*, never of pointer values. A
pointer is valid to compare and to compute with regardless of whether its address
is aligned; only the access asks the question. This is Rust's rule ("valid for
accesses" rather than "valid"), and it is also the only rule that survives
`p + 1` on a `i32*`.

### Initialization

- A **stack object is uninitialized** from its allocation until written. Reading
  it is a violation (access rule 4).
- A **global is zero-initialized** if it has no initializer. This is not a
  language preference; it is the C ABI: a C linker's `.bss` is zero, and a
  language that made it unspecified would make every `extern` global ambiguous.
- **The lowering never emits a load of undef or poison.** LLVM's `undef` is
  deprecated and `poison` propagates; using either to mean "uninitialized" would
  import exactly the inherited answer this model refuses. What the lowering emits
  instead is a *defined* read of bytes that are whatever they are, guarded by the
  checked build's check when the compiler cannot prove the write. The static half
  is `check_flow.cc`, already shipped.
- **A `const` binding is not read-only memory.** It forbids writing the *name*.
  The compiler may not infer `readonly`/`constant` on the backing object from it,
  and this is recorded so nobody "optimizes" it later: `const x: i32 = 5;` and
  `let x: i32 = 5;` produce the same object, and only the name is protected.

  For an aggregate the rule is about **places**, and it reaches one level
  further: `TABLE[0] = 1` and `&TABLE[0]` are refused, exactly as `&c` is
  (`arrays.md` decision 24), while the bytes stay unprotected — a pointer to the
  same object can arrive from another unit, which is why the IR may still store
  through one.

### Lifetime

- A **stack object's** lifetime is its scope, except that taking its address and
  letting the pointer escape ends it earlier — and that is exactly the `escapes`
  question the checked layer's future borrow analysis answers. Until then, the
  rule is the programmer's, and the checked build checks it.
- A **heap object's** lifetime is `alloc` until `free`. A pointer to it is
  well-defined as a *value* after `free` (comparable, storable, arithmetic-able);
  an *access* through it is not. LLVM says the same thing.
- **Use after the end of a lifetime is a contract violation**, in both
  directions, and — this is the decision — **the language does not adopt LLVM's
  exception** where a load from a dead *stack* object returns poison instead of
  being an error. Adopting it would mean a use-after-scope could be *silently
  unobservable* at `-O0` and fatal at `-O2` depending on a pass, which is the
  failure mode this document exists to prevent. The lowering therefore emits
  `llvm.lifetime.start`/`end` for objects whose address is taken (so stack
  coloring works) and *does not* rely on their dead-load exception for
  correctness; the checked build traps, and the release build's behavior is
  whatever the language's rules say about a violation (§ below).
- **The compiler may reuse storage.** Two objects with disjoint lifetimes may
  share an address — that is what stack coloring and `realloc`-free allocation
  do. It is safe precisely because provenance is per allocation: a pointer into
  the first object keeps its provenance after the second is placed there, and
  using it is a violation even though the address is now "valid".

### `volatile`

- `volatile` is a property of a **type**, so it is a property of the object and of
  every access through a `*volatile T`.
- A volatile access is **performed exactly as written**: it is not removed, not
  merged, not widened, not narrowed, not reordered with respect to another
  volatile access of the same object, and not reordered with respect to a call.
  It *is* reorderable with respect to non-volatile accesses and with respect to
  volatile accesses of other objects, which is CC++'s rule and the one every
  compiler implements.
- **`volatile` is not a memory barrier and not a thread primitive.** The language
  says so in the README's own words and in the diagnostic text for the lint that
  asks "did you mean `atomic`". Using it for inter-thread communication is the
  single most common way this feature is misread.
- A volatile access must still satisfy the alignment rule, and may be unaligned
  only if written as an unaligned access.

### Concurrency — reserved, decided in shape, not implemented

There are no atomics, no threads and no `async` yet, so the language **does not
yet have a memory model for concurrent execution**, and this document says so
rather than implying one. What it fixes is the shape, because it constrains the
design that comes later:

- **The intended model is data-race-free + sequentially-consistent atomics**, the
  C11/C++/Rust shape, and not the "everything is UB" or "everything is
  sequentially consistent" extremes. The reason is the same as everywhere else in
  this project: a race that is *defined* costs every optimization, and a race that
  is *UB* costs every bug report.
- **A data race is a contract violation**, in this model's vocabulary, and it is
  **detectable**: it is the one violation a checked build can find with a shadow
  memory (a TSan-shaped check), which is why the checked-build story below is
  worth designing before the feature exists.
- **At this document's level the only requirement today is negative**: the
  lowering must not introduce or remove memory accesses in ways that would create
  races out of naive code — which is already guaranteed by emitting no TBAA, no
  `noalias` that was not written, and no speculative accesses the language did not
  ask for (which is what `dereferenceable` buys, and we do not emit it).

## What minc+ guarantees, and what it does not

This is the section to read twice, because the slogan is easy and the precise
statement is what the compiler actually promises.

**Three grades of operation:**

1. **Defined.** The language states the result: wrapping arithmetic, casts,
   `expose`/`with_exposed_provenance`, pointer arithmetic, comparison, loads and
   stores that meet the obligations, division by a non-zero divisor, and a
   division by zero (which traps — `sema.md`).
2. **Obligation.** The operation has a stated precondition; meeting it makes it
   defined. An access inside a live object, at the right alignment, on written
   bytes. `alloc` returning an object that does not cross the address-space
   boundary. `restrict`: the annotation is the obligation.
3. **Not undefined.** A language where an obligation is a *licence* for the
   optimizer to assume it was met, and any consequence follows. This language
   does not have that grade, and the reason is mechanical: **it hands the
   optimizer no assumption it did not state.** No TBAA, no `inbounds` without a
   proof, no `nsw`/`nuw`, no `dereferenceable`, no `nonnull`, no `nnan`, no
   `range`. The unstated assumption is the thing that turns a bug at one site
   into a changed program at another, and the list of assumptions this compiler
   *is* allowed to make lives in one file and is scanned (§ *How this stays
   correct*).

So the honest sentence is:

> **minc+ has no undefined behavior that the programmer cannot see.** Every
> assumption the compiler gives the optimizer is either proved by the compiler or
> written in the program, and there are no others.

And the honest caveat, in its own paragraph, because it is the part a reader will
otherwise fill in wrongly:

> **When an obligation is violated, the behavior of *that access* is outside the
> model.** It is not "safe", and the language does not claim the rest of the
> program is unaffected — LLVM's own rules (access must be "based on" the object)
> are in force, and they are the language's rules too. What changes is which
> assumptions exist: the violation makes the program wrong, not the optimizer
> arbitrary, and a checked build reports it at the boundary instead of letting it
> surface as a mystery three functions later.

| Situation | Checked build | Release build |
| --- | --- | --- |
| Access outside the object | **trapped** with a named site (`memory-out-of-object`) | outside the model |
| Access at the wrong alignment | **trapped** (`memory-misaligned`) | outside the model |
| Access through a null pointer | **trapped** (`memory-null`) | outside the model |
| Read of unwritten bytes | **trapped** (`memory-uninitialized`) | outside the model |
| Access after the object's lifetime | **trapped** (`memory-dangling`) | outside the model |
| Two pointers violating a written `restrict` | **reported** (`memory-restrict-overlap`) | outside the model |
| A data race (future) | **reported** by the race check | outside the model |

"Outside the model" is a deliberate phrase: it is not "undefined behavior" with
its loaded history, it is "the program did not meet a precondition the language
states in writing", and the diagnostic vocabulary above is how a reader is told
*which* precondition.

## The surface — what `.mx` will spell

The model above is surface-independent. The surface lands in stages, and this
section fixes the shape so that stage one does not have to be redone:

**Stage 1 — raw pointers, the C-compatible floor.** `*T`, a prefix type
constructor, because the type grammar is positional and `*` in type position
cannot conflict with multiplication; `&x` for the address of a modifiable lvalue;
`*p` to dereference; `p[i]` as `*(p + i)`; `p.f`/`p->f` when aggregates land.
`str` keeps its NUL-terminated, `ptr`-to-`i8` representation, because the ABI
wants it and because `str` has no length to carry. A `*T` carries no length —
that is what makes it C-compatible and what makes the obligation the
programmer's.

**Stage 2 — the checked layer.** A pointer that carries its extent
(`slice<T>`) and a reference that carries permission (`&T` / `&mut T`), both of
which *discharge the same obligations* as above, by construction. Nothing in
§ *The model* changes when they land; the `AccessObligation` record below gains
the "proved" value it already has a slot for.

**Stage 3 — the surfaces that only exist because of interop.** The ABI facts a
value crossing the boundary obeys — layout, alignment, calling convention — plus
`restrict` annotations, function pointers, and the FFI wrapper's own rules, in
`src/cinterop` with its own record. `extern fn` itself has already landed and is
the language's *declaration* form rather than a boundary rule
([`extern.md`](extern.md)); what stage 3 adds is what happens to a type that
crosses, not how the crossing is spelled.

**Not in the model, on purpose:** no `unsafe` keyword (the language is unchecked
everywhere; the counted operations above are what a reader audits), no smart
pointers, no GC, no reference counting, and no borrow checker *in the model* — it
would be a discharge mechanism for stage 2, and the model is deliberately written
so it can be added without reopening a rule.

## Where this lands in the compiler

The model is not a document plus a wish list; every rule above has a place it is
implemented, and the ones that must land **before** anything dereferences a
pointer are marked.

| Where | What it gains | Why here |
| --- | --- | --- |
| `include/sema/type.h` | **shipped:** `TypeKind::Pointer` has its body — a `pointee` `TypeId`. Qualifiers (`const`, `volatile`) still arrive with the syntax that spells them | The kind was reserved from the start, so landing the surface was a *body* and not a `case` — and `ir.md`'s mapper now maps `Pointer` to `ptr` instead of refusing it by name |
| `include/sema/type_store.h` | **shipped:** `pointerTo` interning, plus `sizeOf`/`alignOf` for a pointer; array layout is decided (the element's **complete** size × the count, the element's alignment) and lands with the syntax | One place owns sizes and alignments; the pointer width already existed (it is what `str` used). The layout rule is [`arrays.md`](arrays.md) decision 6, because `p + 1` landing on the next element *is* a memory rule |
| `src/sema/check_expr.cc` | **shipped:** `&`, `*`, `[]`, and the lvalue/modifiable-lvalue rules they extend | The lvalue machinery was already there (`const` assignment, `++`/`--`, parens) |
| `src/sema/check_flow.cc` | **shipped:** the initialization half of the access rule, for objects whose address is taken | It was already the pass that owns "is this byte written" |
| **`src/sema/access.cc`** (the record) | **shipped:** `TypedFile::accesses()` — an `AccessObligation` per dereference, with `accessAt(node)` | **The lowering may not re-derive an alignment or a provenance fact**, exactly as it may not re-derive a conversion. The shape, and the two fields deliberately narrower than this model, are in the section below |
| `src/ir/values.h` | `Place` gains producers: `&`, `*`, indexing, field projection | `ir.md` already specifies the table over producers as the mechanism, so a new producer is a line |
| `src/ir/expr.cc` | the accesses themselves, from the record | It materialises decisions, it does not make them |
| `src/ir/runtime.cc` | the checked-build access guards (null, alignment, bounds, liveness) and `expose`/`with_exposed_provenance` | `ir.md` already designates it as the home of the operations the hardware does not define |
| `src/ir/invariants.cc` | the assumption scan: no TBAA metadata, no unproved `inbounds`, no `nsw`/`nuw`, alignments as recorded, no `dereferenceable`/`nonnull` the language did not state | It is the file `ir.md` already designates for exactly this; the list is § *Not undefined* turned into code |
| `src/driver` | `-Wprovenance` and the checked-build switch (`-fcheck`), which is what `-O0` defaults to | Driver decisions, and the flags have to exist before the checks are useful |
| `docs/architectures/ir.md` | the assumption list it already promised — now the enumerated table the scan reads, with `inbounds` marked as the one row whose proof still has no home | The list is a property of what gets *emitted*, which is that stage's business; the rules it encodes stay here, and it links back for them |

### The record the lowering is not allowed to re-derive

`ir.md` already made the argument for conversions and for the operation type of
`op=`; memory needs the same thing for the same reason. The lowering must not
*recompute*:

- **the alignment of an access** — it comes from the type and the target's data
  layout, and re-deriving it means a second copy of the layout table;
- **whether an access is checked, unaligned, or volatile** — three different
  emitted instruction shapes, and the tree's type does not distinguish them;
- **whether the pointer is a `Place` and what its provenance is** — the
  difference between `getelementptr` and a load, and between `inbounds` and not.

So `sema` publishes, per access node:

```
struct AccessObligation {
  ast::AstId place;      // the `*p`, the `p[i]` or the `a[i]` the lowering stands on
  TypeId type;           // what is accessed, which is the size and the alignment
  AccessKind kind;       // ordinary | unaligned | volatile
  ProvenanceKind provenance; // object | foreign
  // The count of the object the place is inside, when this stage can prove one;
  // `0` is "not known", which is every `*p`. Added by `arrays.md` decision 26.
  std::uint64_t extent = 0;
};
```

Two of those fields are deliberately narrower than the model allows, because
the stage can only answer what the grammar lets it see, and a value no input can
produce is a value no test can pin:

- **`kind` is `ordinary` alone** until `unaligned` and `volatile` have syntax to
  ask for them; the enumeration grows with the keyword, and the two consumers
  (`src/ir`'s aligned and volatile access shapes) arrive with it.
- **`provenance` is `object | foreign`**, which is the *proof* the syntactic rule
  can carry: `object` for the address of an object this unit named and moved by
  arithmetic since, `foreign` for everything the compiler cannot name. The
  `derived | exposed | with_exposed` split belongs to the operations that join a
  pointer and an integer (`expose`, `with_exposed_provenance`), which are stage
  three; when they land, `foreign` refines into `exposed` and `with_exposed` and
  nothing else in the record moves.

and `TypedFile::accesses()` is the question. The lowering's converse rule is the
project's oldest one: **a missing obligation is a refusal, not a guess** —
`ir-missing-obligation`, which is an ICE (a bug in this compiler) and not a
diagnostic about the user's program, because a program that type-checked cannot
be missing one.

## Cross-platform

Memory is where cross-platform stops being a build question and becomes a
semantics question, so the rules are stated per row and each has a mechanism:

- **The pointer width is the target's.** `isize`/`usize`, `sizeof(*T)` and the
  alignment of every type come from `sema`'s triple table, and `ir` asserts that
  table against LLVM's own `Triple`/`DataLayout` (the test `ir.md` already names).
  There is no host `#ifdef` in this path.

  The assertion is a mechanism and not a sentence: the first time a type is
  mapped into a module, `Lowering::layoutOf` compares the store's `sizeOf`/`alignOf`
  against the target's data layout for the shape it just built, and a
  disagreement is an `ir-internal` naming the type, both numbers and the target --
  the module is not emitted. It is the same category as a missing access
  obligation (a bug here, not a statement about the program), and it runs per
  *mapping*, so a type the target refuses is still refused by the mapper with its
  own sentence. `tests/unit/ir/layout_test.cc` walks every named triple, and it is
  the test that was written by the bug it prevents: i386's ABI is
  `...-i64:32:64-...-f64:32:64-f80:32`, so a 64-bit value there is aligned to four
  bytes and the x87 format is **twelve** bytes in a four-byte slot -- while this
  table said 8 and 16, which made every `i64`, `f64` and `f80` object on that
  target an alignment the scan refused and, for `f80`, a size eight bytes too
  long for anything that counted bytes.
- **The address-space-boundary rule is target-dependent in practice.** "No object
  may cross the unsigned address space boundary" is satisfied for free on 64-bit
  and is a real allocator obligation on a 32-bit triple, where a large `alloc`
  near the top of the address space is not just slow — it is out of the model.
  `alloc`'s contract states it, the checked build tests it, and this is the row
  that would otherwise be discovered by a user on ARM32.
- **Alignment differs by target** (`f80`'s 16-byte slot on System V, `double` on
  Darwin), and it is the data layout's answer, never the host's.
- **Capability targets** (CHERI-shaped): everything here except `expose` is
  target-independent, and `expose` is documented as the operation that does not
  survive there — which is the honest trade for having `malloc` at all.
- **The allocator interface is the one place the model meets an implementation**:
  `alloc(size, align)`, `free(p)`, and the obligations above as its contract, with
  a checked build that verifies them. It is deliberately *not* `malloc` — `malloc`
  is reached through FFI, where C's rules apply.
- **The checked build itself must be cross-platform.** It is a shadow-memory
  implementation in the runtime, keyed by allocation, with no `#ifdef`: the
  platform-specific part is only how a shadow region is reserved, which is
  `src/support/`'s business like every other platform question.

## Cost

The design's cost is a deliberate trade, and it is stated so it can be audited:

- **Checks are in the checked build, not in the optimized one.** `-O0` and
  `-fcheck` trap; the release build does not pay for guards, exactly as
  `sema.md`'s division guards exist for the divisor the compiler cannot see. A
  language that checks by default and has no way to stop is a language nobody
  ships, and one that never checks is a language whose model is aspirational.
- **The untyped-aliasing decision costs somewhere and nothing somewhere else.**
  No TBAA means an optimizer cannot use the types of the accesses to disambiguate
  them, which is a real cost in pointer-heavy code — the same cost the Linux
  kernel and most distributions already pay by building with
  `-fno-strict-aliasing`. The recovery path is a written annotation, and it
  recovers *where it is true* instead of everywhere it is plausible.
- **No `noalias` by inference costs the same as C's `restrict` being rare**, which
  is the status quo in C code that was not written for a compiler. It is the price
  of not having a Rust borrow checker, and it is paid once per hot function rather
  than once per program.
- **What is *not* a cost:** objects, provenance and the no-`inbounds` rule. These
  describe what LLVM already does; being explicit about them does not remove an
  optimization, it removes the *silent* ones.
- The numbers that matter will be measured by the differential oracle
  (`ir.md` § *Tests*): one program compiled by this compiler and by the system C
  compiler, run and compared, at `-O0` and at `-O2`. A rule that costs a
  measurable amount is a rule that gets a note in this section, not a rule that
  gets quietly dropped.

## How this stays correct when the language grows

Same standard as `ir.md`'s five rules: mechanical, not "be careful".

1. **`TypeKind::Array` is still a refusing `case`** in `ir.md`'s type mapper (`ir-unsupported-type`), and `Pointer`, which used to be, is now a body — which is the mechanism working: the change was one `case`, the mapper is exhaustive with no `default:`, and a kind nobody handled is a build error rather than a silent gap. What the array case will *say* is decided ([`arrays.md`](arrays.md), including the storage type of an array of `bool` and the aggregate ABI the boundary refuses).
2. **A new producer of `Place` is a compile error** while the table in
   `values.h` is exhaustive with no `default:`.
3. **A new access kind is a compile error** in `AccessKind`'s switch, and a
   `TestAllAccessKinds`-shaped enumeration test fails if a kind exists that no
   test produces.
4. **The assumption list is closed and scanned.** § *Not undefined* names every
   assumption this compiler may hand the optimizer. The list lives in one file,
   `invariants.cc` reads it, and a test fails if the module contains an assumption
   that is not on the list — so *adding* one is a two-file change with a test,
   which is exactly the friction it should have.
5. **An annotation is a *written* thing or it does not exist.** `restrict` and
   `captures(...)` are emitted from source, never inferred from a shape the
   language might change. A future borrow analysis may *prove* an obligation,
   which moves a site from the programmer's column to the compiler's; it may
   never *assume* one.
6. **The allocator's obligations are in its signature**, so a second allocator
   (an arena, a pool, a custom one) inherits them and the checked build verifies
   them, rather than each allocator re-deriving the rules.
7. **Every new target adds a row** to the pointer-width and alignment tables, and
   `ir`'s data-layout assertion fails until the row is right. The alignment half
   is three numbers in `TargetInfo` (`int64AlignBits`, `float64AlignBits`,
   `float80AlignBits`) rather than a rule, because i386 is the worked example of a
   target whose ABI does not align a value to its own width -- and the assertion
   in `layoutOf` is what makes a wrong one a refusal instead of an object file.

## Non-goals

- **A borrow checker in the model.** It is a discharge mechanism for the checked
  layer, and the model is written so it plugs in without a rewrite. Committing to
  it here would be deciding the largest feature of the language in a document
  about aliasing.
- **A GC, reference counting, or an object graph.** Ownership is the
  programmer's, and the allocator interface above is the whole of what the
  language provides.
- **An object model with `struct` layout, unions, bitfields, and the C calling
  convention for aggregates.** Those are `src/cinterop`'s and get their own
  record; this document only requires that the object has a size and an
  alignment, which it does.
- **A formal proof of the model.** The validation is the differential oracle and
  the checked build, as everywhere else in this project.
- **`restrict`'s full C wording.** We take the useful half (the write-since
  guarantee) and check it.
- **A `volatile`-as-barrier or atomics-as-`volatile` reading.** Both are
  explicitly rejected.
- **A second, "unsafe" model.** There is one model; `expose` is a counted
  operation inside it.

## Decisions

| # | Decision | Why |
| --- | --- | --- |
| 1 | **Two floors:** the C ABI is byte-identical; the semantics are ours | The OS, `libc`, the linker and LLVM all speak the C ABI, so it cannot be dropped; none of our aliasing/lifetime rules cross FFI, so interop is unaffected by them |
| 2 | **FFI is where the two floors meet**, and nothing of the model crosses it | It is the one place a `*T` and a C pointer must be the same thing, so it is the one place the rules have to agree — and it is one stage, not the language |
| 3 | **Memory has no effective type**; a store of one type and a load of another is defined | LLVM states that it does not associate types with memory; claiming otherwise would be a rule the IR cannot express |
| 4 | **Objects are allocations**, each stack binding its own | It is what makes two adjacent locals non-aliasing, which is the basis of every optimization |
| 5 | **Access is `[p, p+S) ⊆ a live object that `p`'s provenance covers`** | The single rule from which the rest follows; it is also LLVM's own |
| 6 | **A pointer is address + provenance**, and provenance never grows | Without it, `p + n` would be "whatever is at that address", which is the C position this document refuses |
| 7 | **`ptr → int` and `int → ptr` are named operations**, never implicit | They are where provenance is lost; naming them is what makes the loss auditable, and it is what a capability target needs |
| 8 | **No `unsafe` keyword**; the counted operations are what a reader audits | The language is unchecked everywhere, so a keyword would gate nothing; `-Wprovenance` counts instead |
| 9 | **No TBAA, ever**, and no `noalias` except a written annotation | An inferred `noalias` is the classic silent miscompile; the history of the attribute is the evidence |
| 10 | **No `inbounds` unless proved**, the analogue of no-`nsw` | A promise the compiler did not prove is a licence to change a correct program |
| 11 | **Alignment is a property of the type and the access**, and an unaligned access is spelled | It is the difference between "slower" and "wrong", and only the source knows which was meant |
| 12 | **Globals are zero-initialized**; stack objects are not | It is the C ABI's `.bss`, and a language that left it unspecified would make every `extern` global ambiguous |
| 13 | **The lowering never emits `undef` or `poison`**, and does not adopt LLVM's dead-stack-load exception | Reading unwritten bytes must be the same error in every build; the exception makes it pass-dependent |
| 14 | **A pointer remains a valid value after its object dies**; the access does not | It is LLVM's rule, and it is what keeps comparison, arithmetic and tagging defined |
| 15 | **`const` protects a name, not memory** — and for an aggregate it protects every *place* reached through the name: an element store and an element's address are refused (`arrays.md` decision 24) | Inferring `readonly` from it would make a `const` binding's object different from a `let`'s, which the ABI and the user both see |
| 16 | **Comparison ignores provenance**; ordering on two addresses is defined | Two addresses are two numbers; C's UB here is one of the refusals above |
| 17 | **`volatile` is an access property, not a barrier**; `atomic` is a different feature | Getting this wrong is the most common memory-model bug in C-family code |
| 18 | **Concurrency is reserved**, DRF + SC atomics, and a race is a detectable violation | The shape constrains the future design; the checked build is why a race is findable rather than theoretical |
| 19 | **Checks live in the checked build** (`-O0`/`-fcheck`), not the release build | A language that cannot stop checking is unshippable; one that never checks has an aspirational model |
| 20 | **The assumption list is closed, in one file, and scanned** | "No undefined behavior" is a slogan; a scanned closed list is a guarantee that fails in CI when it is violated |
| 21 | **The model lands with the record** (`AccessObligation`) before the first `*` is lowered | The same rule the coercion record was: the lowering materialises, and a missing decision is a refusal |
| 22 | **The model is surface-independent**, and the checked layer discharges the same obligations | So stage 2 is an addition, not a rewrite — and so this document does not have to guess what the surface will be |

## How the claims above are checked

The ladder, in build order:

1. **The invariant scan, always on.** Every module is scanned for the closed
   assumption list: no `!tbaa`/`!alias.scope`/`!noalias` metadata, no `inbounds`
   without a recorded proof, no `nsw`/`nuw`, no `dereferenceable`/`nonnull`/`noundef`,
   and every `load`/`store` alignment equal to what its `AccessObligation`
   recorded. This is what makes § *Not undefined* a fact rather than a claim.
2. **The obligation record's enumeration test**, in the shape of
   `coerce_test.cc`: for every access kind and every provenance kind, a program
   that produces it, and an assertion that the lowering's emitted shape is the
   recorded one.
3. **The refusal tests.** An access with no recorded obligation, an `Array` type
   reaching `ir`, a `restrict` violation in the checked build: each is a named
   diagnostic and **no module**. "No module" is part of the assertion.
4. **The checked build's own tests**, one per row of the violation table: an
   out-of-object access, a misaligned access, a null dereference, a read of
   unwritten bytes, a use after the end of a lifetime, a `restrict` overlap. Each
   traps or reports with its named code, and each must trap in `-O0` *and* in the
   checked build at `-O2`, because a check the optimizer can remove is not a check.
5. **The differential oracle** (`ir.md` § *Tests*): the same program compiled here
   and by the system C compiler, executed and compared — at `-O0` and at `-O2`.
   It is the only test that can prove the two floors are in the right order, and
   it is where the cost numbers in § *Cost* come from.
6. **A cross-target alignment/width test**: the `DataLayout` assertion `ir.md`
   already names, extended to `sizeof`/`alignof` for every type in the store. It is
   the one place the front end's target model can be wrong with nothing else
   noticing. **Shipped** as two halves, both in `tests/unit/ir/layout_test.cc`:
   `EveryNamedTargetAgreesWithItsDataLayout` lowers one program that uses every
   scalar, an array of one, an array of `bool`, a slice and (where the ABI has it)
   the x87 format against all seven named triples, so the *check itself* is the
   assertion; `EveryNamedTripleIsTheOneLlmParses` compares the architecture, OS and
   ABI-selecting environment this compiler parsed against LLVM's `Triple`, and the
   pointer width against the `DataLayout` of the module it built. `sizeof` and
   `alignof` are not in the assertion yet because they are not in the grammar yet --
   when they land they read `sizeOf`/`alignOf`, so they are covered by construction
   rather than by a new test.
7. **The property tests that are cheap and exhaustive**: pointer arithmetic
   round-trips within an object, `p + n - n == p` for in-object `n`,
   `p1 - p2` consistency, and `p[i]` equal to `*(p + i)` for every type in the
   store.

## References

Read for this document, and quoted where the wording is the argument:

- **ISO/IEC TS 6010**, *A provenance-aware memory object model for C*, published
  2025-05; draft **N3231**. WG14's project page lists it as a TS, which is the
  fact that decides § *C*: C23 (ISO/IEC 9899:2024) has no normative provenance
  model, and the C2y draft (N3886) is where the wording still moves.
- **LLVM 22 LangRef**, read from the installation this project builds against:
  *Allocated Objects*, *Object Lifetime*, *Pointer Aliasing Rules*, *Pointer
  Capture*, `noalias`, `captures`, `getelementptr` (`inbounds`/`nusw`/`nuw`),
  `ptrtoint`, `ptrtoaddr`, `inttoptr`, and *Pointers with external state*. Every
  quotation in § *LLVM* is from there.
- **Rust `std::ptr`** module documentation and the **Reference**'s section on
  undefined behavior — the allocation axioms, the spatial/temporal/mutability
  provenance model, "no operation can ever grow the permissions", and the
  explicit statement that the aliasing rules are not yet decided.
- **RFC 3559**, *Rust has provenance* — the strict/exposed provenance split, and
  the reason `with_addr` and `with_exposed_provenance` are different functions.
- **rust#31681** and the `noalias` history around it: the reason decision 9 is
  conservative.
- **Clang's bounds-safety attributes** (`__counted_by`, `__sized_by`) as the
  C-side attempt to put an extent back into a pointer — the same idea as the
  checked layer's `slice<T>`, retrofitted.
- The languages in § *What the checked languages do* are cited for their
  *position*, not their wording: Go and Swift check, D has `@safe`, Zig does not,
  Ada checks by default.
