# Slices — `[]T`, the view, and the six rules that keep it from being C's decay

The design record for the slice: what it **is** (a `{ptr, len}` descriptor), what it
is **not** (an object, a container, a literal), how one is taken (`..`), how its
length is asked for (`len`), and what the language refuses on purpose so that a
slice never becomes the thing that made C's arrays unexplainable.

It sits under [`architecture.md`](../architecture.md) and beside
[`arrays.md`](arrays.md) and [`memory.md`](memory.md). `arrays.md` decision 17
reserved the two spellings — `[]T` parses today and is refused with a sentence,
and `..` is lexed, parsed and refused with a sentence — so this record is the
landing of something already spelled, not a new syntax.

**Status: planned.** Nothing here is implemented; the two spellings above are the
only part of it that exists, and they exist as *refusals*.

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
| **15** | **`len(x)` is the spelling, for arrays and slices both.** On an array it folds to the count; on a slice it reads the descriptor | It must be one form: an array can never have a member, so `.len` would be a spelling that only half the types could use. It also means a slice does not gain a member before `struct` exists, which is decision 22's whole point. `sizeof(x)` answers for the *descriptor* (two words) and `len(x)` for the *data*, and those are different numbers on purpose |
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
fn usize first_half(a: []i32) { return len(a) / 2; }

fn sum(s: []i32) {
  let total: i32 = 0;
  for i in 0..len(s) { total = total + s[i]; }
}

fn void main() {
  let table: [8]i32 = [1, 2, 3, 4, 5, 6, 7, 8];
  let all: []i32 = table[..];        // the whole object, as a view
  let head: []i32 = table[0..4];     // the first four
  let tail: []i32 = table[4..];      // from four to the end
  let mid: []i32 = head[1..3];       // a view of a view: 0 is `2` here, not `1`
  sum(mid);
  let p: *i32 = &table[0];
  let first_two: []i32 = p[0..2];    // from a pointer: both bounds written
}
```

Refused, each by name and each with the fix in the sentence:

| Written | Why it is refused |
| --- | --- |
| `[]i32{1, 2, 3}` | a slice has no literal: name an array or an allocation first (decision 7) |
| `s.len` | `len(s)` is the spelling; members arrive with `struct`, and a view will not be the type that introduces them (decision 15) |
| `&s[0..2]` | a pointer to a descriptor is a pointer to a temporary (decision 11) |
| `p[..4]` on a `*i32` | a pointer has no length to count back from; write both bounds (decision 8) |
| `extern fn f(s: []i32)` | the descriptor's ABI at a foreign boundary is not promised yet (decision 13) |
| `f(table)` where `f` takes `[]i32` | no decay: `f(table[..])` (decision 14) |
| `let a: []i32 = table;` | same: an array is not a view, `table[..]` is (decision 14) |

## The plan, in steps

The order is the pipeline's, and each step is a thing that can be tested alone: the
type, then the length, then taking one, then the descriptor in the module.

| # | Step | Files | What turns on | What is still refused |
| --- | --- | --- | --- | --- |
| 1 | **The type and its layout** | `sema/type.h` (`TypeKind::Slice`), `type_store.h`/`.cc` (`sliceOf`, hash/equal, `sizeOf`, `alignOf`, `spelling`, `isObject`), `sema/typespec.cc` (the `[]T` reader produces the type instead of the sentence) | `[]i32` has a size, an alignment, one spelling and one `TypeId`; two slices of different elements are different types; the `[]T` refusal sentence disappears and a `[]T` *binding* starts working | taking one, `len`, anything that reads it |
| 2 | **`len`** | the grammar (a keyword), `parse`, `syntax`, `ast`, `sema` (folding for an array, the descriptor read for a slice), `ir` | `len(a)` folds to a constant; `len(s)` reads a word. One operator, two answers, both tested | slicing |
| 3 | **Taking one: the `SliceExpr`** | `syntax`/`ast` (a distinct node kind with `begin`/`end`, absent when the form omits it), `parse` (the `..` builds it instead of the reserved sentence), `sema` (`checkSlice`: base kinds, the four forms, constant bounds, the pointer's both-bounds rule) | all four forms type-check; `a[0..5]` on `[4]T` is `IndexOutOfRange`; `a[3..1]` is its own sentence; the reserved sentence for `..` is gone | the descriptor in the module |
| 4 | **The descriptor** | `ir/types.cc` (`{ptr, usize}`), `ir/expr.cc` (build it from a place + two values), `ir/expr.cc` (`s[i]` as a place through it), `ir/declarations.cc` + `ir/types.cc` (by-value parameter and return) | a slice crosses a call by value; `s[i] = v` writes the array it views; a view of a view works; the aliasing test passes | `sizeof`, debug info, `str` |
| 5 | **`sizeof` and the debug type** | `sema`, `ir/expr.cc`, `ir/debug.cc` | `sizeof([]i32)` is the descriptor's size and `sizeof(table)` the object's; `len(s)` and `sizeof(s)` disagree on purpose and both are tested; a debugger shows `ptr` and `len` | `alignof` (its own item) |
| 6 | **The boundary and the corpus** | the `extern` refusal's message, `examples/016_slices.mx`, the site page, the roadmap, the invariant scan's "cannot prove a runtime extent" row | the refusal reads as a boundary rather than a gap; the example runs | everything in *Not in scope* |

## Tests

| Property | How it is a test |
| --- | --- |
| A slice is two words | `sizeOf`/`alignOf` per target triple, the shape the array's layout tests already use |
| A copy is a view | compile and **run**: write through the copy, read through the original, assert the array changed |
| A view's index 0 is its own | `s = a[2..5]; s[0]` is `a[2]`, asserted by value |
| An array copy is a copy | the other half: `let b: [4]i32 = a;` then write `b`, assert `a` unchanged |
| Constant bounds are checked | one case per form: `a[0..5]` on `[4]T`, `a[5..]`, `a[3..1]`, each asserting the code and the sentence |
| `len` has two answers | `len(a)` is a constant (usable where a constant is required) and `len(s)` is a load, asserted from the module |
| No literal, no member, no decay | the refusal table above, one test per row, each asserting the code |
| `extern` refuses both directions | the existing aggregate drift, extended: `extern fn f(s: []i32)` and `extern fn []i32 g()`, while `*[]i32` passes |
| A slice of a slice composes | `a[1..7][2..4]` — the bounds are relative to the view, and the test says so |
| A slice of a pointer is unchecked | `p[0..n]` compiles for any `n`, which is the documented obligation (decision 8), and the test is that nothing pretends otherwise |
| The descriptor is in registers | the module has no `alloca` for a slice parameter, and a returned slice is a two-word aggregate |
| The scan is honest | an access through a slice whose extent is not provable produces the "runtime extent" row and not a claim |

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

## Not in scope, with the reason

- **`str` as `[]u8`.** `str` stays a NUL-terminated pointer; the conversion is a
  length computation and therefore explicit and named, and it is the next item
  after this one (decision 6).
- **`append` / `push` / capacity.** A growth operation is an ownership decision
  (22), and the descriptor has no room for it by design (1).
- **Borrow checking (`&T` / `&mut T`).** `memory.md`'s checked layer. It is what
  would turn decision 21's obligation into a rule, and it is a project, not a
  step.
- **`-fcheck` and runtime bounds.** Named by decision 16 and by `arrays.md`
  decision 26, and a switch rather than a language feature.
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
