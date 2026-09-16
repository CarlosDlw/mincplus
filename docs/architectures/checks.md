# The checked build — `-fcheck`

`memory.md` states one obligation per access, and then says something unusual about
violating one: the *access* leaves the model rather than the program becoming
arbitrary. This file is the other half of that sentence. A **checked build** is the
compiler watching its own program: it puts a guard in front of an access, and when
the guard fires it prints where it fired and stops.

Two things are deliberately not this file. The *model* — what an access means, what
provenance is, which situations are violations — is `memory.md`, and it is not
restated here. The *lowering* — that a guard is emitted from the access record and
never invented — is `ir.md` § *The runtime contract* and § *The checked build's
guards*, and this file is the implementation of those rows plus the parts of them
that turned out to need a decision.

## The question, and what the market answers

A language that promises "if the checker lets it pass, it must run" has to decide
what happens to the promises it cannot *prove* — a pointer that arrives as a value,
an index that is computed, an object whose lifetime the compiler cannot see. There
are two families of answer in wide use, and they are not interchangeable:

| Family | Examples | What a violation *is* | Cost model |
| --- | --- | --- | --- |
| **Defined trap** | Ada `Constraint_Error`; Go's `index out of range` / nil panic; Rust's `panic!` from a bounds check or an overflow check | an outcome the language *names*: the program stops, and the reason is part of the semantics | paid for in every build, or removed only where the compiler proves it cannot fire |
| **Diagnostic layer** | `-fsanitize=undefined`, `-fsanitize=address`, MSVC `/RTC`, `_GLIBCXX_ASSERTIONS` / `_LIBCPP_HARDENING_MODE` | a *tool's* report about a program that is already outside the language | off by default, on for testing, and never part of the meaning |

Both families split again on *who does the checking*, which is the distinction this
compiler actually needs:

- **The module can answer** — the check reads values the instruction already has:
  an address against zero, an address against an alignment, an index against a
  count that is a number in the type. Every tool above does these, and they are
  cheap because nothing has to be tracked.
