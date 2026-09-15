# Builtins — `src/builtins`, `__builtin_*`, and the operators that look like calls

The design record for the compiler-served operations: what `sizeof` is, what
`__builtin_trap()` is, why `exit` is neither, and why `assert` is a macro. It
sits under [`architecture.md`](../architecture.md) and beside
[`cli.md`](cli.md), and it exists because "builtin" is one word for four
different things that have opposite fixes when they go wrong.

**Status: design. Nothing here is implemented.** The v1 list at the end names the
prerequisite each item is waiting for, and two of them are waiting for language
surface ([`extern`](extern.md), `struct`) rather than for this design.

## The question, stated precisely

The request was "how do languages implement builtins like `assert`, `exit`, etc.,
and what is the most robust form". The honest answer starts by refusing the
premise that those two are the same kind of thing, because the design falls out
of the separation rather than the other way round.

`exit` is a **symbol in a library**. The C runtime provides it, it has an ABI, it
is called by `atexit` handlers, it can be replaced by a user, and `LD_PRELOAD` can
interpose it. A compiler that made `exit` a builtin would break every one of those
things, and it would break the promise this project is built on — if the checker
lets it pass, it must run — because the compiler would be emitting an instruction
where the program asked for a *call*.

`sizeof` is **not a runtime thing at all**. It has no address, no symbol, and no
effect, and its argument is *not evaluated*: `sizeof(x++)` must not increment.
That is a property of the grammar, not of a name lookup, and it is why `sizeof`
in C is a keyword and not a function called `sizeof`.

`assert` is a **macro**, because the information it needs — the text of the
expression, the file, the line — exists only before the compiler has a tree, and
because what it does on failure must be replaceable by the program.

So the design record has four conversations, not one.

## What the market does

| Language | Spelling | How it is implemented | The failure it had |
| --- | --- | --- | --- |
| C / GCC / Clang | `__builtin_*` | Reserved identifier namespace (the standard reserves `__` for the implementation), so a user name can never collide. Declared to the parser, typed like a function, lowered to an intrinsic or an instruction. `sizeof`/`_Alignof`/`_Static_assert` are **keywords**. | None at the level of naming. GCC's `-fno-builtin` exists because a *different* mechanism — matching user function names like `strlen` and replacing them — has to be switchable off. |
| Rust | `core::intrinsics::*` | The signature is **in the library**: a `#[rustc_intrinsic]` function (or an `extern "rust-intrinsic"` block), and the compiler replaces the *body*, not the name. The 2014 RFC that got here is explicit about why: intrinsics with a different ABI could not be used like functions, so every one of them was wrapped in a real `fn`, and `rustc` then paid to inline thousands of wrappers at `-O0`. | The wrappers. And the older `#[lang = "..."]` scheme, which allowed an intrinsic to be declared many times. |
| Zig | `@sizeOf`, `@trap`, `@panic` | A spelling that **cannot be an identifier**: `@` names a builtin, so it cannot be shadowed, cannot be declared, and cannot be passed as a value (it must be *called*). | Its documentation is a generated file that says, in the source, that it "must be kept in sync with the compiler implementation" — one list, two readers, drifting. |
| Go | `len`, `cap`, `append`, `panic` | Predeclared names in the universe scope, and `builtin.go` is a **fake package that exists only to be documented**: the file is never compiled. The compiler special-cases each name in the type checker and again in the lowering pass. | The special case is in two passes, and `builtin.go` is a third place that has to agree with both. |
| Swift | `Builtin.*` | A separate, deliberately ugly module of raw operations; the standard library wraps them in the nice API. | The wrapper layer is its own project (it is why Swift has `Builtin` and `_Builtin`). |

Three lessons, and they are the ones this document acts on:

1. **The spelling must be reserved.** `__builtin_` (C's rule, and the one a C
   programmer already knows) or `@` (Zig's). What must not happen is a *nice*
   name — `popcount`, `sizeof`-as-a-function — because then the compiler has to
   decide whether the user meant their own function or the compiler's, which is
   what `-fno-builtin` is: a switch for a decision that should never have been
   needed.
2. **The signature belongs to the compiler, in one place, and it type-checks
   through the normal checker.** Rust's current design puts the *signature in the
   library* and replaces only the body, and that is exactly the "one statement,
   many readers" shape this project uses everywhere. The alternative — a special
   case per builtin in `sema` — is Go's, and it means arity and type errors for
   builtins read differently from every other error.
3. **A builtin must be marked, not matched by name.** If the pipeline recognizes
   `clz` by its spelling four stages later, then a user function named `clz`
   is either broken or a trap. The marker is a property of the table entry.

## The decision that decides the rest: three families

| Family | Exists at runtime? | Where it is decided | Examples |
| --- | --- | --- | --- |
| **1. Runtime functions** | yes, as a symbol | `resolve` (a declaration), then the linker | `exit`, `abort`, `malloc`, `__assert_fail` |
| **2. Front-end operators** | **no** | the grammar, then `sema` (const-eval) | `sizeof`, `alignof`, `static_assert`, `offsetof` |
| **3. Intrinsics** | as an instruction or an LLVM intrinsic | `sema` types it, `ir` lowers it | `__builtin_trap`, `clz`, `add_overflow` |

`assert` is in none of them: it is a **macro**, and it is the only one of the
four that is, because it is the only one whose argument is *text*.

The three families have three different *fixes*, which is the practical reason to
keep them apart: a wrong runtime function is a declaration problem, a wrong
front-end operator is a grammar/const-eval problem, and a wrong intrinsic is a
lowering problem. One bucket would send every one of them to the same file.

### Family 1: not builtins, and the compiler must not pretend otherwise

Nothing is added to the compiler for these. What they need is the **declaration
form** (`extern fn`, which has landed — [`extern.md`](extern.md)), because `exit`
is declared once and linked, exactly like `printf`, and the language's promise (a
call is a call) is the point.

