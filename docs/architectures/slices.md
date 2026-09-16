# Slices — `[]T`, the view, and the six rules that keep it from being C's decay

The design record for the slice: what it **is** (a `{ptr, len}` descriptor), what it
is **not** (an object, a container, a literal), how one is taken (`..`), how its
length is asked for (`len`), and what the language refuses on purpose so that a
slice never becomes the thing that made C's arrays unexplainable.

It sits under [`architecture.md`](../architecture.md) and beside
[`arrays.md`](arrays.md) and [`memory.md`](memory.md). `arrays.md` decision 17
reserved the two spellings — `[]T` parsed and was refused with a sentence, and `..`
was lexed, parsed and refused with a sentence — so this record is the landing of
something already spelled, not a new syntax, and both reserved sentences are gone
(`parse-reserved-range` no longer exists).

**Status: implemented, except the two readings of the length.** The type, its
layout, the four forms, the bounds that are checked, the descriptor in the module,
the by-value call convention, the debug type, the refusals and the example are in
and tested. Two things this record assumed would land with it did **not**, and
neither is a gap in the slice: **`len(x)`** (decision 15) and **`sizeof`** (step 5)
are each a *form the grammar has to have* — `len` a call whose argument is a place,
`sizeof` a type in an expression — and both are their own roadmap items rather
than part of the view. Until they land, a view's length is read by the programmer
who wrote the bounds (the example does exactly that), and the descriptor's two
words are proved by the store's own layout tests instead of by `sizeof`.

## The decision, in one paragraph

A slice is a **descriptor** `{ptr, len}` — two words, a *view* of storage somebody
else owns. Copying it copies the descriptor and aliases the data; index `0` is the
first element of the view and never an index into whatever it was taken from;
there is no capacity, so there is nothing to grow and no `append`; there is no
slice literal, because a literal's storage would be a temporary and a view of a
temporary is the dangling class this language refuses to hand out; and the length
is a word in the descriptor rather than a sentinel, except in `str`, which stays
exactly what it is today.

## Does this need `struct` first?

`arrays.md` decision 17 said a slice is an aggregate value, "which means slices
land after `struct` and after the `cinterop` ABI layer, in that order". That
reasoning was written before the array work, and the array work answered it: what
`struct` and `cinterop` were standing in for is the **aggregate-value machinery**,
and that machinery shipped with arrays — `alloca`, the copy, `memcpy`, by-value
parameters under an internal convention, `DICompositeType` in debug info, and one
refusal (`sema-extern-aggregate`) for the boundary where the promise does not
exist yet. A slice needs all of it and needs nothing else from `struct`: no
members, no layout algorithm, no name.

What `cinterop` still owns is unchanged and stays refused: **a slice across an
`extern` declaration**. The mechanism is here; the promise is not.

And landing slices *before* `struct` settles one question that `struct` would
otherwise settle by accident: `len(x)` is a front-end operator from the first day,
so when `struct` later brings `.field`, a slice does not quietly acquire a member
and there is no second way to ask for a length.

## What the market did, and what each answer cost

