---
sidebar_position: 9
---

# The memory model

The language's answer to "what is a pointer, and what happens when one is used
wrongly" is stated in writing, once, and the compiler is built to keep it. This
page is the summary; the full statement, with the argument and the references, is
`docs/architectures/memory.md` in the repository.

## The model in one page

1. **A pointer is an address plus a provenance.** Provenance is the permission to
   access a particular allocation, for a particular time. It is not the address.
2. **An object is an allocation**, identified by a base and a byte size. There is
   no "effective type" of memory: storing an `i32` and loading an `f32` from the
   same bytes is defined.
3. **An access is `[p, p + size)` lying inside a live object that `p`'s provenance
   covers.** That one sentence is the whole access rule.
4. **Aliasing is untyped unless the source says otherwise.** No type-based alias
   analysis, ever. The only `noalias` in the module comes from an annotation the
   programmer wrote.
5. **A pointer is not an integer.** `expose` (pointer → integer) and
   `with_exposed_provenance` (integer → pointer) are *named operations*, and they
   are the only place provenance is lost or regained.
6. **No `inbounds` unless the compiler proved it** — exactly as there is no `nsw`
   on arithmetic the language defines to wrap.
7. **An access must satisfy the alignment its type states.** An unaligned access
   is a different, spelled operation.
8. **Reading unwritten memory, out-of-object access, and use after an object's
   lifetime are contract violations, not undefined behavior.** The difference is
   the subject of the section below, and it is smaller than the slogan "no
   undefined behavior" makes it sound, which is why it is stated precisely rather
   than sold.
9. **The language never hands the optimizer an assumption it did not state.** The
   list of assumptions is closed, lives in one file, and a scan reads that file —
   so a new assumption is a code change plus a test, not a comment.
10. **The C ABI is the floor; the semantics are ours.** Layout, alignment, calling
    convention and object sizes are byte-identical to C. Provenance, aliasing,
    lifetime and what counts as a violation are the language's own, and none of
    them crosses the FFI boundary in either direction.

## Three grades of operation

**Defined.** The language states the result: wrapping arithmetic, casts,
`expose`/`with_exposed_provenance`, pointer arithmetic, comparison, loads and
stores that meet their obligations, division by a non-zero divisor — and a
division by zero, which traps.

**Obligation.** The operation has a stated precondition, and meeting it makes the
operation defined: an access inside a live object, at the right alignment, over
written bytes; `alloc` returning an object that does not cross the address-space
boundary; `restrict`, where the annotation *is* the obligation.

**Not undefined.** This language does not have the third grade that C and C++ do
— the one where meeting an obligation is a *licence* for the optimizer to assume
it was met and let any consequence follow. The reason is mechanical: **it hands
the optimizer no assumption it did not state.** No TBAA, no `inbounds` without a
proof, no `nsw`/`nuw`, no `dereferenceable`, no `nonnull`, no `nnan`, no `range`.

So the honest sentence is:

> **minc+ has no undefined behavior that the programmer cannot see.** Every
> assumption the compiler gives the optimizer is either proved by the compiler or
> written in the program, and there are no others.

And the honest caveat, because it is the part a reader will otherwise fill in
wrongly:

> **When an obligation is violated, the behavior of *that access* is outside the
> model.** It is not "safe", and the language does not claim the rest of the
> program is unaffected — LLVM's own rules (an access must be "based on" the
> object) are in force, and they are the language's rules too. What changes is
> which *assumptions* exist: the violation makes the program wrong, not the
> optimizer arbitrary.

## What a checked build reports

| Situation | Checked build | Release build |
| --- | --- | --- |
| Access outside the object | **trapped**, with a named site (`memory-out-of-object`) | outside the model |
| Access at the wrong alignment | **trapped** (`memory-misaligned`) | outside the model |
| Access through a null pointer | **trapped** (`memory-null`) | outside the model |
| Read of unwritten bytes | **trapped** (`memory-uninitialized`) | outside the model |
| Access after the object's lifetime | **trapped** (`memory-dangling`) | outside the model |
| Two pointers violating a written `restrict` | **reported** (`memory-restrict-overlap`) | outside the model |
| A data race (future) | **reported** by the race check | outside the model |

"Outside the model" is a deliberate phrase: it is not "undefined behavior" with
its loaded history, it is "this program did not meet a precondition the language
states in writing" — and the diagnostic vocabulary above is how a reader is told
*which* precondition.

## Where the language is today

The model is implemented in the *surface* it has so far — pointers, `null`,
`*void`, stepping, comparison, the two provenance joins as casts (`p as usize`
is `expose`, `addr as *u8` is `with_exposed_provenance`, both counted by
`-Wprovenance`), and the refusals that go with them (a pointer is not an integer
*by conversion*, `*void` cannot be accessed) — and the records the model needs are
what `sema` publishes for the lowering to read: the access obligation, the
alignment, the conversion, the operation type.

:::note[Not implemented yet]
Aggregates (`struct`, which needs member alignment), `restrict`, address spaces,
`alloc`/`free`, the checked build and its guards, the *named* forms `expose` /
`with_exposed_provenance` (the casts that carry their semantics are implemented —
see [Casts](/language/expressions#casts)), and `volatile` are not implemented.
Arrays are: `[N]T` with the count in the type is what makes `&a[0]` and `&a` two
different, checkable pointers. What exists is the model, the reasoning, and the
parts of the surface that do not need the rest.
:::

## Why it is worth reading even now

Two design decisions that are already load-bearing came out of this model and
would be expensive to undo:

- **The assumption list is closed and scanned.** Adding one is a code change with
  a test, which is what keeps "no undefined behavior the programmer cannot see"
  from decaying into an aspiration.
- **Records, not re-derivation.** The stage that has the information decides, and
  every stage below reads. That is why the lowering cannot invent an `inbounds`
  or a conversion: the facts it needs are in the tree, and a fact that is not
  there is a fact nobody decided.
