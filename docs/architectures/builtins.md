# Builtins — one table, five readers, and the four families that must not be one

The design record for the compiler-served operations: what is a *row*, who reads
it, what a row has to answer before it may exist, and why `assert` is in none of
it. It sits under [`architecture.md`](../architecture.md) and beside
[`extern.md`](extern.md), and it exists because "builtin" is one word for four
different things whose fixes are in four different places.

**Status: shipped, for the rows it has.** The table, both spelling classes, the
reserved namespace (a declaration and a `#define`), the binding, the call through
the shared argument machinery, the lowering with LLVM's own attributes, the
differential test and `mincc builtins` are all in the tree and tested; the bit
operations and `__builtin_trap` are the rows. What is *not* here is a family that
is not a row (`sizeof`/`alignof`/`static_assert` are grammar, the overflow family
is out-parameters, `offsetof` needs `struct`) and the availability field, which
arrives with the first row that needs it — both are in *What the implementation
found* and in the roadmap, with the reason.

This is the second pass. The
first one got the *taxonomy* right and stopped there: it named the families, chose
`__builtin_`, and said "a row in a table" without ever saying what a row **is**,
where the table **lives**, what it may **depend on**, how it is checked, or how a
builtin that only exists on one target, or only above a feature flag, or that must
fold to a constant, is expressed. That is the part a compiler is made of. This
revision keeps the four families and the `assert`-is-a-macro conclusion, and
replaces everything else with a data model, a set of invariants that can be
*tests*, and a plan in steps.

## The four families, and the one question that assigns one

| Family | Exists at runtime? | Decided by | Spelling | Examples |
| --- | --- | --- | --- | --- |
| **0. Compiler-emitted** | as an instruction or an intrinsic | the construct being lowered | **none — not writable** | the `memcpy` of an array copy, `llvm.trap` for a checked division, `sret` |
| **1. Library symbols** | yes, as a symbol | `resolve` (a declaration) and the linker | a declared name | `exit`, `abort`, `malloc`, `printf`, `__assert_fail` |
| **2. Front-end operators** | **no** | the grammar, then `sema` | a keyword | `sizeof`, `alignof`, `static_assert` |
| **3. Intrinsics** | as an instruction or an `llvm.*` intrinsic | `sema` types it, `ir` lowers it | a plain name (`clz`), or `__builtin_*` when it is not safe to make it one | `clz`, `rotl`, `__builtin_trap` |
| **4. Macros** | whatever they expand to | the preprocessor | an ordinary macro | `assert` |