| Language | The design | What it teaches |
| --- | --- | --- |
| **C** | no slices; an array **decays** to a pointer in every expression except `sizeof`, `&` and `_Alignof` | The origin of every failure this record is organized against: `sizeof a` is a pointer's size inside the function that took `a`, `void f(int a[10])` means `void f(int *)`, and no compiler can refuse `a[10]`. This language has **no decay in any position**, so `a[..]` is how an array becomes a view, and the missing characters are the feature |
| **Go** | `[N]T` is a value and `[]T` is a header `{ptr, len, cap}`; two types that look alike and copy differently | Two types whose *syntax* is one bracket apart and whose *semantics* are opposite (assignment copies an array, aliases a slice), which is why decision 2 below is stated as loudly as it is. `cap` is what `append` needs and what makes slicing share writable storage in ways the type does not show; it is also why `s[:2]` and `s[:2:2]` exist. Not copying the capacity is a deliberate simplification: it removes a field, an operator form and the aliasing subtleties that come with growing |
| **Rust** | `&[T]` / `&mut [T]`: a fat pointer that is *borrowed*, so lifetimes are in the type; `&arr[..]` coerces an array to a slice; indexing panics out of range in safe code; `len()` | The strongest version of the idea, and the reason it is strong is the borrow checker — the part this language does not have yet. Everything else is copied: index `0` is the view's first element, the length is in the descriptor, and taking a slice is an *explicit* operation on a place |
| **Zig** | `[]T` (`ptr` + `len`), `[*]T` (a many-item pointer, the thing you slice to get a `[]T`), `[:0]u8` (a sentinel slice) | The representation this record uses, and the sentinel slice is exactly what `str` already is. It also shows the cost of the full taxonomy: six array-shaped types, and a reader who has to know which one a parameter is |
| **Swift** | `ArraySlice` shares storage **and keeps the parent's indices**, so `slice[0]` is a trap and `startIndex` is the real first element | Failure mode 4, and the single decision this record makes differently from almost everyone: a view's index is its own. A slice that remembers where it came from moves an off-by-N bug into every loop that walks it |
| **C++** | `std::span<T>` and `std::string_view`: a pointer and a length, non-owning, no lifetime in the type | The same descriptor with none of the safety: a `string_view` outliving its string is the canonical bug of modern C++. This record cannot close that hole either -- see decision 21 -- but the difference between "documented and unchecked" and "documented and unchecked *with the layer that would check it named*" is the whole of what it can do today |
| **D** | `T[]` is a fat pointer with `.ptr` / `.length`; `arr[1..3]` slices; `.dup` copies | The endgame shape (members on a built-in) that this language deliberately does not take at this point: `.length` is a member, and members arrive with `struct` |

## The five failure modes, and which decision answers each

1. **The bound leaves the type (decay).** `sizeof` starts lying, no compiler can
   refuse `a[10]`, and an array parameter is a pointer. Answered by decision 8 and
   by `arrays.md` decision 2 (no decay) being kept: `f(a[..])` is one extra
   character and it is where the length comes from.
2. **Copy and alias look the same (Go).** Answered by decisions 1–3: no capacity,
   the copy rule stated in the type's own documentation, and a test that writes
   through the copy and reads through the original.
3. **A view of something that no longer exists (C++).** Answered as far as this
   language can answer it today by decision 7 (no slice literal, so a view cannot
   be made over a temporary in the first place) and stated honestly by decision 21.
4. **The view remembers its parent's indices (Swift).** Answered by decision 3:
   index `0` is the view's first element, always.