The rule this document sets, for the future: **when someone wants a library
function to be easier to call, the answer is the runtime header, not the builtin
table.** A builtin `exit` would be a compiler-invented function with no symbol;
`atexit`, interposition and C interop all die the moment it exists, and the
diagnostic that would explain the loss cannot be written.

`panic` sits here too. It is `print + trap` (or `print + abort`) in the runtime,
not an instruction, and a `-fcheck` build's failure path is a call to it.

### Family 2: front-end operators, because the argument is not a value

`sizeof` is **not** a call, and the difference is observable:

```
let n: usize = sizeof(x);      // x is not read
let m: usize = sizeof(x++);    // x is not incremented
sizeof(i32)                    // the argument is a *type*
```

So `sizeof` is a **keyword in the expression grammar**, and its operand is
recorded as one of two operand kinds: a type, or an *unevaluated* expression. It
is resolved and folded in `sema`, where the type store lives, and it produces a
constant of type `usize`. The consequences, each of which is a test:

- **Nothing about it reaches `ir`.** A folded `sizeof` is a literal by the time
  the lowering sees it, and the invariant scan does not have to know it existed.
  This is the same decision `sema` already makes for everything it decides.
- **A wrong operand is a normal `sema` diagnostic**, not a special message: a
  type that is not sized (once there is one), an expression with no type.
- **`sizeof` of an incomplete type is an error and not zero**, which is C's rule
  and matters for the same reason it matters there (a `sizeof` that silently
  answers 0 makes an allocation of 0 bytes look correct).