- **Only a shadow memory can answer** — the check needs state about *other*
  memory: whether this byte was ever written (MSVC `/RTCu`, Valgrind), whether the
  allocation this pointer came from is still alive (ASan's quarantine, Valgrind),
  whether two `restrict` pointers overlap. AddressSanitizer is the reference
  implementation: one shadow byte per eight bytes of address space, a redzone
  around every allocation, and a runtime library that reads the map.

`memory.md` already assigns each violation to one of the two (its table of
`memory-null`, `memory-misaligned`, `memory-out-of-object` versus
`memory-uninitialized`, `memory-dangling`, `memory-restrict-overlap`), and
`ir.md` states the rule that makes the split usable: the module-answerable guards
are emitted **from the access obligation**, in every build the flag is on, and the
shadow-memory half is *not deleted* where it will eventually be called.

Two market observations shaped what is below rather than being copied:

1. **A check the optimizer can delete is not a check** (`ir.md`). Rust's overflow
   checks are a profile *default* and can be turned off per build; Go's bounds
   checks are contract and can only be *proved* away. This compiler's checks are
   neither proved away nor deleted by the optimizer, because they are only in the
   checked build to begin with — and the build that has them keeps them at every
   optimization level, which is the property `memory.md` decision 19 asks for.
2. **A trap with no message is a tool, not a language.** Go prints the index and
   the length; MSVC `/RTC` prints a line of its own; a bare `SIGILL` prints nothing
   and is what this compiler did before this file. The message is the difference
   between "the program is broken" and "the program is broken *here*, because of
   this rule".

## The guards

Five rows, and every one of them is emitted from a fact in the access record
(`sema::AccessObligation`) rather than derived here:

| Row | The record says | What the lowering emits |
| --- | --- | --- |
| `memory-null` | there is an obligation at all | the address against zero, and a trap on the equal edge |
| `memory-misaligned` | the type being accessed | the address's low bits against `alignOf(type)`, unless that alignment is 1 |
| `memory-out-of-bounds`, static | `provenance == Object`, `extentKind == Count`, and the count | the index against the count, unsigned |
| `memory-out-of-bounds`, slice | `extentKind == Length` | the index against the descriptor's `len` word, unsigned |
| *(no row)* | `provenance == Foreign`, no extent | nothing: the module has no base to compare against, and the access is exactly the one `memory.md` says only a shadow memory can judge |

Three details are decisions rather than implementation:

- **The index comparison is unsigned, and that is the whole check.** A negative
  index is a large unsigned number, so one `icmp uge` catches both ends; a signed
  comparison plus a separate sign test would be two instructions that can disagree.
- **`extent` is a property of the *record*, and the slice's length is one too.**
  A slice's length is a *value*, not a number in a type, and the first draft of
  `ir.md` therefore said a slice subscript carried no extent — which left the most
  common indexed access in the language unchecked while `a[i]` on a four-element
  array was. The record grew a third answer instead (`ExtentKind::Length`), because
  the alternative was the lowering inventing a bounds check from the *shape* of the
  descriptor, and "the lowering decides nothing" is the rule that has earned its
  keep in every stage.
- **The null guard is emitted for every obligation, including one whose address is
  provably not null** (a `getelementptr` of an alloca). `ir.md` states the reason:
  a checker that deletes the checks it can prove is a checker that stops reporting
  the day the proof has a bug. The release build deletes all of them at once, which
  is a decision about a *build* and not a proof about a program.

The guard's shape is one shape, and it is the shape the existing runtime contract
already uses for division and float-to-int conversions: the value is tested, a
conditional branch sends the failing edge to a block that traps and the passing
edge to the block the access lives in. `invariants.cc` reads the finished module
and reports `ir-unguarded-access` when an access through a pointer has no such test
in front of it (`### The scan` below).

## The runtime, and the message

The lowering emits, once per module that has any guard:

```llvm
declare i32 @fflush(ptr)
declare i64 @write(i32, ptr, i64)          ; `declare i32 @_write(i32, ptr, i32)` on PE

define internal void @__minc_check_fail(ptr %message, i64 %length) {
  call i32 @fflush(ptr null)
  call i64 @write(i32 2, ptr %message, i64 %length)
  call void @llvm.trap()
  unreachable
}
```

and each guard passes a private constant string of its own:

```console
$ mincc build -fcheck -o prog t.mx && ./prog
mincc: trap: memory-out-of-bounds at t.mx:4:3
mincc: trap: memory-null at t.mx:9:12
```

Four decisions are in that snippet:

1. **The message is built by the compiler, not by the program.** The site is known
   at compile time — file, line, column, and which rule — so there is no formatting
   at run time, no `printf`-style machinery in the module, and no way for a message
   to be wrong about where it came from. It is also the same information a
   *diagnostic* carries, which is what makes the two readable side by side.
2. **`write` to descriptor 2, with `fflush` first.** `write` is the one interface to
   standard error that is a plain function on every platform this compiler targets
   (`write` in POSIX libc, `_write` in the Windows CRT import library and in MinGW
   — hence the one-symbol difference, chosen from the module's own triple and from
   nothing else). The `fflush(ptr null)` before it is ISO C's "flush every output
   stream": without it the program's own buffered output would be lost behind the
   trap, and a reader would be looking at a message with none of the context their
   program printed first.
   **Printing through `stderr` was the alternative and it does not work**: in IR
   there is no macro, and `stderr` *is* a macro — MinGW spells it
   `__acrt_iob_func(2)` and exports no data symbol of that name, so the module
   would link on Linux and macOS and fail on the Windows runner.
3. **`llvm.trap` after the message, and not `exit` or `abort`.** A trap is what the
   language already defines for the value violations (`runtime.cc`), it is what a
   debugger stops on with the frame intact, and it is the one ending that cannot be
   confused with the program choosing to stop. `exit(101)` would be an *orderly*
   end to a program that is not in an orderly state, and it would land in the same
   place a program's own `exit` lands.
4. **`internal` linkage, one function, and nothing exported.** The checked build
   adds no symbol to the program's namespace and no library to the link line: it
   is a function in the module, and `build --emit obj` produces an object that
   links exactly like an unchecked one (the only new undefined symbols are the C
   library's `write` and `fflush`, which every hosted program already has).

The semantic traps — division by zero, a shift count past the width, a
float-to-int conversion out of range — keep the bare `llvm.trap` they have always
had, in every build, with no message. That asymmetry is deliberate and is stated in
`ir.md`: those are the language *defining* an outcome, so they are not a diagnostic
and must not depend on a flag, while a checked-build guard is a report and exists
only where the flag says so. `memory.md` § 11 says the same thing from the other
side: the runtime that carries messages and a panic handler is its own module, and
this file is the first slice of it rather than a decision about its shape.

## The flag

| Flag | Effect | Default |
| --- | --- | --- |
| `-fcheck` | guards on, at any `-O` level | **on at `-O0`**, which is what `memory.md` decision 19 asks for |
| `-fno-check` | guards off at `-O0` | off at `-O1` and above |

So a plain `mincc build prog.mx` is a checked build and `mincc build -O2 prog.mx`
is not, which is Rust's profile shape and Go's `-race`/`-gcflags` shape: the build
you are *developing* with is the one that reports, and the build you *ship* pays
nothing. Two spellings and not one, because a flag has to be able to say no: an
`-O0` build with the guards off is what a compiler's own test suite needs when it
is testing the *absence* of a guard, and what a user needs when a guard fires on
code whose invariants live in a language the compiler cannot see (a hand-written
allocator's header, an MMIO register that is deliberately misaligned).

The flag is read by the driver and passed down as `ir::LoweringOptions::checks`.
It changes nothing above `ir`: `sema` records the same obligations either way,
because the record is what a *future* consumer (the shadow memory, an LSP's
hover, a re-check) reads. A build with the guards off is a build with the same
facts and fewer instructions.

## The scan

"An invariant that is not checked is a sentence in a document" (`ir.md`), and a
guard is an invariant of exactly that kind. `invariants.cc` therefore reads the
finished module and, when the caller says the module was built with checks,
reports `ir-unguarded-access` for a `load`/`store` whose pointer:

- is **not a constant**, and
- is **not the object itself** (an `alloca`), and
- is **not the storage the ABI handed us** (an `Argument`: a by-reference aggregate
  parameter), and
- has no **conditional branch immediately above it**, and
- has no test of that pointer in the chain of unique conditional predecessors
  (the chain, because one access can carry three guards in a row, and only the
  first of them compares the address; the immediately-above condition is the other
  half, and it is what stops an access emitted in a straight line from passing on
  the strength of some *earlier* guard's test).

The three escapes are not loopholes, and each is the same sentence: a check is
unnecessary exactly when the object under the address is one the **compiler — or
the calling convention — put there**. A constant address is a global (never null)
or a null constant, and an access through a null constant *is* refused earlier
(`sema-address-from-constant`, decision 11b of `casts.md`); an `alloca` is a frame
object whose address is non-null by construction; an `Argument` is the pointer a
by-reference aggregate parameter arrived as, which the caller had to make valid
(`arrays.md` decision 13). Everything else — a pointer parameter's *value*, a
load, a computed `getelementptr` — is an address the *program* obtained, and it is
guarded.

The scan's escapes are also why its rule is not "the address is tested somewhere
earlier": the guard tests the address as it *is*, and constant folding is allowed to
eat a test of a constant. Being honest about which shape survives folding is what
keeps the rule from failing on a correct module.

## What is deliberately not here

- **The shadow memory.** `memory-uninitialized`, `memory-dangling`, and
  `memory-out-of-object` for `foreign` provenance are not implemented and cannot be
  by this design: they need a write map keyed by allocation and a runtime that owns
  it, which is AddressSanitizer's shape and its cost. `memory.md` states the plan;
  this build states its absence in one place rather than pretending a bounds check
  covers a lifetime.
- **A `foreign` pointer's extent.** `p[i]` where `p` is a parameter is guarded
  against null and misalignment and *not* against its extent, and the reason is not
  that the compiler is lazy: the object `p` names is not in this unit. A pointer
  whose provenance is `object` but whose value flowed into a local
  (`let q = &a[0]; q[1]`) is the interesting middle case, and closing it needs a
  dataflow analysis that tracks a pointer's base and offset through assignments —
  a real analysis with a real proof obligation, not a third `if` in the lowering.
  It is named in `roadmap.md` rather than half-built here.
- **Recovery, handlers, and `-fno-sanitize-recover`-style modes.** A fired guard
  stops the program. A program that wants to *handle* a violation is asking for
  exceptions, which this language has not decided about.
- **Alignment for a `foreign` pointer that the *format* knows to be aligned.** The
  check is the type's alignment and nothing more; nothing tracks provenance-derived
  alignment, because that is the same analysis as the item above.

## How the claims above are checked

| Claim | Where |
| --- | --- |
| the record carries the slice's length as a bounds source | `tests/unit/sema/access_test.cc` |
| every guard is emitted from the obligation, never invented | `tests/unit/ir/checks_test.cc` |
| a guarded access reads the right value when the guard passes | the same file, on a generated module: the guard is a branch, so the check is "the access is still there and still reachable" |
| a null address, a misaligned address, and an out-of-bounds index each trap, and each prints its own site | `tests/unit/driver/build_test.cc` (a program that does each, one per case) |
| `-fno-check` removes every guard and the same program runs to the end | the same file |
| the guards keep working at `-O2`, which is the property `memory.md` decision 19 asks for | the same file, `-fcheck -O2` |
| an access with no guard in front of it fails the scan | `tests/unit/ir/invariants_test.cc`, the `UnguardedAccess` row |
| the scan runs only on a module the guards were asked for | the same file, and `tests/unit/ir/checks_test.cc` |

## References

- `-fsanitize=address` — 1/8-scale shadow memory, redzones, quarantine: the design
  the `memory.md` violations that are *not* in this file belong to.
- `-fsanitize=undefined` and `-fsanitize-trap` — the same split this file makes,
  between reporting and trapping, and the reason a diagnostic layer has to be a
  build flag.
- Rust's `debug_assertions`, `overflow-checks`, and bounds checks — a check that is
  a profile default, and a check that is contract.
- Go's bounds-check insertion and nil-dereference panic — the message that names
  the index and the length, which is what § *The runtime* is after.
- Ada's `Constraint_Error` — a violation with a defined outcome, the closest
  precedent for the value traps `runtime.cc` emits.
- MSVC `/RTC` (`/RTCs`, `/RTCu`, `/RTCc`) — the same table of checks, on a
  compiler whose build modes this file's flags are modelled after.
- P3471 / `_GLIBCXX_ASSERTIONS` / `_LIBCPP_HARDENING_MODE` — the C++ standard
  library's three-level check mode, and the argument for *levels* rather than one
  binary switch.
- `docs/architectures/memory.md` — the model, the obligations, and decisions 7, 15
  and 19, which this file implements.
- `docs/architectures/ir.md` — the runtime contract, the assumption list, and the
  guard table this file's first table repeats.
- `docs/architectures/codegen.md` — the flags, and why a trap arrives as a signal
  on one host and as an exception code on another.
