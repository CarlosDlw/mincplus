# Fixed-size arrays — `[N]T`, and the space `[]T` left open for slices

This record decides the first **aggregate** in the language. It is the first one
because it is the smallest: a count, an element, and a layout rule. It is also the
place where four of the five things every C-influenced language got wrong once are
in scope at the same time — the type losing its bound, a value type that looks like
a reference, the stack (or the heap) that nobody computed, and the ABI.

The constraint that decides most of what follows is the one `globals.md` and
`resolve.md` already fixed: **a unit is compiled alone, and this language is
unchecked by design** with a checked build as the reporting layer. An array is
therefore not "a pointer with a length somewhere else" (the answer that loses the
bound) and not "a heap object" (the answer that decides for the programmer), and
its layout has to be knowable *before* the program runs, because the compiler is
the one that writes the stack frame.

Nothing here is committed yet: `TypeKind::Array` is reserved and the syntax that
builds one does not exist (`type.h` says so, and `types.cc` refuses it by name).
That is the cheapest moment this decision will ever have.

## What the market did, and what each answer cost

| Language | The array | What it cost |
| --- | --- | --- |
| **C** | `T a[N]`, which **decays** to `T*` in almost every expression (C17 6.3.2.1p3: except for `sizeof`, `_Alignof`, `&`, and a string literal's initializer) | `sizeof a` is the size of a *pointer* as soon as `a` is a parameter, and `void f(int a[10])` means `void f(int *a)`. The bound is not in the type, so no compiler can refuse `a[10]` — the one check that would be free. Arrays are not assignable and cannot be returned, which is why `memcpy` is the language's array assignment |
| **C** (again) | `T a[n]` with a runtime `n` (VLA, C99 6.7.6.2; **optional in C11**, and MSVC never had it) | `sizeof` becomes a runtime expression, the stack frames are unbounded, and the failure mode is a silent stack overflow. Every successor refused it — Zig, Rust, Odin, Jai, Go and C++ (where GCC/Clang keep it as an extension) |
| **C** (again) | `T a[]` as the last member (flexible array member, C99 6.7.2.1p18) | `sizeof` deliberately *ignores* it, so the size of the object is unknowable from the type; it cannot be embedded, cannot be initialized, and the allocation is the programmer's arithmetic. The kernel still carries its own safer replacement for the same pattern |
| **C++** | `std::array<T, N>` | Fixes the decay (it is a struct), but a parameter still copies, `std::span` had to be added 10 years later to spell the view, and it inherits C's ABI: passing one by value is the aggregate-classification problem below |
| **Rust** | `[T; N]`, count part of the type, `Copy` only when `T: Copy` | Correct, and it needed **const generics** (RFC 2000) before generic code over `N` was expressible at all; a `for` over an array by value waited for an edition (2021) because the by-reference form was already shipped |
| **Zig** | `[N]T` (value), `[_]T` (inferred count), `[]T` (slice, `ptr+len`), `[*]T` (many-item pointer), `[N:x]T` (sentinel) | The best answer in the field, at the cost of **six** array-shaped types the reader must know. Its anti-bug core is that `*T` points at exactly *one* item, so C's `p + 1` on a single object is a compile error; ours cannot copy that without breaking the shipped pointer model |
| **Go** | `[N]T` value, `[]T` header (`ptr, len, cap`) | Two types that look alike and copy differently: assignment copies an array and aliases a slice, and `append` may or may not reallocate. The lesson is not the syntax, it is that the **copy rule has to be in the type and in the documentation**, which is `globals.md`'s argument for the module system in miniature |
| **Swift** | `Array` (heap, COW) — until **SE-0453** (Swift 6.2) added `InlineArray<let count, Element>` | A language that used tuples as fixed-size arrays for a decade, precisely because tuples give up indexing and iteration. SE-0453's design is the closest to this one: size = `stride(Element) × count`, alignment of the element, in-place literal initialization with **no** intermediate copy, and a count mismatch is a **compile error** |
| **Java / C#** | Heap objects with a runtime length field | Bounds are checked at run time because they are not in the type; C# needed `stackalloc` and `Span<T>` to get out of the heap |

Five failure modes recur, and every one of them is a decision in this record:

1. **The bound left the type** (decay) → `sizeof` lies, no static bounds check is
   possible, arrays stop being assignable. Decision 2 and 7.
2. **The length came from the run time** (VLA, `malloc(n)`) → unbounded frames,
   silent overflow, and an object whose size the compiler cannot state. Decision 4.
3. **One syntax, two copy rules** (Go's `[N]T` vs `[]T`, C's `char[]` vs `char*`) →
   the language teaches its own trap. Decisions 3 and 16.
4. **The object's layout was not the type's business** (FAM, trailing padding) →
   `p + 1` lands wrong, and `sizeof` and the allocator disagree. Decision 6.
5. **The ABI was assumed by the front end** (aggregates by value) → LLVM does not
   lower aggregates to ABI-compliant code, and every front end that assumed it
   shipped wrong code: Zig on MIPS struct returns, Crystal on AMD64/ARM64, Odin's
   ARM32 ABI, .NET's ARM64, OpenSmalltalk's AMD64, Inko's own C-ABI bug that
   appears only when optimizations are on. Decision 11.

## The decisions

| # | Decision | Why |
| --- | --- | --- |
| **1** | **The count is part of the type**: `[4]i32` and `[8]i32` are different `TypeId`s, interned structurally like every other type | The single decision that buys everything else. The bound cannot be lost (2), `sizeof` is constant (9), an out-of-range constant index is a diagnostic (7), and a function that takes a `[4]i32` cannot be called with a `[5]i32` — all for free, because the identity is already how this compiler compares types |
| **2** | **There is no decay, in any position, ever.** `&a[0]` names the element pointer and `&a` names the whole object; `a + 1` and `f(a)` for a `*T` parameter are errors, each with the fix in the sentence | Decay is the mechanism behind failure mode 1: it is why `sizeof a` on a parameter is the pointer's size, why C's type system cannot express "returns an array of 4", and why no C compiler can refuse `a[10]`. The cost is one `&` and one `[0]` at the call site |
| **3** | **`[N]T` is a value type**: assignment, argument passing and `return` copy the object; a future `[]T` slice is a **view** and copies nothing | Zig and Rust agree; C++'s `std::array` agrees; Go is the cautionary tale. The copy is also the language's *defined* answer — there is no shallow reference to leak — and the cost is real and stated: a `[1024]i32` parameter is 4 KiB of copy, which is what a lifetime/borrow layer and a lint are for, later |
| **4** | **No variable-length arrays.** The count is a compile-time constant, and a count that is not is refused by name, pointing at `alloc` | A VLA makes `sizeof` a runtime expression and the frame unbounded, which is a silent stack overflow (failure mode 2). The count is an ICE in the same sense `globals.md` decision 2 defines one |
| **5** | **`[0]T` does not exist**, and neither does a flexible array member: the count is ≥ 1 | An object of zero bytes has no address, which the model's *Objects* section has no rule for; an FAM makes `sizeof` a lie by construction. The "header plus trailing storage" pattern is `alloc` plus a pointer today, and a slice later. Reserved, not forbidden forever |
| **6** | **Layout**: `sizeOf([N]T) = N × sizeOf(T)`, `alignOf([N]T) = alignOf(T)`, with `sizeOf(T)` the element's **complete** object size — padding included | `p + 1` must land on the next element, and for an element that is a `struct` the complete size is the padded one (C's `sizeof(struct)` rule). Getting this wrong is failure mode 4 and shows up as a wrong answer, not a crash |
| **7** | **`a[i]` is an access with a known extent**: the obligation records the element type, the array's own object as the provenance, and the *extent* the type gives. A **constant** index outside `[0, N)` is a **compile-time error**; a runtime index is the checked build's range check | This is the check C cannot make and Zig/Rust/Swift make. It costs nothing to record, because the access obligation already exists (`memory.md`, `TypedFile::accessAt`), and the extent is a number the type already carries |
| **8** | **The literal has two forms, and there is exactly one inference mechanism**: `[...]` is *context-typed* like `1` and `1.5`; `T{...}` is a complete value that needs no context | A second inference (Rust's "an array literal has the type its shape suggests") is a second answer to "what type is this", and this language has exactly one: a literal is decided by its consumer (`sema.md`, *Literals*). The typed form is what makes `let a = ...` possible without inventing the second one |
| **9** | **The length is exact**, and `[_]` is the *only* count inference — **outermost only** | C's "fewer initializers means zero-fill the rest" is how an array ends up half-written and nobody notices; Swift SE-0453 makes the same call (a count mismatch is an error). `[_]` is the mark that says "the count comes from here", and outermost-only is what keeps a ragged literal from having a type at all |
| **10** | **`[value; N]` is the fill form**, and it is checked against the count; an empty list is refused | The explicit way to write zero-fill (or `[1; 8]`), and the *only* one: `[4]i32{}` has no meaning, and a sentence says so instead of guessing |
| **11** | **Arrays do not cross the C boundary by value**: an array-typed parameter or return on an `extern` declaration is refused, and C's `T a[N]` parameter maps to `*T` | There is no C ABI for an array parameter (it decays — decision 2's counterpart on the C side), and for aggregates there is no LLVM lowering that is ABI-compliant: classification into INTEGER/SSE/MEMORY, `byval`, `sret`, two-eightbyte flattening, and an IR-level `memcpy` into ABI-typed slots are the *front end's* job. That layer is `src/cinterop`'s, with its own record — this decision is the seam, and it is why the promise "if the checker lets it pass, it must run" survives |
| **12** | **An array's storage type is built from the element's storage type**: `[N]bool` is `[N x i8]`, and an element load is normalised exactly as a scalar `bool` already is | `ir.md` decision 5: `bool` is `i1` as a *value* and `i8` as an *object*. An array of `i1`s would be a bitfield, which is a different feature, and would make `sizeof([4]bool)` disagree with `alignOf` |
| **13** | **A by-value `[N]T` is a caller-allocated copy carried by a pointer**: the caller `memcpy`s into a temporary and passes `ptr`; the callee's binding *is* that pointer, so `&a` needs no spill and the callee never writes the caller's variable. A `[N]T` **return** is the same shape with the destination passed as an implicit leading parameter (`sret`). LLVM's first-class aggregate is *not* used, and the *stable* ABI is still not promised (11) | The revision this record owes its first draft. A first-class `[N x T]` parameter puts a type whose size the source chose into the module's signature — for `[1<<20]i32` that is a megabyte-long type in every call, every debug record and every function pointer — and LLVM's own convention for aggregates is *not* the ABI, which decision 11 already says. The pointer-plus-copy shape is what clang emits for a C aggregate the ABI puts in MEMORY, so the internal convention and the C one are the same *shape*, and `cinterop`'s remaining work becomes the attributes on the parameter (`byval`, `align`, `readonly`) instead of a second lowering. It also removes the one place an aggregate would have had no address: `a` is a place for the whole call, which is what `&a`, `a[i]` and `-g` all need |
| **14** | **A local object larger than `kMaxStackObjectBytes` is refused at its declaration**, naming `alloc` and file scope as the alternatives | A frame the compiler writes has to be a frame the compiler can bound (failure mode 2, one step further than VLA). The limit lives in `support/limits.h` with the project's `static_assert` keeping the human-readable form in step |
| **15** | **A file-scope array initializer is a value, folded by the ICE**: `zeroinitializer` for the zero fill, a `ConstantDataArray` where the element type allows it, otherwise a `ConstantArray` — and **never** LLVM's `constant` | `globals.md` decision 2 and `memory.md` decision 15 already said the first half; decision 33 of `ir.md` and the scan say the second. An array changes nothing here, which is the point of an ICE that folds *values* and not statements |
| **16** | **A string literal is not an array**, and `str` is not `[]u8`: `let b: [4]u8 = "abc";` is refused | The implicit step from a string literal to an array is where C's `char*`/`char[]` confusion starts (failure mode 3). The relationship is real and reserved, not denied: `str` *is* a slice with a sentinel — Zig's `[:0]u8` — and when slices arrive, `str` is one of them rather than a parallel concept |
| **17** | **`[]T` is reserved for the slice and parses today, refused with a sentence**; `..` is reserved for slicing | The seam has to be a *decision* rather than a guess, because a slice is `{ptr, len}` — an aggregate value, which means slices land after `struct` and after the `cinterop` ABI layer, in that order. Reserving the spelling now also means no `.mx` file can use `[]T` for something else in the meantime |
| **18** | **A pointer to an array is a pointer like any other**: `*[4]i32` steps by `sizeOf([4]i32)`, `(*p)[i]` is an element, `p[i][j]` is an element of the `i`-th array | No new rule: the pointer model already says a pointer steps by its pointee's size, and the pointee is now allowed to be an array. It is what makes `&a` useful and what a `struct`-of-arrays and a 2D table need |
| **19** | **The count's identity is its value, not its spelling**: `[04]i32`, `[0x10]i32` and a future `[N]i32` (with `const N = 16;`) are **one** `TypeId`, and so will `[16 / 2 + 8]i32` be when a count becomes a constant expression | The store compares structure, so a count compared as *text* would hand two ids to one type — the exact failure `type.h` says the interned store exists to prevent, and the one Rust had to forbid up front: RFC 2000 makes *structural equality* a **requirement** on a const parameter precisely so `[T; N]` has a decidable identity. It also fixes what the count *is*: a folded `u64`, always ≥ 1, read once by `support::parseIntegerLiteral` with the checker's own base rule, so `[010]i32` is the leading-zero error it is everywhere else in the language, and the decimal spelling of a count is not a second number syntax |
| **20** | **A type whose size is not a number is not a type**: `N × sizeOf(T)` is a checked multiply, and a product that does not fit `size_t` is refused **at the count** | A layout, an `alloca`, a `memcpy` length and a constant all need that number, and a wrapped one is a *wrong answer*, not a crash — the failure mode this record is organised against. The check is at the count because that is where the information and the sentence are; Rust's `size_overflow` is the same check in the same place |
| **21** | **The store gains a second question, and it is not a rename**: `isObject(id)` — "has an object representation, so it can be a binding, a parameter, an element, a field, a copy" — while `isScalar(id)` keeps meaning "scalar-shaped". `isObject` is deliberately the *narrower* one: `isScalar` is true of a deferred literal and `isObject` is not | `isScalar` was written as the "can be stored and passed" predicate and had **no call sites** when this landed — which is why splitting it is free, and why the split has to be made now rather than after twenty sites started relying on the wide reading. The narrowness is not cosmetic: `isScalar` is true of `IntLiteral`, so a predicate built on it alone would accept `[3]<integer literal>` and build a type whose `sizeOf` is 0 — the test *An array's element must be an object* is what pins that, and it is the one assertion that would fail if `isObject` were spelled `isScalar` |
| **22** | **The `[...]` form is a *deferred aggregate type*** — a `TypeKind` carrying the count whose element type is not decided yet — decided by its consumer exactly as `IntLiteral` already is; the typed `T{...}` form is never deferred | This is decision 8 made implementable: "one inference mechanism" only holds if the context-typed literal is a *type* like every other deferred thing. The alternative — an "expected type" threaded beside the tree — is a second typing path, and it is exactly the second answer to "what type is this" that decision 8 refuses. It is also what makes nesting work without a rule: deciding `[[1,2,3],[4,5,6]]` at `[2][3]i32` *is* deciding each inner literal at `[3]i32` |
| **23** | **An array's definite assignment is object-level**: an initializer or a whole-object `=` assigns the object; an element store is a legal *write* that assigns nothing; a read of the object — or of an element — requires the object assigned. The diagnostic names `[0; N]` or an initializer | The flow layer's places are names, and per-element precision (rustc's answer, a bounded element-set per binding) *accepts more programs*, so adding it later is a compatible extension and refusing now is not a rule that has to be un-taught. What it refuses is `let a: [4]i32; for i { a[i] = f(i) }` followed by a read of `a` — and rustc refuses the same loop, with the same one-line fix |
| **24** | **`const` is a rule about *places*, and an element is a place in the binding**: `TABLE[0] = 1` and `&TABLE[0]` are both refused, by the rule that already refuses `&c` (`sema_address_of_const`) — the second is not a new rule but the same one reaching one level deeper. The object may still be written through a pointer that arrived from elsewhere, which is why the module still emits no `constant` (15, `globals.md`) | Every language's answer for a const aggregate's *element* is const (C's `const int a[3]`, Rust's immutable place, Zig's `cannot assign to constant`), and the existing code already refuses `&c` for the reason `memory.md` gives: an address is a way to write the name. Extending it to an element place is what keeps `const` from meaning something weaker for aggregates than for scalars — the alternative is a `const` table whose bytes the same unit may rewrite, which is a footgun invented by us and in no other language |
| **25** | **No operator accepts an array except `=` and the subscript.** `==`, `!=`, the relational, the arithmetic and the bitwise operators are each refused *by name*, with the element-wise alternative in the sentence | C's `==` on two arrays compares addresses (through decay) and is one of the language's oldest bugs; element-wise `==` over a `[N]f32` is a NaN question nobody has decided here; and `==` for an aggregate has to be decided for `struct` fields at the same time, in the record that introduces them. `=` is not an operator question: it is the copy decision 3 already made |
| **26** | **The access obligation gains the extent**: the `a[i]` form records the *array* type it indexes (so the `getelementptr` has an element type) alongside the element type it produces, and the count the type gives. `-fcheck`'s extent guard reads it; a `*p` and a parameter's `a[i]` stay `foreign` | This is the one check C structurally cannot make — its array is a pointer by the time anyone could check — and the one `runtime.cc`'s guard was written for ("an `object`-provenance extent", `ir.md` decision 27). It is also why `a[i]` is *stronger* than `*p` in the same program: the object is named here, so the module can state its bounds |

| **27** | **A written-out fill is bounded, and a zero fill is exempt**: a non-zero fill may not exceed `kMaxFillElements` (2²⁰), while `[1 << 40]u8{0; …}` compiles to one `zeroinitializer` | LLVM's `splat` is a *vector* expression, so an array fill has to be materialised one `Constant*` per element — and the count of a fill is a number in the **type**, which the source can make arbitrarily large in ten characters. The exemption is not a courtesy: a zero fill is one constant whatever the count, so the bound would be refusing work the compiler does not do. What the bound buys is that the compiler's cost is a function of the source and not of a number in it |
| **28** | **The frame is bounded too**: an object this function would place in its own frame may not exceed `kMaxStackObjectBytes` (16 MiB), refused at the declaration, at the two places a slot is created (a local binding, and a by-value argument's caller-side copy) | Decision 14 promised this and the promise is worth restating where it is enforced: without it `let a: [1 << 40]u8;` builds a terabyte `alloca`, the backend emits a stack subtraction that size, and the failure is a segfault at the function's first instruction — a bug handed to a debugger instead of to a diagnostic. The by-value *parameter* has no slot of its own (13 says its storage is the pointer it arrived as), which is why the *caller's* copy is the second call site and not the callee's binding |

| **29** | **The lexer gains one character of lookbehind**: `.` in front of a digit starts a fraction only when the character before it is not a `.` | `a[1..2]` and `.5` are both legal spellings of different things, and which one a `.` is belongs to a character the token starts *after* — so the decision cannot be made by longest-match alone. Without it, `..` is unlexable under every input (the second dot becomes `.2`), and the spelling a slice will be taken with would not have existed: a reserved spelling that the lexer cannot produce is not reserved, it is missing |

## The surface

```minc
// A type
[N]T                  // an array of N T;  N is an integer literal, ≥ 1
[4]i32                // and [8]i32 is a different type
[2][3]i32             // two arrays of three — the outer is written first
*[4]i32               // a pointer to an array of four
[4]*i32               // an array of four pointers
[]T                   // RESERVED: a slice. Parses, refused by name for now

// The two literal forms
[1, 2, 3]             // context-typed: what the consumer needs it to be
[0; 64]               // the fill: 64 zeros, checked against the count
[[1, 2, 3], [4, 5, 6]]// nested; everything inside takes the type from outside

[3]i32{1, 2, 3}       // the typed initializer: a complete value, no context
[_]u8{1, 2, 3}        // the count comes from the list, visibly, outermost only
[2][3]i32{[1, 2, 3], [4, 5, 6]}
[64]u8{0; 64}         // the fill in the typed form, checked against the type

// Bindings, either way
let a: [3]i32 = [1, 2, 3];
let b = [3]i32{1, 2, 3};
const TABLE = [_]i32{1, 2, 3};          // file scope, an ICE
let grid: [2][3]f32 = ...;              // filled by statements, or by a literal

// Access, and the error decay would have allowed
a[0] = 1;             // an element of the object itself
let p: *i32 = &a[0];  // the element pointer
let q: *[3]i32 = &a;  // the whole array
```

**`let a = [1, 2, 3];` is an error, and the error teaches both fixes:**

```console
error[sema-literal-type-unknown]: the type of this array literal is not known here:
  annotate the binding (`let a: [3]i32 = [1, 2, 3];`) or name the type in the literal (`[_]i32{1, 2, 3}`)
```

**Reading `[N]T{...}` costs one scan and no backtracking**, and it is worth
writing down because it is the kind of thing that becomes a reparse later. The
group that can be a count is exactly `[` number `]` (or `[_]`), and then the
decision is whether a **type run followed by `{`** comes after it: the scan walks
the run -- a `[N]` group as one step, so `[2][3]i32{...}` is seen -- and answers
`true` only on `{`. Every other continuation is the list reading: `[1][0]` is a
one-element list being indexed, because there is no brace after its type run.
Nothing else in this grammar puts `{` after type tokens, so the two readings can
never both be valid. The scan is bounded by the type run, and a run ends at the
first token that cannot be part of a type, which in a real program is a handful.

**The general `T{...}` is not recognized**, and that is a deliberate limit of
this step rather than an oversight: a typed initializer is only recognized when
its type starts with `[N]`, the array constructor. `i32{1}` and a future
`Point{...}` are therefore *parse* refusals today and not the sentence about
braces being for aggregates — that sentence needs a type name followed by `{` to
be unambiguous with a block, which is a question the first non-array aggregate
brings with it.

The two forms and `[_]` are deliberately *not* additional grammar for `let`: the
binding keeps `name: type`, and the typed form is usable anywhere an expression is
— `take([1, 2, 3])`, `return [2]i32{...};`, `a[i] = [2]i32{...}[0];`.

**The context form is not a deferred *type*.** A number literal is deferred
because it flows through operators that must find the common type of operands
nobody has typed yet; an array literal cannot be an operand of any operator
(decision 25), so the only thing it can meet is a consumer that already has the
type. `checkExpr`'s `expected` *is* that type, and `checkArrayLiteral` uses it
directly. No second typing mechanism, no new `TypeKind`, and no way for two
answers to coexist — which is the reason the implementation of step 7 came in
smaller than this record first planned.

## Where it lands

| Stage | Change |
| --- | --- |
| `lex` | nothing: `[`, `]`, `;` and integer literals are already tokens. The count's *spelling* is read once, by the same literal reader `#if` and the checker use |
| `parse` | `typeRunLength` and `parseType`/`parseTypeAndName` accept a bracketed count between the `*`s and the words; a primary expression may start with `[`, with the one-token rule above. Two new syntax kinds (`ArrayLiteral`, `TypedInitializer`), and a `Type` node that keeps its brackets and count as children so the tree stays lossless |
| `ast` | `TypePart` gains the array shape: `isArray` plus the count's spelling, in source order, so `*[4]i32` and `[4]*i32` are different runs of parts |
| `validate` | the structural rules: a count is present, the element type is present and not `void`/`!`/a function, a typed initializer's braces are not empty, a `[]` is the reserved slice spelling (and says so) |
| `resolve` | nothing: an array is a type, and a type is not a name |
| `sema` | `TypeStore::arrayOf(element, count)`; `Type` gains the count and the interner's hash/equal include it; `sizeOf`/`alignOf`/`spelling` for `Array`; the count reader (an ICE, literal today); the two literal forms (`[...]` deferred to its consumer, `T{...}` checked against its own type); exact length, `[_]` outermost-only, the fill, the element rules; `a[i]` as an obligation with a **statically known extent** and the constant-index check; the stack-object limit; and the ICE's array case |
| `ir` | `llvmType([N]T)` = `[N x llvmType(T)]` and `storageType` = `[N x storageType(T)]` — the `case` that today refuses arrays by name becomes a body; alloca/alignment; element access through the recorded obligation; `&a` as a `getelementptr` to the object; whole-object copy for `=`, arguments and `return`; global arrays (decisions 15, 33); `-g` gains `DICompositeType` + `DISubrange` so a debugger can show the elements |
| `backend` | nothing: data is data, and the object writer already emits `.data`/`.bss` |
| `cinterop` | the boundary's array rule: C's `T a[N]` parameter is `*T`; a `[N]T` parameter on `extern` is refused by name (decision 11). The aggregate classification is that record's |
| `driver` | nothing: `mincc ir` already prints the module, and `sizeof` (the operator that makes a layout visible) is its own item |
| `support/limits.h` | `kMaxStackObjectBytes` and `kMaxFillElements`, each with the human-readable form and the `static_assert` the project's rule requires; the two bound a frame and a written-out fill, which is why they are separate numbers and not one |
| docs | a language page for arrays (`arrays.md` beside `pointers.md`), plus the feature checklist, and `types.md`'s conversion section saying that arrays do not convert |

### What has landed

Steps **1–9** are implemented and tested: the type and its interning, layout and
spelling (`type_test.cc`), the syntax run in the grammar and the reader
(`array_test.cc`, `initializer_test.cc`), element access with the constant bounds
check, the by-value shape in **both** directions (a caller copy for a parameter,
`sret` for a return — `lower_test.cc` pins both), both literal forms with the
exact length, the fill, `[_]`, the aggregate **value record** (a list or a splat,
recursive, with the element's own type), the constant `ir` emits for it
(`global_test.cc` in both stages), and the page a reader starts from. `mincc run`
on `examples/014_arrays.mx` is the end-to-end proof, and it now writes its tables
at file scope as well as inside `main`.

Step **10** landed with it: both reserved spellings now have their sentence.
`[]T` parses and is refused by the reader of types, and `a[1..2]` is
`parse-reserved-range` at the operator -- **one** message, where before it was two
about the bracket. The `..` needed one character of lookbehind in the *lexer* to
get there: `.` in front of a digit is a fraction (`.5`, `1.5`) unless the
character before it is another `.`, so `a[1..2]` is `1`, `.`, `.`, `2` and not
`1`, `.`, `.2`. Without that, the operator a slice will be taken with had no
spelling at all -- which is exactly the failure mode this step exists to prevent.

Two bounds landed with step 8, and they are decisions rather than details
(27, 28): a **frame** object may not exceed `kMaxStackObjectBytes` (16 MiB) and a
non-zero **fill** may not exceed `kMaxFillElements` (2²⁰). Both are enforced in
`ir` -- the stage where the frame and the constant are the compiler's own cost --
with one message each, and both stop the walk so that one refusal cannot become a
trail of `ir-internal` complaints about symbols that were never emitted.

## The implementation, in the order it lands

This is the part the market's bug lists are actually about. Every language above
shipped arrays; the ones that suffered shipped them as *one* change — a type, a
literal, a lowering and an ABI in the same release — and the failures were the
seams between those four, not any one of them. So this lands as ten steps, and the
property each step keeps is the same: **`make ci` is green, every earlier
behaviour is untouched, and exactly one thing is still missing.** A step that
turns on a literal before its type exists is a step where a wrong identity is
first noticed inside a folded constant, which is the least legible place in the
compiler to notice anything.

The order is *type, then value*: steps 1–5 make `[N]T` a type with a layout and a
lowering, and steps 6–8 give it values. Nothing folds a constant before its
identity and its size are pinned by tests of their own.

| # | Step | Files | What turns on | What is still refused |
| --- | --- | --- | --- | --- |
| 1 | **The type and its layout** | `sema/type.h` (the count in `Type`), `type_store.h`/`.cc` (`arrayOf`, the hash and `equal`, `sizeOf`, `alignOf`, `spelling`, `isObject`/`isAggregate`) | `[4]i32` has a size, an alignment, one canonical spelling and one `TypeId`, asserted in unit tests over the store | every user-visible form: no syntax builds one yet |
| 2 | **The type in the grammar** | `parse` (`parseType`), `TypePart` (the `[` + count spelling, in source order), parse dump tests | `let a: [4]i32;` parses, and the tree keeps `*[4]i32` distinct from `[4]*i32` | the literals; element access; file scope |
| 3 | **The type in the checker** | `sema/typespec.cc` (the count's fold and the `[N]` part), the binding/parameter/return paths, the new diagnostics | a `[4]i32` binding, parameter and return type check; the count rules (≥ 1, size overflow, `[]`, a missing element) each have a code and a sentence | the literals; `a[i]`; every operator but `=` |
| 4 | **Element access** | `sema/access.cc` (the obligation's extent; `a[i]` as an access with the array's type beside the element's), `ir` (`getelementptr` without `inbounds`, the load, the alignment) | `a[i]`, `&a[0]`, `&a`, and the constant-index bounds error at the index | the literals; by-value copies; file scope |
| 5 | **The value shape** | `ir/declarations.cc`, `ir/types.cc` (`storageType` for an array), `ir/expr.cc` (the copy) | a parameter and a return are the caller-copy-plus-pointer shape (13), a whole-object `=` is one `memcpy` with the object's own alignment, `-g` names the object | the literals; file scope |
| 6 | **The typed literal** | `parse` (`T{...}`, the one-token lookahead), `sema` (the element rules: exact length, no ragged, the fill, no conversion inside), `ir` (the aggregate value) | `[3]i32{1, 2, 3}`, `[_]u8{...}` (outermost only), `[64]u8{0; 64}` | the context form |
| 7 | **The context form** | `sema/type.h` (the deferred aggregate kind), `check_expr.cc`/`coerce.cc` (the decision and its recursion into the elements), the new `sema-literal-type-unknown` | `let a: [3]i32 = [1, 2, 3];`, nesting, `[0; 64]`, and the refusal whose sentence names both fixes | file scope |
| 8 | **File scope** | `sema/global.cc` (the ICE's array case), `ir/declarations.cc` (the constant), `ir/debug.cc` (the composite type) | `const TABLE = [_]i32{...};`, the splat, nesting, an array of `str`, `static`, and `-g` naming the elements | the slice spellings |
| 9 | **The example and the page** | `examples/014_arrays.mx`, the corpus test, `website/docs/language/arrays.md` | the only specification a reader can run, and the page with a *not implemented yet* mark wherever a piece above is still refused | — |
| 10 | **The reserved spellings** | `parse` + `validate` + `sema` for `[]T` and `..`, plus the lexer's one-character lookbehind so `..` is reachable at all | two sentences saying *reserved*, so the day a slice lands the message changes in one place and no `.mx` file ever spelled `[]T` as something else | the slice itself |

Two of these steps are the ones to resist merging. **Step 1 must not carry a
literal**: the interner's hash and `equal` are where a count compared as text
would hide, and a test over the store is the only place that is legible. **Step 6
must precede step 7**: the typed form needs no deferred kind, so it proves the
element rules, the exact length and the lowering *before* the inference mechanism
is introduced — after which a failure in step 7 is unambiguously the decision
logic, and not one of the five things step 6 already pinned.

## The hazards, and what each one already cost someone

Each row is a real bug class from the languages above, with the guard here and the
step that installs it. The rule is `CONTRIBUTING`'s: every claim has a test, and
the failure modes that matter are the ones that *compile* — so each guard is a
diagnostic or a scan, never a convention.

| Hazard | Who paid for it | The guard, and where it lands |
| --- | --- | --- |
| The type's identity is a *spelling* | Rust: RFC 2000 makes structural equality a **requirement** on a const parameter, because otherwise `[T; N]` has no decidable identity | the count is a folded value in the interner's `hashOf`/`equal` (19) — step 1's test is two spellings of one count producing one id |
| The bound leaves the type | C: `sizeof a` is a pointer's size in a function, `void f(int a[10])` is `void f(int *)`, and no compiler can refuse `a[10]` | no decay anywhere (2); the extent in the obligation (26); the constant-index bounds error (7) — steps 3 and 4 |
| The length comes from the run time | C: a VLA makes `sizeof` a runtime expression and the frame unbounded; the failure is a silent stack overflow | the count is a folded constant (4) and an object too large for a frame is refused where it is declared (14) — steps 3 and 5 |
| The copy rule is not in the type | Go: `[N]T` copies and `[]T` aliases, so the same-looking assignment means two things and `append` may or may not reallocate | the copy is the *type's* property (3) and the view is a different type that does not exist yet (17) — step 5, and the site page states it in the type's table |
| An aggregate in the ABI, assumed by the front end | Zig on MIPS struct returns, Crystal on AMD64/ARM64, Odin's ARM32, .NET's ARM64, Inko under optimisation — all shipped wrong code from a front end that trusted LLVM's aggregate convention | decision 13's revision: the internal shape *is* the MEMORY-class shape (caller copy + pointer, `sret` for a return), so the ABI is `cinterop`'s attributes and not a second lowering — step 5 |
| A big object copy written as an instruction chain | Every front end that built an `insertvalue` chain per element: compile time grows with the count, and the count is unfriendly input | one `memcpy` with a constant length per object copy (15/13) — step 5, asserted as *one* memory intrinsics call, not a golden file |
| A splat *expanded* by the compiler | The constant folders that turn `[0; 1<<20]` into a million values before the backend sees the zero fill | the value record is a **shape** — element type, count, a list *or* a splat — and never the bytes (15) — step 8, tested with a count whose expansion would be visible in the compile time |
| A count in the *type* deciding the compiler's cost | every front end that materialises a range designator: `int a[1000000] = {[0 ... 999999] = 7};` costs the compiler a million operands and the guard against it is always an afterthought | a non-zero fill is written out and **bounded** at `kMaxFillElements`, with the count, the bound and the fix in the sentence (27) — and a zero fill is exempt because it is *one* constant, which is why `[1 << 40]u8{0; …}` still compiles |
| A frame the source chose the size of | `let a: [1 << 40]u8;` is eleven characters, and the alloca is a terabyte: LLVM builds it, the backend emits the subtraction, and the crash is at the first instruction of the function | `kMaxStackObjectBytes` (28), asked at the two places a slot is created — a local binding and a by-value argument's caller copy — so the refusal lands on the declaration |
| Braces where the language has none | C's habit is `int a[3] = {1, 2, 3};`, so the first thing a reader writes in an annotated binding is `{1, 2, 3}` — and a parser that says "expected an expression" there teaches nothing about the two characters to swap | `parse-brace-without-type`, at the two positions where a `{` cannot be a block (an element, and the value of an annotated binding), with the group still read as the literal it was meant to be |
| A half-initialized object that nobody sees | C: "fewer initializers means zero-fill" is how an array ends up partially written with no diagnostic at all | exact length, one explicit fill (9, 10) — step 6 |
| A partially initialized object read as if whole | rustc's init analysis exists for this; C considers it undefined and says nothing | object-level definite assignment with the fix in the sentence (23) — step 3, using the flow pass that already refuses an unassigned scalar |
| A loop bound that goes stale | C: `sizeof(a)/sizeof(a[0])` and the `ARRAY_SIZE` macro, because the count is not readable from the language | the bound never affects the type, and `[_]` makes the count come from the list at the *definition*; `len(a)` is listed as the companion of `sizeof` and arrives with it — step 9's example writes the count once |
| `char*` and `char[]` as one thing spelled twice | C, still | a string literal does not become an array and an array does not become a pointer (16); `str` is a future `[:0]u8` and not a parallel concept (17) — steps 3 and 6 |
| Address comparison through `=`/`==` | C: `a == b` on two arrays compares addresses, and the reader means element-wise | every operator but `=` and the subscript is refused by name (25) — step 3 |
| A const object whose bytes the same unit rewrites | nobody, because every language decided the other way — but C's own `const`-qualifier hole shows how the rule leaks | the place rule reaches an element (24), with the pointer escape stated as the *documented* hole it already is — step 4 |
| `-fcheck` bounds checking an object it cannot name | C: the check exists only in `-fsanitize=bounds` and `_FORTIFY_SOURCE`, both of which need the compiler to know the extent it threw away | `object` provenance plus the recorded count (26) is exactly the module-statable half of the checked build — step 4 |

## What this makes impossible

- **`sizeof a` that lies.** There is no decay, so an array parameter is still the
  array: `sizeof` inside the function is the object's size.
- **A bound nobody can check.** The count is in the type, so `a[10]` on a
  `[4]i32` is one token and one diagnostic, at compile time, without analysis.
- **Silent stack overflow.** A VLA cannot be written, and an object too large for
  a frame is refused where it is declared.
- **Self-modifying size.** `sizeof` and the address of `&a[1]` are constants.
- **An array that is half-initialized by accident.** The length is exact, and the
  only zero fill is the one that was written.
- **A ragged table.** `[[1, 2], [3]]` has no type: the inner length is either
  written or `_`, and `_` is outermost-only.
- **`char*` vs `char[]`.** A string literal does not become an array, and an array
  does not become a pointer, so the two are never the same object spelled twice.
- **An aggregate crossing the C ABI by accident.** The one place an array could
  silently become ABI-relevant is refused until the layer that owns the ABI exists.

## What is deliberately not decided here

| Question | State |
| --- | --- |
| `[]T` slices, `..` slicing, `{ptr, len}` | reserved spelling, refused with a sentence; the representation is an aggregate, so it lands after `struct` and the `cinterop` ABI layer |
| `sizeof` / `alignof` / `len` | the natural companion, and one item: the array's size is a constant so it joins the ICE, and `len(a)` is the count with a name — without it a loop bound is a literal that can go stale, which is the `ARRAY_SIZE` macro C wrote because its language could not. The same reader that folds a count for a type folds one in a constant expression (`builtins.md`) |
| A count that is a constant expression or a named constant (`[N]T` with `const N = 4;`) | the seam is the count reader: today a literal, because a type position has no typed tree to fold. It is the same machinery `sizeof` needs, so it is not a separate design |
| Const generics, `[N]T` generic over `N` | not in scope: it needs functions over types, which is a much larger record. Rust's RFC 2000 is the reference for how much larger |
| Designated initializers (`.field: v`, `[i]: v`) | reserved for the same list, decided with `struct`, where most of its value is |
| `char` ↔ `str` and byte views (`[N]u8` as a `str`) | refused today on purpose (decision 16); the relationship is `[:0]u8` and belongs to the slice record |
| Bulk operations (`memcpy`-shaped, `copy`, `fill`) | nothing here should grow a builtin before the stdlib does |
| A `-Wcopy` lint for large by-value copies | `-Wconversion` is the shipped precedent for a lint with a code and a test; this one waits until there is a program big enough to want it |
| `volatile`, `restrict` on an array, alignment above the element's | `?`, and each is a qualifier question the model has not opened |

## How the claims above are checked

The rule for a language feature in this project is that every claim has a test, so
the table is the work list, one row per module and per mini-behaviour.

| Claim | Checked by |
| --- | --- |
| A file-scope table is a **list of values** | `array_test.cc`: `const TABLE: [3]i32 = [10, 20, 30];` publishes `aggregate`, three `int` elements, each with its own type and no `splat` |
| A **fill is one record** whatever the count | the same file: `[1048576]u8{0; 1048576}` publishes `splat` with **one** element — the property that makes the compiler's cost the source's and not the type's |
| A nested initializer **recurses**, including a nested fill | `[[1, 2, 3], [4, 5, 6]]` gives an element of kind `aggregate` with its own three records; `[2][3]i16{[1, 2, 3]; 2}` gives an aggregate element whose own `splat` is set |
| An element that is not constant is refused **once, at the element** | `[2]i32{1, g()}` is one `sema-global-not-constant` whose sentence is about the *call*, not about the table |
| The module: a table, a zero fill and a `bool` array | `ir/global_test.cc`: `[i32 10, i32 20, i32 30]`, `zeroinitializer` for a fill of 2²⁰, and `[3 x i8]` (never `[3 x i1]`) for `[3]bool` |
| **The two bounds are bounds** | `[2097152]u8{7; 2097152}` is one `ir-initializer-too-large`, and `[1 << 40]u8{0; …}` still compiles as one `zeroinitializer`; `let big: [16777217]u8;` is one `ir-object-too-large` while exactly `kMaxStackObjectBytes` builds |
| A `{` with no type in front of it is a sentence, not four | `= {1, 2, 3}` and `{{1, 2, 3}, …}` are `parse-brace-without-type`, once each, with the group read as the literal it was meant to be |
| `[N]T` is a type, `[4]i32` equals `[4]int`, and `[4]i32` is not `[8]i32` | `sema` tests over the store, in the shape of the existing type-identity tests |
| The count survives the tree | a parse test that reads `*[4]i32`, `[4]*i32` and `[2][3]i32` and asserts the structure and the spelling of each |
| A count of zero, an empty `[]`, and a missing element type each have their own code and sentence | one input per parse/validate code, from the same enumeration sweep the other stages use |
| A count that is not a constant is refused, naming `alloc` | a sema test with `let n: i32 = 4; let a: [n]i32;` and with `[1 + 1]i32` (the literal-only rule), each asserting the sentence |
| The two literal forms agree | one test per form for the same value, asserting the same `TypeId` and the same bytes in the module |
| Exact length, in both directions | `[4]i32 = [1, 2, 3]` and `[4]i32{1, 2, 3, 4, 5}` are `sema` errors whose message names both counts |
| `[_]` infers; `[2][_]` does not | two tests, the second asserting the refusal is about the *inner* `_` |
| No context, no type | `let a = [1, 2, 3];` is `sema-literal-type-unknown`, and the sentence names both fixes |
| The element rules apply inside | `[1, 2.0]` (the mixed-number rule), `[256]` into `[1]u8` (the range rule), `"abc"` into `[4]u8` (decision 16) |
| The fill is checked, and an empty list is refused | `[4]i32{0; 3}` and `[4]i32{}` |
| Ragged literals and ragged types | `[[1, 2], [3]]` with no context; `[2][3]i32` against a `[2][4]i32` binding |
| No decay | `f(a)` where `f` takes `*i32` and `a` is `[4]i32` — refused, naming `&a[0]`; and `a + 1` refused by the operator |
| Value semantics | a program that assigns one array to another, passes one by value and returns one, asserting the destination changed and the source did not |
| `sizeof`/alignment of an array, including an array of a padded element | layout tests per target triple, in the shape of the existing `TypeStore` layout tests |
| The constant-index bounds check | `a[4]` on a `[4]i32` is a diagnostic at the index; `a[3]` is not; `a[-1]` is |
| The runtime index is the checked build's | a program that reads `a[i]` with a runtime `i` out of range: defined-but-unreported in a release build, a reported trap under `-fcheck` (the shape § *The checked build* already uses) |
| The stack limit | an object just under and just over `kMaxStackObjectBytes`, asserting the second is refused and names `alloc` |
| The module's shape | `ir` tests: `[N x i32]` for the type, `[N x i8]` for `[N]bool`, the alignment on every access, `getelementptr` **without** `inbounds`, one `memcpy` for a copy, and the scan clean |
| A file-scope array is a value and not `constant` | the `globals` tests, extended with the array kinds — `zeroinitializer`, a `ConstantArray` whose operands are at the *storage* form of the element, and a nested `ConstantArray` — and the `ir` assumption scan unchanged |
| `-g` names the elements | a debug test that finds the `DICompositeType` with its `DISubrange` for an array binding |
| The boundary refuses what it cannot promise | `array_test.cc`: an `extern` declaration with a `[N]T` parameter **and** one with a `[N]T` return are each `sema-extern-aggregate`, while `extern fn i32 f(p: *[4]i32)` and a *defined* function that takes and returns arrays are accepted — the refusal is about the array in the signature and not about the word (decision 11) |
| A C-shaped `T a[N]` parameter is `*T` at the boundary | `cinterop`'s record, where the boundary's own rules live; the language side refuses the array by name and will never quietly decay one |
| The examples still compile and run | a new `examples/014_arrays.mx` (a sum over an array, a 2D table, a file-scope `const` table, `&a[0]` into a helper) in the corpus `make examples` runs |
| **A predicate with a consequence**: `[4]i32` is an object and not a scalar, and every consumer that must accept an aggregate does | a store test in both directions, plus the binding/parameter/return paths that had to be re-answered — the guard against `isScalar` having quietly widened (decision 21) |
| **The count's identity** | `[04]i32`, `[0x10]i32` and `[16]i32` yield **one** `TypeId` and one spelling; `[010]i32` is refused with the shared reader's own message (decision 19) |
| **A type whose size is not a number** | a count whose `N × sizeOf(T)` overflows `size_t` is refused *at the count*, naming the count (decision 20) |
| **Every operator but `=` and the subscript is refused, each by name** | the operator table is walked: comparison, relational, arithmetic, bitwise and unary, one locked refusal per family, plus the converse that `=` and `a[i]` are accepted (decision 25) |
| **`const` reaches the elements** | `TABLE[0] = 1` and `&TABLE[0]` are both `sema_address_of_const` / the store rule, with a scalar `&c` test alongside so the extension did not widen the rule (decision 24) |
| **A partially initialized array is not read as whole** | the loop that writes elements and then reads the object is refused, and the sentence names `[0; N]`; an initializer and a whole-object `=` are accepted (decision 23) |
| **The value shape in the module** | a by-value parameter is a `ptr` and **not** a `[N x T]` in the signature, the call site holds exactly **one** memory intrinsic, a return is `sret`-shaped, and no `insertvalue` chain appears for a copy (decision 13) |
| **The extent is recorded, and the checked build uses it** | `a[i]` on a local records `object` plus the count and `-fcheck` emits the bounds comparison; the same subscript on a parameter records `foreign` and emits none (decision 26) |
| **The splat is a shape, not bytes** | a large `[0; N]` lowers to `zeroinitializer`/`ConstantAggregateZero` from the record's shape, asserted on the module — a folded element list would show up as a compile time and as a count mismatch (decisions 15, 20) |
| The reserved spellings stay reserved | `[]T` and `..` each have a test asserting the sentence (`parse-reserved-range` is in the reachability sweep, so it cannot go dead), and the lexer has one asserting `a[1..2]` is two dots while `.5` and `1.5` are still numbers |

## References

- ISO/IEC 9899 (C17), **6.3.2.1p3** — the array-to-pointer conversion and its four
  exceptions (`sizeof`, `_Alignof`, `&`, a string literal's initializer), and p1
  on why an array is not a modifiable lvalue.
- C17, **6.7.6.2** (declarators, VLA) and **6.7.2.1p18** (a flexible array member
  is ignored by `sizeof`); C11 made the VLA an optional feature.
- GCC's zero-length arrays and Clang's diagnostics on them — the extension the
  standard never had, and why it matters that they are not legal C:
  <https://lwn.net/Articles/908817/>
- The Red Hat Developer article on flexible array members, for the pattern that
  survives only because the kernel enforces what the type cannot:
  <https://developers.redhat.com/articles/2022/09/29/benefits-limitations-flexible-array-members>
- Rust RFC 2000, **const generics** — why `[T; N]` needs a language feature before
  it can be written generically: <https://rust-lang.github.io/rfcs/2000-const-generics.html>
- The Rust standard library's `array` and `slice` pages — `Copy` only when the
  element is, and the coercion to a slice:
  <https://doc.rust-lang.org/std/primitive.array.html>, <https://doc.rust-lang.org/std/primitive.slice.html>
- The Zig language reference, *Arrays* and *Slices* — the taxonomy this record
  borrows the good half of, including `[_]T` and the sentinel forms:
  <https://ziglang.org/documentation/master/#Arrays>
- zig.guide, *Slices* and *Sentinel Termination* — the `ptr`+`len` representation
  and `[:0]T`: <https://zig.guide/language-basics/slices/>, <https://zig.guide/language-basics/sentinel-termination/>
- Go, *Arrays, slices (and strings): the mechanics of `append`* — the canonical
  account of the trap in failure mode 3: <https://go.dev/blog/slices>
- Swift Evolution **SE-0453**, *InlineArray, a fixed-size array* — the closest
  design in the field: `stride × count`, the element's alignment, in-place literal
  initialization, and a count mismatch as a compile error:
  <https://github.com/swiftlang/swift-evolution/blob/main/proposals/0453-vector.md>
- *The mess that is handling structure arguments and returns in LLVM* — the
  SysV/AMD64 vs AArch64 classification rules, `byval`, `sret`, the IR-level
  `memcpy`, and the list of front ends that got it wrong:
  <https://yorickpeterse.com/articles/the-mess-that-is-handling-structure-arguments-and-returns-in-llvm/>
- The System V AMD64 ABI — the classification of aggregates (INTEGER/SSE/MEMORY,
  the four-eightbyte rule) that decision 11 leaves to `cinterop`:
  <https://refspecs.linuxbase.org/elf/x86_64-abi-0.99.pdf>
- LLVM *Language Reference*, `getelementptr` — the `inbounds` rule and the
  one-past-the-end allowance, which `ir.md` decision 28 already governs:
  <https://llvm.org/docs/LangRef.html#getelementptr-instruction>
- *A Guide to Rustc Development*, **Tracking moves and initialization** — the
  place-based analysis this record's decision 23 measures itself against:
  <https://rustc-dev-guide.rust-lang.org/borrow_check/moves_and_initialization.html>
- Rust RFC 2000 § *const parameters must have structural equality* — why an
  array's identity forces a requirement on whatever computes its count (19):
  <https://rust-lang.github.io/rfcs/2000-const-generics.html>
- LLVM *Language Reference*, the **`byval` and `sret` parameter attributes** —
  the shape decision 13 revised towards, and the attributes `cinterop` will
  choose from: <https://llvm.org/docs/LangRef.html#parameter-attributes>
- LLVM *Language Reference*, **`ConstantAggregateZero`** — the one constant a
  zero fill becomes, whatever its count, and the reason decision 27 exempts it:
  <https://llvm.org/docs/LangRef.html#constantaggregatezero-constant>
- LLVM *Language Reference*, the **`splat` constant expression** — "only for
  vectors", which is why a non-zero *array* fill has to be written out at all:
  <https://llvm.org/docs/LangRef.html#constant-expressions>