`alignof` is the same shape, answered from the same table (`sema/target.h` +
`TypeStore`), and `static_assert` is its statement-shaped sibling: a **keyword
statement** (C23's spelling) whose condition must be a constant, whose failure is
a diagnostic with a code and the message the user wrote.

`offsetof` is deliberately in this family and deliberately **not in v1**: it needs
`struct` first, and an `offsetof` written before the layout rules exist would be
a second implementation of them.

### Family 3: intrinsics, declared and marked

The mechanism, in one sentence: **a builtin is a row in a table, and the three
readers of that table are `sema` (the signature and the effect), `ir` (the
lowering) and the reference (the generated documentation).** No stage matches a
builtin by its *name*.

The seam this lands on already exists, and it is
[`resolve/predefined.h`](../../include/resolve/predefined.h): the names the
language binds before any source is read are one table and one *category*
(`Def::predefined`), so `resolve` binds them, `sema` types them and `ir` lowers
them by switching on which name it is rather than by matching on a spelling.
That is where a closed list of builtins goes — `trap`, `clz`, the overflow
intrinsics — and the family 1 names (`exit`, `abort`) stay where they are:
symbols an `extern` declaration reaches, with no builtin row at all.

```
                    ┌─→ sema: arity, types, effect (Diverges/Pure/WritesMemory)
BuiltinSpec (row) ──┼─→ ir:   id -> llvm intrinsic / instruction
                    └─→ reference: name, signature, one-line description
```

This is `command_spec`'s shape applied a stage earlier, and it is what makes the
following a *test* rather than an intention:

- every `BuiltinId` has exactly one row;
- every row has a signature, a lowering and a description (totality, both ways:
  a row with no lowering and a lowering with no row are both failing tests);
- `sema`'s handling of a builtin call goes through the **same call-checking path**
  as a user function, so `__builtin_clz()` and `clz()` produce the same
  *kind* of arity/type error.

A builtin has a **synthetic source file** registered in the `SourceManager`
(`<builtin>`), so a row has a `Span` like any declaration. That costs one line
and buys the two things a language server needs from a builtin: hover shows the
signature, and go-to-definition has somewhere to go that is not a null pointer.

### What the effect field buys

Every row states its **effect**, and the set is closed:

| Effect | Meaning | Who reads it |
| --- | --- | --- |
| `Pure` | no memory read, no write, no observable state | `ir` (reordering, CSE), `sema` (const-folding of arguments) |
| `ReadsMemory` | may read memory it did not write | `ir` |
| `WritesMemory` | has a side effect | `ir` (no reordering across it) |
| `Diverges` | **does not return** | `sema` (reachability), `ir` (`noreturn`) |

`Diverges` is not a nicety, it is a correctness field. `sema` already reports
"`main` returns `i32`, so it cannot reach the end without returning a value", and
that analysis must be right about:

```
fn i32 fail() -> i32 {
  __builtin_trap();        // does not return: no "missing return" here
}
```

A builtin whose effect is *unknown* is not a builtin this compiler states, and the
table has no default: the field is required, so adding a row is a decision about
correctness and not about typing.

The allowed values are also deliberately **only what we can prove**. There is no
"may read/write" catch-all, because a call that might do anything is the same as
saying `ir` may not optimize around it — which is the conservative answer and
should be spelled `WritesMemory`.

## The spelling

**`__builtin_` for family 3, and keywords for family 2.** Both are reserved, and
both are what a C programmer already expects: the standard reserves `__`-prefixed
names for the implementation, and `sizeof`/`alignof`/`static_assert` are already
reserved words in C.

Not taken, and why:

- **A nice name (`popcount`, `clz`) with a name-match in the pipeline.** This is
  what `-fno-builtin` exists to switch off. Two names for one thing, and the
  compiler has to guess which the user meant.
- **`@name` (Zig's spelling).** It is the strongest reservation available — a
  builtin cannot be shadowed, captured or passed as a value, because it is not an
  identifier. It is also the largest syntax departure in the whole language: it
  adds a token class, a grammar production for "builtin call", and a rule about
  `@` in every future construct. The reservation we already have (`__builtin_`,
  and the C rule that backs it) buys the same guarantee for nothing, and the "must
  be called directly" rule is enforceable in the checker one stage later — with a
  diagnostic that names the reason instead of a parse error.
- **A prelude written in `.mx` and parsed at startup (Rust's current shape).** It
  is the most attractive of the three: the signature is written in the language,
  checked by the real parser, and a builtin becomes an ordinary declaration whose
  body the compiler replaces. It is not taken *yet*, for one measurable reason:
  the prelude's spans point at a file that does not exist on disk, so every
  diagnostic about a builtin signature would render as `<unknown>:?:?:` — the one
  place in the compiler where a message would have no location. The table gets to
  the same end (one statement, three readers, totality tested) without that hole,
  and the prelude becomes worth it when builtins grow enough to need *generic*
  signatures (width-polymorphic `clz`, `add_overflow`). Recorded as a decision to
  revisit, not as a rejection.

## Const evaluation: one evaluator, not two

Family 2 is const-evaluated in `sema`, because the type store is what knows a
size. Family 3 is **not** const-evaluated in the front end.

The reason is the rule this project applies everywhere else: a second
implementation of one semantic is a second set of ways to be wrong. LLVM already
folds `llvm.cttz.i32` on a constant argument, and it does it with the rules
LLVM's own optimizer uses. A front-end folder for the same thing would be a
second answer to "what is `popcount(0)`" that has to agree with the first, and
nothing would force them to.

So: the front end folds what only the front end knows (a size, an alignment, a
type property) and hands everything else to the backend, which is where the
optimizer already is.

## The invariant scan, and the one builtin it forbids

`ir/invariants.cc` reads the finished module against a **closed permit-list**: no
metadata but `!dbg`, no `noalias` but a written `restrict`, no `inbounds`, no
`nsw`/`nuw`, no fast-math. Builtins are the most likely way for a forbidden thing
to enter the module, so each candidate is checked against that list before it is
allowed in the table:

- `__builtin_trap` → `llvm.trap`. Already emitted by `runtime.cc` for division and
  shift checks, so the *mechanism* exists; this only gives it a spelling.
- `__builtin_unreachable` → `llvm.unreachable`. **Allowed, with a plan**: the
  instruction is UB if reached, so a checked build (roadmap § 6) turns every one of
  them into a `trap` — which is what a sanitizer does to C's
  `__builtin_unreachable`. Without that plan it would be a hole in "if the checker
  lets it pass, it must run".
- `clz`/`ctz`/`popcount`/`bswap`/`rotl`/`rotr` → the matching `llvm.*` intrinsics.
  No metadata, no attributes beyond what the intrinsic itself carries.
- **`__builtin_expect` is not in v1**, and the reason is the permit-list rather
  than the feature: its only effect is `!prof` metadata, and `!prof` is not on the
  list. Adding it is a *decision about the invariant list* — a deliberate change
  with a test — and it should be taken when a real program shows it is worth it,
  not because a builtin list looks incomplete without it.

`llvm.assume` is in the same category and gets the same answer: it is an
optimizer *promise*, and a compiler that promises something false on behalf of the
user has no way to be right. Not in v1, and if it ever arrives it arrives with the
checked-build story first.

## `assert` is a macro, `static_assert` is a keyword

This is where the user's question lands, and the answer is that they are not the
same construct:

```
assert(x > 0)                  // a macro: text in, text out
static_assert(sizeof(i32) == 4, "i32 is 4 bytes")   // a keyword: a constant, checked
```

`assert` is a macro because the two things it needs are properties of the *source
text*: the spelling of the condition (`"x > 0"`, for the message a human reads)
and the location (`__FILE__`, `__LINE__`). The preprocessor already has
`__FILE__`, `__LINE__` and `__COUNTER__` (examples/pp/007), so the mechanism is
in place; the macro expands to a conditional call to a **runtime function**:

```
#define assert(cond) ((cond) ? ((void)0) : __assert_fail(#cond, __FILE__, __LINE__))
```

`__assert_fail` is family 1 — a declared symbol, not a builtin — which is what
makes `assert` overridable (a program can define its own failure path), keeps the
message format in the standard library where it can be changed without a compiler
release, and lets `NDEBUG` disable it the way every C program already expects.
`__func__` would be a fourth argument; it does not exist yet, and it is a
prerequisite worth naming rather than working around (`__FILE__`/`__LINE__` cover
the report).

`static_assert` is a keyword because it needs no text and no location: it is a
constant condition, evaluated in `sema`, and its two arguments are a constant
expression and a string literal.

## v1, with the prerequisite named

| Builtin | Family | Prerequisite | Why it is in v1 |
| --- | --- | --- | --- |
| `sizeof(T)` / `sizeof(expr)` | 2 | none | every allocation, every buffer bound |
| `alignof(T)` | 2 | none | the other half of the layout question, and `sema` already answers it |
| `static_assert(cond, "msg")` | 2 | none | the only way to assert about *types*, and the C23 spelling |
| `__builtin_trap()` | 3 | none | the primitive `assert` and `panic` will be built on, already emitted by `runtime.cc` |
| `__builtin_unreachable()` | 3 | the `-fcheck` story | the language must be able to state what it knows |
| `__builtin_clz/ctz/popcount/bswap/rotl/rotr` | 3 | none | arithmetic the *language* defines, so a program does not need a libc for it |
| `__builtin_{add,sub,mul}_overflow` | 3 | `*T` out-parameters (pointers: shipped) | checked arithmetic, which is where "the checker let it pass" gets teeth |
| `offsetof(T, field)` | 2 | `struct` | deferred, and named so it is not forgotten |
| `assert(c)` | macro | the runtime header (the declaration form has landed) | the user's example, and it is not a compiler feature at all |
| `exit`, `abort`, `panic` | 1 | the runtime: a header with the declarations, and the symbol behind them | a symbol, a header, and no compiler change — `mincc run` already reaches libc through `extern fn`, so what is missing is only the `minc+` runtime's own names |

Two entries are waiting on language surface rather than on this design, and saying
so is the point of the column: `offsetof` on `struct`, and family 1 on the
runtime's own header. Neither is a reason to add a builtin as a shortcut — that is
exactly the mistake this document exists to prevent.

## What is not on the table

- **`printf` and anything else in input/output.** Standard library, family 1. Not
  mentioned again here.
- **A builtin that a library could implement.** The test is: does it need
  information the program cannot have (a size, a bit operation, a trap) or
  operations the language cannot express? If not, it is a function.
- **A builtin that is a spelling for a cast** (`ptr_to_int`, `bit_cast`). A cast
  is a cast; giving one a function-shaped spelling creates a second grammar for
  the same operation.
- **`__builtin_expect`, `__builtin_assume`, anything carrying metadata.** See the
  invariant scan above; the answer is "when the permit-list changes, deliberately".

## Decisions not taken

1. **Matching a user function by name and replacing it** (GCC's `-fbuiltin`
   behaviour for `strlen`, `memcpy`). It needs an off switch, it makes a program's
   meaning depend on a name it did not choose, and it breaks interposition. The
   reserved spelling makes the whole question go away.
2. **A special case per builtin inside `sema`** (Go's shape). It works, and it is
   the reason Go's arity error for `append` does not look like its arity error for
   a user function.
3. **A documentation file kept in sync by hand** (Zig's `lib/std/builtin.zig`
   says so in its own source). The reference page is rendered from the table, the
   way `mincc help` is rendered from `command_spec`.
4. **A prelude in `.mx`, for now.** Right destination, wrong moment: it makes
   builtin signatures *generic-capable* and costs a diagnostic with no location.
   Revisit when the signatures stop being fixed-width.