5. **A container pretending to be a view (Go's `append`).** Answered by decision 1:
   there is no capacity and no growing operation. A growable buffer is a library
   type over an allocator, not a property of the descriptor.

## Decisions

| # | Decision | The alternative, and why not |
| --- | --- | --- |
| **1** | **The descriptor is `{ptr, len}`: no capacity.** | Go's `cap` exists to make `append` amortized, and it makes every slice implicitly share writable storage with a growth path. Growing needs an allocator and an ownership answer this language has not decided; a buffer is a type built *on* `alloc` later, with its capacity visible as its own field |
| **2** | **A slice is a view: copying it copies two words and never the data.** Assignment, argument passing and `return` alias. | The opposite of `[N]T` (decision 3 of `arrays.md`), and that is the point: one is an object, the other is a view, and the *syntax says which* (`[N]T` vs `[]T`) instead of the context deciding. A test asserts the aliasing, because a property this surprising that is not tested is a property nobody knows |
| **3** | **`s[0]` is the first element of the view.** | Swift's preserved indices turn every loop over a slice into a question about where it started; and a slice that is not at `0` cannot be passed to anything that takes a slice without a conversion |
| **4** | **The element type is in the type, and its rules are the array's**: `[]T` ≠ `[]U`, no element of `void`, `!`, or a function type, and an element must be an *object* (`arrays.md` decision 21) | The same predicates, so a slice of a slice and an array of an array cannot disagree about what an element may be |
| **5** | **Layout: `sizeOf([]T) = 2 × sizeOf(usize)`, `alignOf([]T) = alignOf(usize)`.** Asserted per target triple, like the array's | The length is a `usize`, not a `u32` (a `u32` length cannot describe a slice of a 4 GiB object on a 64-bit machine) and not a `usize` per element (bytes invite dividing by the element size at every use) |
| **6** | **The length is a word, and `str` keeps its own type**: a `str` is a *sentinel* slice (`[:0]u8`), stays a pointer and a NUL, and stays the C-compatible thing it is today | Folding `str` into `[]u8` now would make the length a `strlen` call (so `len(s)` on a `str` is O(n) where every other slice is O(1)) and would change every signature in the corpus. The relationship is real and stays reserved: the conversion is explicit and named, and it is its own item after this one |
| **7** | **There is no slice literal.** `[]T{...}` is refused by name, with the fix in the sentence (name the array or the allocation first) | A literal has to live *somewhere*: a temporary array on the stack gives a view whose lifetime ends at the end of the statement, which is failure mode 3 manufactured by the language itself. A slice is always taken from something that already exists |
| **8** | **Slicing forms: `a[l..r]`, `a[l..]`, `a[..r]`, `a[..]`**, on an **array**, a **slice**, or a **pointer** — and on a pointer both bounds are written (`p[l..r]`), because a pointer has no length to default to | This is exactly Zig's answer for the same reason, and it is why it composes: `a[..]` is how an array becomes a view at a call site, and `p[0..n]` is the one place a length comes from the programmer. Requiring both bounds on a pointer makes the unchecked case *look* unchecked |
| **9** | **Constant bounds are checked at the subscript** (an extension of `IndexOutOfRange`), and a constant `l > r` is its own diagnostic | `a[0..5]` on `[4]i32` is arithmetic on two numbers the compiler has, and it is the check C's type system cannot express. `l > r` is a negative length, which has no definition to give |
| **10** | **Bounds are materialised at the pointer index width**, by the same `checkOperand` conversion the subscript already records for `a[i]` | One rule for an index and a bound: `signedInt(pointerBits)`, with the sign extension recorded rather than derived in the lowering. A `u8` bound is zero-extended and an `i8` one sign-extended, exactly as `a[i]` is today |
| **11** | **The result is a value, not an lvalue**, and `&a[l..r]` is refused | The descriptor is a temporary; a pointer to it would be a pointer to two words that die at the end of the expression. The `&` of a *view* is `&a[l]`, which already works |
| **12** | **`s[i]` on a slice is a place**: it loads and stores through the descriptor's pointer under the same provenance rule as `p[i]` | A slice that could not be written through would be a read-only view with no way to say so in the type, and `str` already covers "read-only" by being a different type |
| **13** | **Slices are passed and returned by value** (two words, registers), under the same **internal** convention as an array, and **`extern` refuses them** (`sema-extern-aggregate`, the code that already exists) | The promise at the C boundary is the same promise arrays are waiting for, and it is not this record's to make. What is not refused is `*[]T`: a pointer to a descriptor is a pointer |
| **14** | **Arrays do not decay**: a `[]T` parameter is called with `a[..]` | `arrays.md` decision 2, kept. The alternative is the failure mode this whole area of the language is built to avoid |
| **15** | **`len(x)` is the spelling, for arrays and slices both**, and it is **not implemented yet** — it is the grammar item that reads a length, and it lands on its own | It must be one form: an array can never have a member, so `.len` would be a spelling that only half the types could use. It also means a slice does not gain a member before `struct` exists, which is decision 22's whole point. `sizeof(x)` answers for the *descriptor* (two words) and `len(x)` for the *data*, and those are different numbers on purpose |
| **16** | **Bound checking is compile-time where the bound is constant, and the runtime case is named, not pretended**: a non-constant bound on a slice or a pointer is the checked build's (`-fcheck`), and the invariant scan says it cannot prove a runtime extent rather than claiming it did | The alternative is to emit a check nobody asked for (cost in every loop) or to call it UB in silence (the thing this language does not do). `arrays.md` decision 26 already answered this for an array's runtime index, and a slice is the same case with the length in hand instead of in the type |
| **17** | **The `ir` type is `{ptr, usize}` as a first-class LLVM aggregate** — so a parameter arrives in registers and a local slice needs an `alloca` only when its address is taken | A hoisted `{ptr, i64}` in registers is what every backend does with a two-word descriptor; making it an address-taking object everywhere would pessimise the common case for a case nobody wrote |
| **18** | **Debug info: a `DICompositeType` with two members** (`ptr`, `len`), built from `sema`'s answers the way the array's is | The machinery exists (`ir/debug.cc`), and a debugger that cannot show a slice's length is a debugger that cannot show a slice |
| **19** | **A slice is built in `ir` from the same operands the tree carries**, with the type from `typeOf`; `sema` publishes no descriptor record | Unlike a global's initializer (`arrays.md` decision 27), nothing here needs a second pass or a re-read: the base is a place the lowering already computed and the bounds are already-lowered values |
| **20** | **Provenance**: an element access through a slice carries the provenance of the base where the base is an object the store can name (`&arr` → `object`), and `unknown` otherwise; slicing does not invent provenance | `memory.md`'s scan needs to know what an address points at, and a slice of an *object* is still that object. Slicing a pointer does not upgrade what the pointer was |
| **21** | **Lifetime is documented and unchecked, and the layer that would check it is named**: a slice must not outlive what it views; `memory.md`'s checked layer (`&T` / `&mut T`, the borrow answer) is where that becomes a rule the compiler enforces, and this record states the obligation it will enforce | There is no borrow checker today and no cheap stand-in. What this record *can* do, and does, is refuse the one way to make a dangling view in an expression (decision 7) and say plainly on the language's own page that a view is the programmer's promise, exactly as `alloc`'s result is |
| **22** | **No `append`, no `push`, no capacity, no `concat`.** Growing is a library type over an allocator | A growth operation needs to know who owns the storage and whether it may be reallocated; that is an ownership decision this language has not made, and making it here would decide it by accident |
| **23** | **No `..` with a step, no `..=`-style closed ranges, no multidimensional slicing syntax** | `a[i..j][k..l]` already works by composition, and a step turns a view into a strided view, which is a different type (an index multiplied by a stride with the extent divided -- Zig calls it `@memcpy`-adjacent, C calls it a strided array, and both are later, if ever) |

## The syntax, and the refusals

```minc
// Two views of one table, and a write through one of them.
fn void add_ten(s: []i32) {
  for let i: i32 = 0; i < 2; i += 1 { s[i] = s[i] + 10; }
}

fn []i32 tail(s: []i32) { return s[1..3]; }

fn i32 main() {
  let table: [4]i32 = [1, 2, 3, 4];
  let all: []i32 = table[..];        // the whole object, as a view
  let head: []i32 = table[..2];      // the first two
  let mid: []i32 = table[1..3];      // from one to, but not including, three
  let from_two: []i32 = table[2..];  // from two to the end
  add_ten(mid);                      // `table[1]` and `table[2]` are now 12, 13
  let inner: []i32 = mid[1..2];      // a view of a view: `inner[0]` is `mid[1]`
  let p: *i32 = &table[0];
  let first_two: []i32 = p[0..2];    // from a pointer: both bounds written
  return all[0] + head[1] + inner[0] + first_two[1]; // 1 + 12 + 13 + 12
}
```

A length is read by the programmer for now: `len(s)` (decision 15) is a separate
grammar item, and until it lands a view's extent travels beside it as a parameter
-- which is the same thing a view of a *pointer* has always required.

Refused, each by name and each with the fix in the sentence:

| Written | Why it is refused |
| --- | --- |
| `[]i32{1, 2, 3}` | a slice has no literal: name an array or an allocation first (decision 7) |
| `s.len` | no member access exists yet at all: `.` is not a postfix operator, and a member arrives with `struct`. When a length becomes readable it is `len(s)` and not a member, so a view does not gain one and there is no second spelling (decisions 15, 22) |
| `&s[0..2]` | a pointer to a descriptor is a pointer to a temporary (decision 11) |
| `p[..4]` on a `*i32` | a pointer has no length to count back from; write both bounds (decision 8) |
| `extern fn f(s: []i32)` | the descriptor's ABI at a foreign boundary is not promised yet (decision 13) |
| `f(table)` where `f` takes `[]i32` | no decay: `f(table[..])` (decision 14) |
| `let a: []i32 = table;` | same: an array is not a view, `table[..]` is (decision 14) |

## The plan, in steps

The order is the pipeline's, and each step is a thing that can be tested alone: the
type, then the length, then taking one, then the descriptor in the module.

| # | Step | Files | What turns on | Status |
| --- | --- | --- | --- | --- |
| 1 | **The type and its layout** | `sema/type.h` (`TypeKind::Slice`), `type_store.h`/`.cc` (`sliceOf`, hash/equal, `sizeOf`, `alignOf`, `spelling`, `isObject`), `sema/typespec.cc` (the `[]T` reader produces the type) | `[]i32` has a size, an alignment, one spelling and one `TypeId`; two slices of different elements are different types; the `[]T` refusal sentence is gone and a `[]T` *binding* works | **done** (`type_test.cc`: `ASliceIsTwoWordsWhateverItViews`, `ASliceIsAnObjectAndNotAnArray`) |
| 2 | **`len`** | the grammar (a keyword), `parse`, `syntax`, `ast`, `sema` (folding for an array, the descriptor read for a slice), `ir` | `len(a)` folds to a constant; `len(s)` reads a word. One operator, two answers, both tested | **not done, and not a slice's**: a call whose argument is a *place* is a grammar item, and it lands with the other reading operators |
| 3 | **Taking one: the `SliceExpr`** | `syntax`/`ast` (a distinct node kind; the operands split at the `..`, which stays in the tree), `parse` (the `..` builds it instead of the reserved sentence), `sema` (`checkSlice`: base kinds, the four forms, constant bounds, the pointer's both-bounds rule) | all four forms type-check; `a[0..5]` on `[4]T` is `IndexOutOfRange`; `a[3..1]` is its own sentence; `parse-reserved-range` is gone | **done** (`slice_test.cc`, `parser_test.cc`) |
| 4 | **The descriptor** | `ir/types.cc` (`{ptr, usize}`), `ir/expr.cc` (build it from a place + two values, `s[i]` as a place), `ir/types.cc`/`expr.cc`/`declarations.cc`/`function.cc`/`stmt.cc` (`byReference`: a view crosses by value, an array by copy) | a slice crosses a call by value; `s[i] = v` writes the array it views; a view of a view works; the aliasing test passes | **done** (`ir/slice_test.cc`, `build_command_test.cc`) |
| 5 | **The debug type, and `sizeof`** | `ir/debug.cc`; `sema`, `ir/expr.cc` for `sizeof` | a debugger shows `ptr` and `len`; `sizeof([]i32)` is the descriptor and `sizeof(table)` the object | **the debug type is done** (`ir/slice_test.cc`); **`sizeof` is not** -- it is a type in an expression, i.e. grammar, and it is its own roadmap item |
| 6 | **The boundary and the corpus** | the `extern` refusal's message, `examples/016_slices.mx`, the site page, the roadmap | the refusal reads as a boundary rather than a gap; the example runs | **done** |

## Tests

| Property | How it is a test | Where |
| --- | --- | --- |
| A slice is two words | `sizeOf`/`alignOf` on 64-bit and on `i686`: the pointer width twice, aligned like the pointer and not like the element | `type_test.cc::ASliceIsTwoWordsWhateverItViews` |
| A view is an object and not an array | `isSlice`/`isArray`/`isAggregate`/`isObject`/`countOf`/`elementOf`, and the one element rule that can refuse | `type_test.cc::ASliceIsAnObjectAndNotAnArray` |
| The four forms are one type | one case per form, on each of the three bases | `slice_test.cc::TheFourFormsAreOneType`, `TheThreeBasesAreOneType` |
| A write through a view reaches the array | compile, link and **run**: write `view[0]`, read `table[2]`, and the exit status is the array's value | `build_command_test.cc::AWriteThroughAViewReachesTheArrayItViews` |
| A view's index 0 is its own | `mid[1..3][0]` is `mid[1]`, asserted by the module's two address computations and by a run | `ir/slice_test.cc::AViewOfAViewWalksFromTheViewsOwnStart`, `build_command_test.cc` |
| Constant bounds are checked | `a[0..5]` and `a[-1..2]` on a `[4]i32` are `IndexOutOfRange`, each naming the bound | `slice_test.cc::TheConstantBoundsAreCheckedAgainstTheObject` |
| The end is one past the last element | `a[0..4]` is the whole object and `a[4..4]` the empty view: both accepted, and the length is the subtraction | `slice_test.cc::TheEndOfAViewIsOnePastTheLastElement`, `ir/slice_test.cc::TheLengthIsTheDifferenceAndNotALoad` |
| A bound is an integer at the index width | a `u8` bound is accepted and a float is `IndexNotInteger` | `slice_test.cc::ABoundIsAnIntegerAtIndexWidth` |
| `l > r` is its own sentence | `a[3..1]` is `SliceBoundsReversed`, and it is *not* the out-of-range sentence | `slice_test.cc::EveryRefusalHasItsSentence` |
| No literal, no decay, no address | one case per row of the refusal table, each asserting the code *and* the fragment | `slice_test.cc::EveryRefusalHasItsSentence` |
| `extern` refuses both directions | `extern fn f(s: []i32)` and `extern fn []i32 g()` both name the boundary, while `*[]i32` passes | `slice_test.cc::TheBoundarySaysWhatToWriteInstead` |
| A slice of a slice composes | the element of a view of a view is the same element, and a `[][4]i32` is a view of rows | `slice_test.cc::ASliceIsAnObjectAndNotAnArray` |
| A view crosses a call by value | the signature is `{ ptr, i64 }` in and out, with no `sret` and no caller copy | `ir/slice_test.cc::ASliceCrossesACallByValue` |
| An element access is a plain `getelementptr` | `s[i]` is one extract, one `getelementptr` and one load; no `inbounds` | `ir/slice_test.cc::AnElementAccessReachesTheStorageTheViewNames` |
| The scan is honest | the access through a slice carries extent `0` -- "not known" -- and the module has no assumption violations | `ir/slice_test.cc::ASliceAccessCarriesNoExtentAndTheScanSaysSo` |
| Every code is reachable | the three new codes are in the list that proves the table has no row nobody can reach | `errors_test.cc::EveryCodeIsReachableFromAnInputTheGrammarAccepts` |
| `len` has two answers | **deferred with the feature**: the item's own tests, and the example passes the count instead | -- |

## Hazards, and who paid for them

| Hazard | Who paid | The guard, and where it lands |
| --- | --- | --- |
| Two types one bracket apart with opposite copy rules | Go: assignment copies an array and aliases a slice, and it is the language's most common early bug | decision 2, stated in the type's own documentation and asserted by a run test that writes through a copy |
| A view of a temporary | C++: `string_view` outliving its `string` | no slice literal at all (7), so the case has no spelling -- and the lifetime obligation stated rather than implied (21) |
| The bound leaves the type and `sizeof` starts lying | C: an array parameter is a pointer, and `sizeof a` is the pointer's size | no decay (14); `sizeof(a)` is the object and `sizeof(s)` is the descriptor, two different questions with two different answers |
| A view that remembers where it came from | Swift: `ArraySlice` keeps the parent's indices, so `slice[0]` is a trap | the view's index is its own (3) |
| A view that can be grown | Go: `append` may reallocate and may not, and the two cases share storage differently | no capacity and no `append` (1, 22) |
| A length that cannot describe the object | `u32` lengths on a 64-bit machine (a 4 GiB ceiling nobody sees until it is somebody's production object) | the length is a `usize` (5) |
| A borrowed view with no borrow checker | C++'s `span`, and every language that has slices and no lifetimes | the honest statement (21), plus the refusals that keep the *construction* of a dangling view out of the grammar (7, 11) |
| A bounds check that is silently absent | C's `a[i]`, and every language whose answer is "undefined" | compile-time checks where the bound is constant (9), the runtime case named and scheduled (`-fcheck`) rather than denied (16) |

## What the implementation found

Four things the design did not predict, written down because each one changed a
decision or a sentence above:

1. **`[]T` was not parseable in a declaration at all.** A type *run* — the tokens a
   `Type` node holds — counted `[N]` as three tokens and `[` alone as one, which
   was correct while `[]` was a refusal one stage down. The moment `[]T` became a
   type, `fn []i32 tail(...)` split its run at the `[`, took the `]` as the
   function's *name*, and reported "expected a function name" at the bracket. The
   run now holds `[]` whole, which is the same statement the type reader makes:
   a slice is a type, so the run has to hold it. This is the class of bug a
   reserved spelling hides: the refusal was doing the parser's job for it.
2. **`sizeof([]i32)` does not parse, and it is not the slice's fault.** The
   argument of a builtin is an expression, and a type in an expression is the
   grammar item `sizeof` is waiting on. So the descriptor's size is asserted
   against the store instead (`type_test.cc`), and the record says so rather than
   showing a snippet that does not compile.
3. **A slice is not an aggregate *for the ABI*.** `isAggregate` means "moves as one
   object", and both kinds qualify; what the call machinery needed was the
   narrower "crosses as a pointer to a copy the caller makes", which is the array
   and not the view. That predicate is now `Lowering::byReference`, one place, and
   it replaced six `isAggregate` call sites in the signature, the call, the
   parameter binding, the `sret` attribute and the `return`. The two kinds were
   never the same question and this is where the difference lands.
4. **A parameter needs a slot, even a two-word one.** Decision 17 said a
   parameter arrives in registers and a local needs an `alloca` only when its
   address is taken. The parameter *does* get an entry-block slot — the same one
   every scalar parameter gets — because `&s` has to work for a view exactly as it
   works for a `u8`, and a rule that says "except for this type" is a rule that
   will be forgotten. The register path is what the argument crosses *in*; the
   slot is where the binding lives, and the optimiser removes it when nothing
   takes its address.

## Not in scope, with the reason

- **`len(x)` and `sizeof(x)`.** Each needs a form the grammar does not have — a
  call whose argument is a place, and a type in an expression — and each is its
  own roadmap item rather than part of the view. Until they land, a view's extent
  is passed beside it, which is what the example does and what a view of a
  *pointer* has always required.
- **Member access (`s.len`).** `struct` brings `.field`; a slice will not be the
  type that introduces a member, and `len(s)` is the spelling when it exists
  (decisions 15, 22).
- **`str` as `[]u8`.** `str` stays a NUL-terminated pointer; the conversion is a
  length computation and therefore explicit and named, and it is the next item
  after this one (decision 6).
- **`append` / `push` / capacity.** A growth operation is an ownership decision
  (22), and the descriptor has no room for it by design (1).
- **Borrow checking (`&T` / `&mut T`).** `memory.md`'s checked layer. It is what
  would turn decision 21's obligation into a rule, and it is a project, not a
  step.
- **`-fcheck` and runtime bounds.** **Landed** for the descriptor's own length:
  `s[i]` is compared against the `len` word the record names
  (`ExtentKind::Length`, `checks.md`), which is decision 16's runtime case and not
  a new language feature. A subscript through a *pointer* still has no extent, and
  that is `checks.md`'s absence rather than a slice's.
- **`alloc` returning a slice.** The allocator's own item; today it returns a `*T`
  and the length lives in the programmer's head, which `p[0..n]` at least makes
  explicit.
- **A strided view.** `a[0..n..2]` is a different type (a stride and an extent),
  not a slice with a step (23).
- **`alignof`.** Deliberately with its own item and its own tests.
- **Slices as struct fields, in `offsetof`, or in a constant initializer.** They
  arrive with `struct`, and a slice in a global initializer is a view of *what*,
  which is a question for the module system (`globals.md`).

## References

- `arrays.md`, decisions 2, 3, 5, 7, 16, 17, 21, 26 — no decay, values copy,
  `[0]T` does not exist, `[]T` and `..` reserved, the element must be an object,
  and the runtime index that the checked build guards.
- `memory.md` — the pointer model, provenance, `alloc`, and the checked layer
  (`&T` / `&mut T` / `slice<T>`) where decision 21's obligation becomes a rule.
- The Zig language reference, *Slices* and *Pointers* — `[]T`, `[*]T`, `[:0]u8`,
  and slicing a many-item pointer: <https://ziglang.org/documentation/master/#Slices>
- Rust, `std::slice` and the `array` coercion — `&[T]`, `&arr[..]`, the panicking
  index, `len()`: <https://doc.rust-lang.org/std/primitive.slice.html>
- Go, *Arrays, slices (and strings): the mechanics of `append`* — the aliasing and
  capacity trap this record removes by removing the capacity:
  <https://go.dev/blog/slices>
- Swift, `ArraySlice` — the preserved indices, and `startIndex`:
  <https://developer.apple.com/documentation/swift/arrayslice>
- C++, `std::span` and `std::string_view` — the same descriptor with no lifetime:
  <https://en.cppreference.com/w/cpp/container/span>
- D, *Slices* — the fat pointer with `.ptr` / `.length` and `.dup`:
  <https://dlang.org/spec/arrays.html>