**Family 0 is new in this revision and it is the one that keeps the table small.**
The compiler already emits operations nobody can spell: `memcpy`/`memmove` for an
object copy, `llvm.memcpy` for an array assignment, `llvm.trap` for a checked
division or shift (`src/ir/runtime.cc`), and `sret` for an aggregate return. They
are builtins in the only sense that matters to a compiler — *the compiler needed
an operation the language has no syntax for* — and they must **not** get a row,
because a row would make them writable and therefore a promise about their
semantics. GCC draws this line with the `IMPLICIT` flag ("when the builtin can be
produced by the compiler"), and this record draws it with the rule:

> A row exists only for an operation the **program** may name. What the compiler
> emits for a construct is a property of that construct, in the stage that lowers
> it, and it is checked there.

The assigning question, in order, one line each:

1. Is it something the *compiler* emits for a construct? → **family 0**, no row.
2. Does the information it needs exist only before the compiler has a tree (the
   text of an expression, `__FILE__`)? → **family 4**, a macro.
3. Is the operand a *type*, or an unevaluated expression? → **family 2**, grammar.
4. Could a library do it with the language as it stands? → **family 1**, a
   declaration and a symbol. This is the **default**, and the burden of proof is on
   anything that wants to leave it.
5. Otherwise → **family 3**, a row.

Question 4 is the one that stops the table from becoming a graveyard of things
that should have been functions: `exit`, `memcpy`, `strlen`, `sqrt`, `pow`,
`panic`, `print` all survive it and stay in the runtime, where they are
replaceable, interposable, and fixable without a compiler release.

## What the market does, specifically

| Compiler | The mechanism, as it actually exists | What it costs them |
| --- | --- | --- |
| **Clang** | `clang/include/clang/Basic/Builtins.def`: a macro DSL, one row per builtin (`BUILTIN`, `LANGBUILTIN`, `LIBBUILTIN`, `TARGET_BUILTIN`, `HEADER_BUILTIN`) — and the file says of itself that it is *"only documentation for the database layout"* and *"will be removed once all builtin databases are converted to tablegen files"*, which is the migration this record's "data, not macros" answer skips ahead to. The type is a **string** over a documented alphabet (`z` = `size_t`, `LLi` = `long long`, `*` = pointer, `I` = "required to constant fold to an integer constant expression"), and the attributes are a **letter alphabet** (`n` nothrow, `r` noreturn, `U` pure, `E` constant-evaluable by the front end, `i` implemented in compiler-rt, `f`/`F` the `__builtin_`↔libc prefix rules, `p:{n}`/`s:{n}` printf/scanf-like, `V:{n}` needs vectors of at least n bits). The same file's comment says the abbreviations *"must be kept in sync with the predicates in the `Builtin::Context` class"*. | The type is a **string**, so a typo parses and is only caught by a test; and the attribute letters are read by predicates written **in another file**, which is the drift the comment admits. |
| **GCC** | `gcc/builtins.def`: `DEF_BUILTIN(ENUM, NAME, CLASS, TYPE, LIBTYPE, BOTH_P, FALLBACK_P, NONANSI_P, ATTRS, IMPLICIT, COND)`. `FALLBACK_P` = "if the compiler cannot expand it, call the library function of the same name"; `IMPLICIT` = whether the compiler may *produce* it by itself; `COND` is a **runtime predicate** (`targetm.libc_has_function(...)`, `flag_isoc99`, `flag_openmp`) — availability is data, evaluated per target and per mode. Families are generated by macros (`DEF_GCC_FLOATN_NX_BUILTINS` expands one entry into `…f16/f32/f64/f128/f32x/…`), so one line of the table is seven builtins. | The surface of the table is a macro soup where one row can hide seven builtins, and a reviewer of a `.def` diff is reading an expansion rather than a definition. `-fno-builtin` exists because of mechanism 2 below. |
| **Go** | `cmd/compile/internal/ssagen/intrinsics.go`: a `map[intrinsicKey]intrinsicBuilder` keyed by **`(arch, pkg, fn)`**, where the value is a **function** that emits SSA. Helpers `addForArchs`/`addForFamilies`/`alias`; `add` **panics on a duplicate row**; version gates are conditionals (`if cfg.gopc64 >= 10`, `if cfg.goriscv64 >= 22`). The *signatures* live in the library, documented from a file the compiler never compiles. | The key is a **string** (`"runtime"`, `"memequal"`): a typo or a rename silently never matches, and the compiler has no way to notice. There is no totality check between "the intrinsics that exist" and "the builders that exist". |
| **Rust** | The signature is **in the library** — a real `fn` with generics and const parameters, marked `#[rustc_intrinsic]`, whose body the compiler replaces; a body may also be given as a *fallback* for backends with no dedicated implementation. Effects are attributes (`#[rustc_nounwind]`), and the module is `#![unstable(feature = "core_intrinsics")]` with the reason written out: *"intrinsics are unlikely to ever be stabilized, instead they should be used through stabilized interfaces"*. Implementations live in **four** places (MIR lowering, `rustc_codegen_ssa`, `rustc_codegen_llvm`, const-eval), and Miri re-checks them, with a distinction between a *valid* and an *equivalent* implementation of the documented spec. | The four implementations are four chances to disagree, and the spec is prose in a doc comment that a human has to keep true. The win — generic, width-polymorphic signatures through the normal type checker — is real, and it is the reason the prelude stays on the table here. |
| **Swift** | `include/swift/AST/Builtins.def`: `BUILTIN(Id, Name, Attrs)`, with **macro families** dispatching on an `Overload` (`.def` says `IntegerOrVector`, `FloatOrVector`, `Integer`). A "polymorphic" builtin must be specialized to an "overloaded static" one by the time constant propagation runs, and if it is not, the **compiler emits a diagnostic** telling the expert user so. For the `_with_overflow` family the file states an obligation in the comment: *"produce a compile-time error if we can statically prove that they overflow"*. | Builtins are visible in raw SIL and become normal calls after specialization, so the same construct has two representations and a window in which a wrong one can be observed. |
| **LLVM** (the other half of our table) | `llvm/IR/Intrinsics.td`: one row per intrinsic with properties — `IntrNoMem`/`IntrReadMem`/`IntrWriteMem`/`IntrArgMemOnly`/`IntrInaccessibleMemOnly`, `IntrNoReturn`, `IntrWillReturn` (**applied by default**), `IntrNoSync`/`IntrNoFree`/`IntrNoCallback` (**also default**), `ImmArg<ArgIndex<n>>` (the argument must be a constant), `Range<lower, upper>` (a checked precondition), `NoCapture`, `NoAlias`, `NonNull`, `Dereferenceable`. From these, `Intrinsic::getAttributes` derives the function attributes — and the file states the memory default explicitly: *"If no property is set, the worst case is assumed."* A family is **one row plus a type parameter** (`llvm_anyint_ty`, `LLVMMatchType`), and the row can name its own front-end spelling (`ClangBuiltin<"__builtin_stack_save">`). | Two default policies in one table (memory defaults to the worst case, `willreturn`/`nosync`/`nofree` default to true) — deliberate, but it means "absent" means something different in each column. |
| **Zig** | `@name` is **not an identifier**: a builtin cannot be shadowed, captured, or passed as a value, and it must be called. The earlier survey also found the reference and the implementation are one list with a comment telling the reader to keep them in step. | The strongest reservation available, at the price of a token class and a grammar production for "builtin call", plus a `@` rule in every future construct. |

### The seven lessons, each mapped to a decision here

1. **A spelling must be reserved, and the guarantee should be the checker's, not
   the lexer's.** `__builtin_` (C's rule: `__` is the implementation's) with a
   refusal for a user declaration of that prefix, instead of Zig's `@` — see
   *The reserved namespace*.
2. **A type written as a string is a type nobody checks.** The signature is a
   closed C++ type in the row, resolved by `sema` against the `TypeStore`.
3. **Effects belong to the row, and the row must not have a default.** GCC's
   `COND`, LLVM's worst-case default, Clang's letter alphabet: one field, required,
   closed set, no fallback — and the one place where a *library* already knows the
   answer (`ir`), the answer is **read, never restated**.
4. **Availability is data.** `(target, width, feature)` is a predicate on the row,
   like GCC's `COND` and Clang's `TARGET_BUILTIN`, so the refusal is a sentence
   naming the target instead of a link error or, worse, wrong code.
5. **A row has to declare what it costs the checker.** `ImmArg` and
   `Range<lower, upper>` exist in LLVM because an argument that must be constant,
   or must lie in a range, is a *language* fact; the table states it and `sema`
   enforces it.
6. **Never stabilize an intrinsic; stabilize a wrapper.** Rust's rule, adopted as
   the two-layer split below.
7. **The signature-in-the-library design is the destination, and the reason not to
   take it yet is a location, not a principle.** What it buys is generic
   signatures through the real checker; what it costs is four implementations and
   a spec in prose. This record gets *some* of the win — families, checked at the
   call site — without the sprawl.

## The module

New: `include/builtins/` + `src/builtins/` (one library, `minc_builtins`), with
**one dependency: `support`**.

```
                 ┌─→ resolve   binds the rows into the file scope
                 ├─→ sema      the signature, the effect, the width rule
BuiltinTable ────┼─→ ir        name → llvm::Intrinsic::ID, and nothing else
 (constexpr)     ├─→ driver    `mincc builtins`: the table, rendered
                 └─→ website   the reference page, generated from the rows
```

The first pass put the table in `resolve/predefined.h`. That is wrong, and it is
wrong for a reason worth writing down: `Predefined` is a list of three names whose
whole point is that `resolve` binds them and two stages switch on the enum. Builtins
are not "names the resolver has"; they are **data with four other readers** — the
type checker, the lowering, a CLI command and a documentation page — and a
documentation generator must not have to link the resolver to read a table. So:

- `builtins` may include `support` and nothing else. **Not `sema`** (a row cannot
  hold a `TypeId`: identifiers are store-local and target-dependent), **not `ir`**
  (a row cannot hold a `llvm::Intrinsic::ID`: `ir.md` makes `ir` the first stage
  that may include `llvm/*`, and a table that included it would make the whole
  compiler depend on LLVM to print a name), **not `resolve`**.
- The table is `constexpr` and static. No `Session`, no allocation, no state: the
  *runtime* side of a builtin is the compiler's own work in `sema`/`ir`, and a
  table of facts needs no lifetime.

## The row

```cpp
struct BuiltinInfo {
  BuiltinId id;                       // the identity; never a string
  std::string_view spelling;          // "clz" or "__builtin_trap"
  SpellingClass spellingClass;        // Prelude | Reserved -- who owns the name
  Signature signature;                // the DSL below; a family or a fixed shape
  Effect effect;                      // required, closed set, no default
  Lowering lowering;                  // what it becomes, as data
  Status status;                      // Stable | Internal
  std::string_view doc;               // the sentence the CLI and the page show
};
```

**Four fields the design had and the implementation does not**, and each is a
decision rather than a saving:

- **`definedness`.** Written down, it would have been a *second* statement of
  something the lowering already says: `clz` is total because the row passes
  `is_zero_poison = false`, and a rotate is total because its kind emits the `urem`
  first. A field beside the code that means the same thing is a field that can
disagree with it — and the differential test would then be comparing a claim
against a claim. So totality is expressed where it is *done*, and the row that
needs a guard gets a `Lowering::Kind` for it (`CheckedShift` is the shape a future
one takes), because a guard is code and not a label.
- **`availability`.** No v1 row is target-gated, and inventing the field first
  would mean inventing an architecture enum in a module that has no business
  naming architectures (`sema/target.h` owns those, and it is one stage up). It
  arrives with the first row that needs it, together with the mapping from
  `TargetInfo` — which is the rule this project already states for
  `AccessKind`: *a kind no input can produce is a kind no test can pin*.
- **`immArgs` and `range`.** The same rule: the rows here take runtime counts and
  runtime values, so a field for "this argument must fold" and a field for "this
  argument is in `[0, width)`" would both be enforced by code no input reaches.
  The checked-arithmetic family and the funnel-shift-with-a-literal are where they
  arrive. What the record *did* keep is the `MatchedWidths` field, because a row
  needed it on the first day: see below.
- **`doc` as a struct with a line and a paragraph.** One sentence is the contract
  (a row with none does not ship, and a test asserts it); the paragraph belongs to
  the page, and a page that needs a second paragraph gets one there.

Field by field, and what makes each one necessary rather than nice:

| Field | Why it exists | Who reads it | What it replaces from the survey |
| --- | --- | --- | --- |
| `id` | An enum is the identity, so no stage can match a builtin by spelling and no string comparison can silently fail. `None` is the "not a builtin" answer, so no second boolean is needed — the shape, and the spelling, `resolve::Predefined::None` already uses. | every stage, in a `switch` with no `default:` | Go's `map[string]` key, which cannot report a typo |
| `spelling` | The one place the text lives, so `__builtin_clz` appears in the tree exactly once. The prefix is reserved by a rule (below), not by the lexer. | `resolve`, the CLI, the docs, the preprocessor's reserved-identifier check | Clang's `Builtins.def` rows |
| `signature` | The type, in a closed form the checker resolves per target (below). | `sema`, the docs | Clang's type string; Swift's `Overload` families |
| `effect` | `sema` cannot ask LLVM (it may not include it), so the front end's decisions — reachability, const-fold of arguments, whether a call may be moved — need their own statement. **Required.** | `sema` (`Diverges` feeds the flow pass), `ir` (for family 1 and 3-not-mapped rows), the differential test | LLVM's memory properties; GCC's `ATTRS` |
| `spellingClass` | The two ways of owning a name, and it is a field rather than a naming convention because the *mechanism* differs: `Reserved` is enforced where the name is taken (a declaration, a `#define`), `Prelude` is an ordinary binding that shadows like `true`. | `resolve` (which names to bind), `sema` (which to refuse), `ir` (nothing — it reads the id) | Clang's `f`/`F` prefix rules, where the surface *is* the encoding |
| `matchedWidths` | The one place LLVM's **verifier** differs from its *optimizer*, and the difference is not UB but invalid IR: `llvm.bswap.i8` is refused ("bswap must be an even number of bytes"). A row that accepted a `u8` would be a program the checker passed and a module `llvm-as` rejects — the one thing this project exists not to produce. | `sema` (the sentence), `ir` (`Intrinsic::getDeclaration` at a width, and the differential test reads the same field) | LLVM has no equivalent — the rule is in the verifier and in the backend |
| `lowering` | What the row becomes, as **data**: an `llvm.*` name, or an instruction shape, or a runtime symbol — never a function pointer, so a row can be printed and diffed. | `ir`, the CLI, the docs | Go's builder closures (powerful, unprintable); GCC's `EXPAND` |
| `status` | The two layers (below), and the honest report of what is not promised. | `sema` (a gate), the CLI, the docs | Rust's `#![unstable]` |
| `doc` | A row with no sentence is a row nobody can use, and it is the page. | the CLI, the site, LSP hover | Zig's `lib/std/builtin.zig`; Go's `builtin.go` |
| *reserved:* `immArgs`, `range`, `availability` | Three fields the design states and the implementation has not earned yet, each with its arrival condition written down rather than a placeholder value that means "unused". | — | LLVM's `ImmArg`/`Range`, GCC's `COND`, Clang's `TARGET_BUILTIN` |

### The signature DSL, and why it is not a string

```cpp
// A type the row asks for, resolved against the store at check time.
enum class BuiltinType : std::uint8_t {
  Void, Bool, Char, Str,
  I8, I16, I32, I64, I128, Isize,
  U8, U16, U32, U64, U128, Usize,
  F32, F64,
  VoidPtr,
  // A hole the receiver fills: the *same* type as argument n (`LLVMMatchType`).
  MatchArg,
  // A hole the checker fills from the argument's own type, width and all.
  AnyInteger, AnyFloat, AnyObject,
};
```

Three properties, all of them tests:

- **Total and closed.** A `switch` over it with no `default:` means a missing case
  is a compile error; Clang's `"z"`/`"LLi"` alphabet is a string that has to be
  validated at runtime and whose typos are valid strings.
- **Target-dependent by construction.** `Usize` resolves through
  `TypeStore`/`TargetInfo`, so `__builtin_*`(usize)` is 32 bits on `i686-pc-linux`
  and 64 on `x86_64-pc-linux` **by the same table**, which is exactly why a
  `constexpr std::array<TypeId, N>` cannot be the table's type.
- **Families are one row.** A signature may be `Family::Integer` (one argument, one
  result, both the argument's own integer width) instead of a fixed list, and the
  checker resolves it to the concrete signature from the operand. LLVM's
  `llvm_anyint_ty`, Swift's `Overload`, GCC's `DEF_GCC_FLOATN_NX_BUILTINS`: one row,
  N concrete types. What differs here is that the N are *computed and enumerated by
  a test* (`allConcreteSignatures(id)`), so "every width of `clz` type-checks" is a
  test rather than a claim. And the signedness question has a decided answer:
  the operation is on the **bit pattern**, so `AnyInteger` accepts signed and
  unsigned alike and no conversion happens — which is what `ctlz` means anyway, and
  which the language can do without inventing an implicit conversion.

### The id enum, and the bound

```cpp
enum class BuiltinId : std::uint8_t { None = 0, Clz, Ctz, Popcount, Bswap, Rotl, Rotr, Trap };
static_assert(kBuiltinRows.size() < 0xFFFEu, "the table must fit the id type");
```

One `static_assert` in the table file, one test that every id has exactly one row
and every row a distinct id. The `None` slot is the "not a builtin" answer, so
`Def::builtin` needs no second field — the same trick `Predefined::None` uses.

The base type is the **smallest that holds the table**, not a wider one chosen for
headroom: `kBuiltinIdLimit` is what the table's `static_assert` checks against, so
the day a row makes the count exceed it, the compile error is one line from the
declaration that has to change. A type sized for a guess about the future is a
fact about nothing, and it hides the day the guess was wrong.

## Availability, stability, and the two layers

**Availability** is a predicate over what the compiler knows today. `TargetInfo`
currently carries the ABI widths and the triple (`sema/target.h`) and the CLI
carries `--target` (`command_spec.cc`) — and that is *all*: there is no
`--target-feature`/`-m` flag, so a row that needs one cannot be expressed yet.
Every v1 row is available everywhere, so the field does not exist: it arrives with
the first row that needs it, as an `Availability` struct over the values
`TargetInfo` already holds, extended by `cpuFeatures` **with the flag that names a
feature set** and not before. A field whose only value is "any" is a second
statement of the fact that nothing is gated.

What does exist, and is what makes the rule enforceable the day the field
arrives, is the shape of the refusal: a `sema` diagnostic whose sentence names the
target, the way `targetRefusal` already names a bad triple. A builtin that only
exists on one architecture must be a sentence at the call site, not a symbol the
linker cannot find.

**Stability** is two layers, and the split is Rust's:

- **`Status::Internal`** — the raw layer, not promised. `__builtin_trap` is the
  shipped example: nameable, and reported as internal by `mincc builtins`.
- **`Status::Stable`** — earned one at a time, with a test that pins the behavior.
  The six bit operations ship stable, and the reason is the totality rule above
  rather than a promise: every input has a defined answer (`clz(0)` is the width, a
  rotate's count is modulo the width), so there is no input a program can write
  whose behavior the language has not decided. A row that cannot say that stays
  `Internal`, and `mincc builtins` prints which is which.

The stable surface a program should be written against is **family 1**: a symbol
in the runtime, declared with `extern fn`, replaceable by the program. `assert`
already demonstrates the shape end to end, and it is why the answer to "make
`exit` easier to call" is a header and not a row.

## Defined behavior: LLVM may answer with poison; the language may not

This is the section the first pass did not have, and it is where the language's one
hard promise — *if the checker lets it pass, it must run* — meets LLVM.

LLVM's contract for many intrinsics is **poison or undefined** for inputs the
hardware cannot answer: `llvm.ctlz` of zero is poison unless the `is_zero_poison`
flag is false, `llvm.uadd.with.overflow` returns a value and a flag (so a call
site that ignores the flag is a program that carefully checked nothing), a shift
count past the width is undefined, and `llvm.unreachable` reaching is UB by
construction. A language that adopts the instruction without looking at the
contract has adopted the UB. So a row must answer the question — and the answer is
written **in the lowering**, where it is done, rather than in a label beside it,
because a label is a second statement of the same fact and can therefore disagree
with it. Four answers, and each has a shape:

| Answer | Where it lives in the row | What the language does |
| --- | --- | --- |
| **Defined for every input** | the operand that decides it: `TailOperand::I1False` for `clz`/`ctz`, `Lowering::Kind::RotateLeft` for a rotate's count | emit the intrinsic as-is — the row already gave it an argument it is defined for |
| **Defined, and the violation is a trap the language chose** | a `Lowering::Kind` that emits the guard (`CheckedShift` is the one the record names for the future) | the guard and the `llvm.trap` edge, so the poisoned form is not in the module at all — the same answer `sema`'s integer table already gives division by zero |
| **Control does not come back** | `Effect::Diverges` | emit it, and the call site is divergent for the flow pass — which is what makes `fn i32 fail() -> i32 { __builtin_trap(); }` not a missing return |
| **No defined answer exists and no trap is added** | no row. A builtin whose poison is the point is a builtin this language does not have | — |

`Effect::Diverges` is not a nicety: `sema` already reports "a body cannot reach its
end" and "`main` cannot reach the end without returning", and a row that diverges
has to feed that pass rather than be special-cased by it. The differential test
cross-checks the field against LLVM's own `IntrNoReturn`, so the table cannot
claim divergence the module does not carry, or the reverse.

`Diverges` is not a nicety: `sema` already reports "a body cannot reach its end"
and "`main` cannot reach the end without returning", and it must be right about

```
fn i32 fail() -> i32 {
  __builtin_trap();      // no "missing return" here
}
```

`Refused` is where `__builtin_unreachable` lands: it is UB if reached, so it is
**not** in v1, and when it arrives it arrives with the checked build turning every
one of them into a `trap` — which is what a sanitizer does to C's
`__builtin_unreachable`, and what makes it a promise the language can keep. The
same question, asked of the checked build, is asked of `llvm.assume` — whose only
effect is to *assert* something the compiler then exploits — and the answer is the
same: not until the checked build can hold it.

## The seam with `ir`: LLVM's own attributes are read, never restated

For a row whose `lowering` is an `llvm.*` name, `ir` does three things and no more:

1. **`llvm::Intrinsic::lookupIntrinsicID(name)`** — the name in the row is a
   *string*, so the mapping LLVM↔us stays data, and `ir` is where the two meet.
2. **`getOrInsertDeclaration(module, id, overloadedTypes)`** — the overloaded
   declaration, from the row's resolved type. The mangled name (`llvm.ctlz.i32`)
   comes from LLVM, not from us.
3. **Nothing about attributes.** `Intrinsic::getAttributes` derives
   `memory(none)`, `noreturn`, `speculatable`, `nocallback`, `nofree`, `nosync` and
   the rest from `Intrinsics.td`, and the declaration gets them that way. Adding
   `Attribute::NoReturn` by hand next to `llvm.trap` would be a second statement of
   a fact LLVM already made, and the one that could disagree.

What was *not* decided by this: the invariant scan's `function-attribute` row
(`ir/invariants.cc`) forbids `noalias`, `nonnull`, `noundef`, `dereferenceable`,
`align`, `signext`, `zeroext`, `inreg` — and deliberately **permits** `noreturn`
and `nounwind`, because they are the two attributes the language has a spelling
for. So `extern fn ! abort();` is a promise about a symbol this compiler did not
compile, and it is permitted on purpose: the type wrote it, and `!` is the
language's rule for it. Builtins do not get to widen that list: a row may not
introduce an attribute the scan forbids, and the scan is the test.

**The differential test**, and it is the one that makes this section checkable: for
every row whose lowering is an intrinsic, build a module that mentions it, and
assert that (a) the declaration's attributes equal what
`Intrinsic::getAttributes` says, and (b) **the row's `effect` agrees with LLVM's
memory property** — `memory(none)` ↔ `Effect::None`, `IntrReadMem` ↔ `Reads`,
anything else ↔ `Writes`, and `IntrNoReturn` ↔ `Diverges`. That is a test that
fails when LLVM changes its mind about an intrinsic, which is exactly when a
front-end's copied table becomes wrong.

## The two spelling classes, as shipped

The taxonomy above answers *which family*; this answers *who owns the name*, and
it is the field that decides whether a plain `clz` is possible at all:

| `SpellingClass` | The name | A declaration of it | `Status` | What it is for |
| --- | --- | --- | --- | --- |
| `Prelude` | `clz`, `ctz`, `popcount`, `bswap`, `rotl`, `rotr` | a local shadows it (with `-Wshadow`); the file scope's own declaration is a redeclaration | `Stable` | the language's own arithmetic: total, and therefore promised |
| `Reserved` | `__builtin_trap` | refused, and a `#define` of it refused too | `Internal` | the raw layer a runtime is written on: not promised, and not shadowable |

The invariant is a test, not a convention: **a `Prelude` row is `Stable` and has
`Effect::None`** (every input has a defined answer, which is exactly what makes it
safe to be an ordinary name), and **a `Reserved` row is `Internal` and starts with
the prefix**. A row cannot be both a name the language promises and one the
program may not take.

The `Prelude` class is what replaces name matching, and it is worth being explicit
about why: the name is *bound in the file scope* (`resolve/def.h`'s `Def::builtin`),
so a call resolves through ordinary lookup to a declaration of ours. Nothing scans
a program for a function called `clz` and replaces the call — which is GCC's
`-fbuiltin` behaviour, and the reason `-fno-builtin` has to exist. A program that
writes its own `clz` calls its own, and the only thing the language asks is that it
be a *block*-scope declaration rather than a file-scope one, where the prelude
already is. `true` has had exactly this treatment since the predefined table
existed; the builtin rows simply join it.

## The reserved namespace

`__builtin_` is reserved, and the reservation is enforced in **two** places,
because there are two ways to take the name away:

1. **A declaration** (or a parameter, or a local) whose name starts with
   `__builtin_` is refused: `sema-builtin-name-reserved`. This is stronger than C,
   where shadowing the prefix is undefined behavior that no compiler diagnoses —
   and it gives Zig's guarantee (a builtin cannot be shadowed or captured) without
   Zig's token class, its grammar production, or the `@` rule in every future
   construct.
2. **A `#define` or `#undef`** of the prefix is refused by the preprocessor:
   `pp-reserved-identifier`. A macro is the other way to make `__builtin_trap`
   mean something else, and the preprocessor *is* earlier than the table; a
   mechanism that can only be caught after expansion is a mechanism that will be
   caught too late.

And the third way, which is not a name at all: **a builtin is not a value.** It may
not be taken as a function pointer, stored, or passed — `sema-builtin-not-a-value`,
with the reason in the sentence (a row need not have an address; a row whose
`lowering` is an instruction has no symbol at all). A row that *does* have an
address — a future family-1-like row that emits a call to a runtime symbol — says
so in one field, and only then may it be taken.

Not taken: **matching a user function by name and replacing it** (GCC's `-fbuiltin`
behavior for `strlen`/`memcpy`). It needs an off switch, it makes a program's
meaning depend on a name it did not choose, and it breaks interposition — the three
reasons `-fno-builtin` exists. A reserved spelling makes the question disappear.

## The store: who reads this, and what the future readers need

The table *is* the store: no side registry, no build-time accumulation, nothing to
keep in step. What the future readers need on top of it is a **query surface** and a
**location**:

```cpp
namespace minc::builtins {
[[nodiscard]] std::span<const BuiltinInfo> all();                    // the enumeration
[[nodiscard]] const BuiltinInfo* lookup(std::string_view spelling);  // exact, one table
[[nodiscard]] const BuiltinInfo* lookup(BuiltinId id);
[[nodiscard]] bool isReservedPrefix(std::string_view spelling);      // sema + the pp
[[nodiscard]] std::vector<BuiltinType> concreteSignature(const BuiltinInfo&, ...); // families
}
```

- **The CLI** (`mincc builtins`) renders the rows: name, signature, effect,
  spelling class, effect, lowering, status, doc line. The shape is
  `command_spec`'s — one table, one rendering — so a row added without a sentence
  is visible immediately.
- **The site** generates the builtin reference from `all()`, so a page cannot
  describe a builtin that does not exist or miss one that does.
- **A future LSP** needs two things a table alone does not give: hover text (from
  `doc`) and a place to *go* (a definition). So the plan includes a **synthetic
  source file**, `<builtin>`, registered in the `SourceManager` with a line per row,
  and a `Span` in every `BuiltinInfo` that points into it. **Prerequisite, checked:
  `SourceManager` has no synthetic/virtual file support today** — `support/source`
  is files on disk. That is one small addition (a `FileId` whose text is the table's
  rendering, so line/column are real and a diagnostic about a builtin has a
  location at all), and it is what keeps a builtin diagnostic from rendering as
  `<unknown>:?:?:` — the single reason the first pass declined the
  signature-in-the-library design.
- **A future macro** that has to answer "what is the type of this builtin call"
  reads `lookup` + `concreteSignature`, which is the same pair the checker uses.

## v1, with the prerequisite named

| Builtin | Family | Prerequisite | Why it is in v1 |
| --- | --- | --- | --- |
| `sizeof(T)` / `sizeof(expr)` | 2 | none | every allocation and every buffer bound |
| `alignof(T)` | 2 | none | the other half of the layout question, already answered by `TypeStore` |
| `static_assert(cond, "msg")` | 2 | none | the only assertion about *types*, and the C23 spelling |
| `__builtin_trap()` | 3 | none | the primitive `assert` and `panic` are built on, already emitted by `runtime.cc`; `Diverges` |
| `__builtin_clz/ctz/popcount/bswap/rotl/rotr` | 3 | the `Trap` answer for the zero/out-of-range inputs | arithmetic the language defines, so a program does not need a libc for it. One row per family, `AnyInteger`, resolved by width |
| `__builtin_{add,sub,mul}_overflow` | 3 | out-parameters through pointers (shipped) | checked arithmetic; Swift's lesson is that the *obligation* has to be stated — an unchecked overflow here would be a language that pretends |
| `__builtin_unreachable()` | 3 | the `-fcheck` story | `Refused` until the checked build can hold it |
| `offsetof(T, field)` | 2 | `struct` | named so it is not forgotten; it needs the layout rules to exist first |
| `assert(c)` | 4 | the runtime header (`extern` landed; the declarations do not exist yet) | the user's example, and not a compiler feature at all |
| `exit`, `abort`, `panic` | 1 | the runtime: a header and the symbol behind it | a symbol and a header, no compiler change — what is missing is the runtime's own names |

## The plan, in steps

The order is the same one the rest of the pipeline used: **the type and the
identity first, the spelling second, the value last** — because a builtin that
exists before its row is checked is a builtin whose row nobody can test.

| # | Step | Files | What turns on | What is still refused |
| --- | --- | --- | --- | --- |
| 1 | **The table and the identity** | `include/builtins/builtin.h`, `builtin_id.h`, `builtin_table.cc`; `minc_builtins` in CMake | the id enum, the row struct, one row (`trap`), the query surface, the `static_assert` on the id bound, and the totality tests (one row per id, one id per row, every row has a doc line, every row is reachable in a table walk) | any user-visible builtin: nothing binds a row yet |
| 2 | **The reserved namespace** | `sema/check_stmt.cc` (declaration names), `pp/directives.cc` (`#define`/`#undef`), two new codes | a declaration and a macro of the reserved prefix are each refused, with tests | calling one |
| 3 | **Binding** | `resolve/def.h` (`Def::builtin`), `resolve/resolve.cc`, `resolve/predefined.cc`'s table shape | the rows are in the file scope, keyed by `BuiltinId` and not by spelling | typing a call |
| 4 | **The call, through the shared path** | `sema/check_expr.cc` (`checkCall`), `sema/builtins.cc` (the signature resolver), the new codes | arity and type errors for a builtin read exactly like a user function's; the width refusal (`bswap` of a `u8`), `Diverges` → the flow pass, and `not-a-value` | lowering |
| 5 | **The lowering** | `ir/declarations.cc` (the name→`Intrinsic::ID` map, the overloaded declaration), `ir/builtins.cc` | the intrinsic is emitted with LLVM's own attributes; the differential test (attributes **and** the effect cross-check); the invariant scan still clean | the families |
| 6 | **Families** | `sema/builtins.cc` (`concreteSignature`), the table rows | `clz` on every integer width, one row, with the enumeration test over the concrete signatures | target-gated rows |
| 7 | **The CLI and the page** | `src/driver/builtins_command.cc` + `command_spec`, the website generator | `mincc builtins`, rendered from the table; the site's reference page from `all()`; the synthetic `<builtin>` file and a `Span` per row | an LSP, which is a later project |
| 8 | **The rest of v1** | the table, one family at a time | `popcount`/`bswap`/`rotl`/`rotr`, then the overflow family, each with the answer its inputs need — an operand that makes it defined, or a guard that traps | `unreachable`, `expect`, `assume`: no row, on purpose |

## The tests

| Property | How it is a test |
| --- | --- |
| Every id has a row, every row has an id | the enumeration is derived from the table, and a test compares the two sets — the shape `allAccessKinds`/`allModuleAssumptions` already use |
| A row with no lowering and a lowering with no row are both failures | `ir`'s switch over `BuiltinId` has no `default:`, so a row without an arm is a **compile error**, and a test walks the table against the lowering id set |
| Every row has a sentence | a test over `doc.line`: empty is a failure, so "not documented" cannot ship |
| A builtin types like a function | one test per row that calls it with the wrong arity and one with a wrong type, asserting the *same code* a user function produces |
| Every row is reachable | the "one input per code" sweep the other stages use, extended to builtins: a `.mx` snippet per row, compiled and run |
| A family is complete | `concreteSignature` is enumerated and each is checked: every integer width of `clz`, signed and unsigned, with the result width asserted |
| A row is typed, not listed | the width rule is a test per row: `bswap` on 8/16/32/64/128 with the accepted and refused widths both asserted, and the signature text is derived from the row rather than written beside it |
| **The effect row agrees with LLVM** | the differential test above: attributes equal `Intrinsic::getAttributes`, and the memory effect matches LLVM's property |
| No stage matches a builtin by name | a source scan: the only file that may contain a `__builtin_` literal is the table (and the tests) — the anti-hardcode guard, and the reason the table exists |
| The reserved prefix cannot be taken | a declaration and a `#define` of it are each refused, one test each |
| Nothing forbidden enters the module | the builtin corpus runs under the existing `ir` invariant scan, with the check that a builtin that would need an attribute the scan forbids is not in the table |

## Hazards, and who paid for them

| Hazard | Who paid | The guard, and where it lands |
| --- | --- | --- |
| A builtin's *spelling* becomes an implicit library call | GCC's `-fbuiltin` replacing `strlen`, and `-fno-builtin` existing to switch it off | a reserved prefix and no name matching (decision 1) — step 2 |
| A signature kept in a second place drifts from the checker | Clang's own comment: the abbreviations "must be kept in sync with the predicates in `Builtin::Context`" | the signature is a closed C++ type resolved by the only stage that owns types (2, 4) — step 4 |
| A builtin that only exists on one target is a link error | every C project that ships `__builtin_ia32_*` behind its own `#ifdef`s | **still open by design**: no v1 row is gated, so the field would have no input to be tested with. It arrives with the first gated row, and the shape is fixed now: the refusal names the target, the way a bad triple already does |
| A *typo* in a table key that silently never matches | Go's `map[string]` key: a renamed function is an intrinsic that quietly stops being one | the key is an enum and the table is enumerated against the id set (1) — step 1 |
| An intrinsic's UB is inherited by the *language* | every front end that lowered `ctlz` without the zero question — LLVM answers poison, the hardware answers a value, and the two disagree | the answer is in the row's lowering, not beside it: `clz`/`ctz` pass `is_zero_poison = false`, a rotate reduces its count first, and a row that needs a guard gets a `Lowering::Kind` that emits one — step 5 |
| A front-end effect table goes stale | the same class as Zig's "keep in sync" comment, one file over | the differential test against `Intrinsic::getAttributes` (6) — step 5 |
| A builtin that cannot be named in a diagnostic | the first pass's own objection to a prelude: a signature whose span has no file renders as `<unknown>:?:?:` | the synthetic `<builtin>` file, and the prerequisite is checked rather than assumed (7) — step 7 |
| A macro takes the name away before the table is consulted | every `#define printf ...` joke, and every real one | the preprocessor refuses the reserved prefix (2) — step 2 |
| A builtin made *stable* by accident | Rust's rule, written in its own module docs: intrinsics are unlikely to ever stabilize, use the wrappers | `Status::Internal` is the default and the CLI reports it (6) — step 7 |

## What the implementation found

Five things the plan did not have, in the order they were found, because a record
that only shows the plan is a record nobody can trust.

**1. LLVM's verifier is a second contract, and it is not UB.** `llvm.bswap.i8`
does not have an undefined answer — it is *invalid IR*: "bswap must be an even
number of bytes". A `bswap` row that accepted every integer width would have been a
program the checker passed and a module `llvm-as` refuses, which is the one failure
this project is built not to produce. Hence `MatchedWidths` and the refusal at the
call site. Checked against the installed LLVM rather than remembered: `llvm-as`
rejects it, and it rejects it as a *verification* error.

**2. A totality rule, not just a totality claim.** The first shape of `clz`/`ctz`
was "LLVM's intrinsic, with the zero question left to the caller" — which would
have made the row inherit `poison` and made the language's `clz(0)` mean whatever
the backend felt like. The row now carries `TailOperand::I1False`, so the language
answers the width, which is what a language without UB has to do. `ctz(0) == 32`
for a `u32` is a test.

**3. A hardcoded command count, found because the new command was the seventh.**
`command_spec` had a fixed-size array with a literal bound; `mincc builtins` hit
it. It is derived from the table now, which is the bug the change exposed rather
than caused.

**4. The reserved prefix needed two enforcers, and the preprocessor was the one
missing.** `sema` refusing a declaration of `__builtin_*` is not enough: a
`#define __builtin_trap(x)` takes the name away *before* the resolver ever sees a
declaration. The preprocessor now refuses the prefix too — the same predicate, one
function, two callers, which is why it lives in `builtins` and not in either
stage.

**5. The `!` from `never.md` did the flow work by itself.** `Effect::Diverges` did
not need a new reachability pass: a call to a `Diverges` row is an expression of
bottom type, which is what `fn i32 fail() -> i32 { __builtin_trap(); }` already
needs. Two features, one mechanism, and the row is where the fact lives.

### The row, as shipped

The struct at the top of this record is the design; this is what the tree has,
printed by `mincc builtins` from the bytes the compiler actually uses:

```
$ mincc builtins

  name            signature                   spelling  status    widths            lowers to             what it is
  clz             (any-int) -> same           prelude   stable                      llvm.ctlz(, false)    the number of leading zero bits, and the width when the value is zero
  ctz             (any-int) -> same           prelude   stable                      llvm.cttz(, false)    the number of trailing zero bits, and the width when the value is zero
  popcount        (any-int) -> same           prelude   stable                      llvm.ctpop            the number of bits set
  bswap           (any-int) -> same           prelude   stable    even byte counts  llvm.bswap            the bytes of an integer in the opposite order
  rotl            (any-int, any-int) -> same  prelude   stable                      llvm.fshl after urem  the value rotated left, by a count taken modulo the width
  rotr            (any-int, any-int) -> same  prelude   stable                      llvm.fshr after urem  the value rotated right, by a count taken modulo the width
  __builtin_trap  () -> !                     reserved  internal                    llvm.trap             stop the program on the spot, where a debugger can see it
```

Every column there is a field of the row and nothing else: the `widths` column is
`MatchedWidths` and appears only where the rule is not `Any`, `lowers to` is the
row's `Lowering` rendered from its data (`after urem` is the `Kind`, not a comment
typed next to it), and the sentence is `doc`. A row added with no sentence or no
lowering is visible in this output, which is the point of rendering it.

The divergence from the design, in one line: `Lowering` carries the overload rule
and the trailing operand, `Signature` carries the width rule, and the three fields
the design reserved are absent rather than defaulted — the reasons are in *The
row*, and each one's arrival condition is a row.

## What is not on the table

- **Input/output, and anything else a library can implement.** The test is: does it
  need information the program cannot have (a size, an alignment, a trap, a bit
  operation) or an operation the language cannot express? If not, it is a function
  in the runtime, declared with `extern fn`, and it needs no row.
- **A builtin that is a spelling for a cast** (`ptr_to_int`, `bit_cast`). A cast is
  a cast; giving it a function-shaped spelling creates a second grammar for one
  operation.
- **`__builtin_expect`, `__builtin_assume`, anything whose only effect is
  metadata.** `!prof` and `llvm.assume` are not on the invariant permit-list
  (`ir.md`), so adopting either is a *decision about the permit-list* — deliberate,
  with its own test — and not a gap in a builtin list.
- **Name matching, of any kind.** See the hazard table.
- **A builtin that a future `struct` method would be better at.** When `struct`
  lands, `offsetof` is a front-end operator (family 2) and not a row, because it
  needs the layout rules the type store owns.

## Decisions not taken

1. **The table in `resolve/predefined.h`** (the first pass's answer). It makes the
   docs generator link the resolver, and it puts data with four readers inside the
   one stage that never reads most of it. A module of its own, depending on
   `support` alone.
2. **A signature in the library, in `.mx`, parsed at startup** (Rust's current
   shape). It is the destination — generic signatures through the real checker —
   and the reason it is not now is a *location*: a signature whose span has no file
   would make every diagnostic about a builtin render without one. The plan
   includes the synthetic file that removes that objection, so the decision is
   "later and measurable", not "no".
3. **Builder closures in the table** (Go's shape). A row that is code cannot be
   printed, diffed, rendered as documentation, or compared against LLVM's own
   attributes by a test. The lowering is data, and the code lives in `ir`.
4. **A `llvm::Intrinsic::ID` in the row.** It would put `llvm/*` in a module that
   `sema` and the CLI include, and `ir.md` makes `ir` the first stage allowed to
   include it. The row carries the *name*; `ir` is where the name becomes an id.
5. **A macro-generated family table** (GCC's `DEF_GCC_FLOATN_NX_BUILTINS`). One row
   and a resolver, so the family is data that a test can enumerate, and one row
   cannot silently expand into seven that nobody reviewed.
6. **A `kMaxBuiltins` in `support/limits.h`.** The table is compiler-owned static
   data, not input, so the bound belongs in the table's own `static_assert` where
   the id type is; a `limits.h` entry would suggest the count can be influenced by
   a program, which it cannot.

## References

- Clang, `clang/include/clang/Basic/Builtins.def` — the macro database, the type
  alphabet (`z`, `LLi`, `*`, `I`, `C`), the attribute letters (`n`, `r`, `U`, `E`,
  `i`, `f`/`F`, `p:{n}`, `V:{n}`) and the "kept in sync with `Builtin::Context`"
  note: <https://github.com/llvm/llvm-project/blob/main/clang/include/clang/Basic/Builtins.def>
- GCC, `gcc/builtins.def` — `DEF_BUILTIN` and its `LIBTYPE`/`BOTH_P`/`FALLBACK_P`/
  `NONANSI_P`/`IMPLICIT`/`COND` arguments, the class split, and the generated
  type families: <https://github.com/gcc-mirror/gcc/blob/master/gcc/builtins.def>
- Go, `src/cmd/compile/internal/ssagen/intrinsics.go` — the `(arch, pkg, fn)` key,
  the builder functions, `alias`, the duplicate-row panic, and the version gates:
  <https://github.com/golang/go/blob/master/src/cmd/compile/internal/ssagen/intrinsics.go>
- Rust, `library/core/src/intrinsics/mod.rs` — `#[rustc_intrinsic]`, the optional
  fallback body, `#[rustc_nounwind]`, the four implementation sites, the
  `#![unstable]` reason, and the *spec vs implementation* distinction:
  <https://github.com/rust-lang/rust/blob/master/library/core/src/intrinsics/mod.rs>
- Swift, `include/swift/AST/Builtins.def` — `BUILTIN(Id, Name, Attrs)`, the
  `Overload` families, the polymorphic/overloaded-static split with its
  "not fully resolved" diagnostic, and the stated overflow obligation:
  <https://github.com/swiftlang/swift/blob/main/include/swift/AST/Builtins.def>
- LLVM, `llvm/IR/Intrinsics.td` — the property vocabulary (`IntrNoMem`,
  `IntrReadMem`, `IntrWriteMem`, `IntrArgMemOnly`, `IntrNoReturn`, `IntrWillReturn`
  by default, `IntrNoSync`/`IntrNoFree`/`IntrNoCallback` by default, `ImmArg`,
  `Range<>`, `ClangBuiltin<>`), the worst-case default for memory, and one row plus
  a type parameter as a family: <https://llvm.org/docs/LangRef.html> and the
  installed `Intrinsics.td` of the LLVM release this compiler builds against.
- LLVM *Language Reference*, the intrinsics whose answer is poison —
  `llvm.ctlz`/`llvm.cttz` and their `is_zero_poison` flag, `llvm.uadd.with.overflow`,
  `llvm.unreachable`: <https://llvm.org/docs/LangRef.html#standard-c-library-intrinsics>
- Zig, *Builtin Functions* — the `@name` spelling that cannot be shadowed or passed
  as a value, and a reference generated from the compiler:
  <https://ziglang.org/documentation/master/#Builtin-Functions>
- Go, `builtin.go` — the predeclared names documented from a file the compiler
  never compiles, for `len`/`cap`/`append`: <https://go.dev/src/builtin/builtin.go>
