# Tuples — `(T, T, ...)`, the first product, and the four things it decides

**Status: designed, not implemented.** Nothing in this record exists in the tree
today. It is written before the code for the same reason `slices.md` was: the
decisions here are the ones a later stage cannot fix by adding a case, and three
of them are decisions about *other* modules (the lexer's `.`, the type reader's
recursion, the ABI's aggregate rule) that arrive quietly if nobody names them.

It is also, deliberately, the **first** item of the group that follows: the
machinery a product needs — arity-unknown interning, an aggregate layout, an
unnamed composite type in debug info, a value that is more than one word — is the
same machinery `struct` needs, and building it here means it is built once, over
a type with no name to argue about. Doing `struct` first would decide all of it
twice.

---

## The decision, in one paragraph

A tuple is a **structural product of two or more types**, written `(T, U)`
wherever a type is written and `(a, b)` wherever a value is written. Its members
have no names: they are chosen **by position**, and the position is settled at
compile time — `t.0`, never `t[i]`. It is an *object* and an *aggregate*: it has
a size, an alignment and an address like an array, it copies like an array, and
it is passed and returned by value under the compiler's internal convention, so
`extern` refuses it exactly as it refuses `[N]T` and `[]T`. Its layout is the C
compiler's: declaration order, each member at its own alignment, the size rounded
up to the tuple's alignment. Its debug record is an **unnamed**
`DW_TAG_structure_type` with one member per position named `__0`, `__1`, …, and
the numbers are the *language's* reserved spelling, not the type's: the type has
no names at all. Finally, a tuple is what turns "this function returns two
things" from function-call syntax into a type — `fn (i32, bool) divmod(...)` — and
that is the whole reason it is being built before generics, because a signature
that returns a product needs the product to exist first.

---

## What the market did, and what each answer cost

| Language | The design | What it teaches |
| --- | --- | --- |
| **Rust** | `(T, T)`, structural; fields are `t.0`, `t.1`; a 1-ary tuple needs the comma (`(i32,)`); `()` is `unit`; `repr(Rust)` **may reorder fields**, `repr(C)` is declaration order | The syntax this record adopts almost entirely, including the numeric member. Two things are declined and both are named below: the 1-ary tuple (this language's identity rules make a product of one its own member) and field reordering (a debugger and a C ABI both read position) |
| **C++** | `std::tuple` in the library; `std::get<0>(t)` to read it; C++17 structured bindings `let (a, b)` that are **aliases** to subobjects, not copies | The member access it never solved is why `get<0>` exists. The bindings half is the cautionary tale this record answers in decision 6: an alias-to-a-subobject is invisible in the type system and, measured on this machine, is invisible to the debugger too — GDB prints the declared type of a structured binding as the `tuple_element<…>::type &&` it really is, and `p a` on a reference-returning `get` answers `Cannot access memory at address 0x1` |
| **Go** | **no tuple type**: functions return a list of values, and the list is not a value | The road not taken, and the reason for the order in this record: with no product, "two values" can only be *returned*, never stored, passed on, read by position, or given a name — so every generic that wants to hand back two things has to invent a struct first. Multiple return *syntax* is a permanent subtraction; a product type is an addition |
| **Swift** | `(T, U)`, `t.0`, destructuring `let (a, b) = t`; tuples are **not** part of the stable ABI across a module boundary | The same surface with the same member spelling, and the one warning worth keeping: an aggregate whose layout is a compiler's private business cannot be an inter-module interface. The ABI manifesto's rule is the one this record follows — internal conventions may change freely; what crosses a boundary must be stated — and the boundary is why decision 12 refuses tuples in `extern` |
| **Zig** | no tuple type either: an **anonymous struct** with fields named `"0"`, `"1"` (`std.meta.Tuple` builds one) | The representation this record's debug info lands on, arrived at independently by a language built on the opposite philosophy: a compiler that has no name to give a tuple gives it an unnamed struct with numeric members |
| **D** | `Tuple!(T, U)` with `.0` and *compile-time* indexing; `.length` | The members-as-indexes half, and the endgame this language does not take yet: built-in members (`.length`) are a members-on-types facility, and that arrives with `struct` |
| **C** | no product but the anonymous struct: `struct { int a; char b; }`, layout measured below | The ABI and the debug record are literally this type — which is why the plan can predict them exactly instead of hoping |

### The three measurements this record rests on

1. **C's layout, measured** (clang, `offsetof`/`sizeof`/`_Alignof`, this machine):

   | members | `sizeof` | `_Alignof` | offset of the second |
   | --- | --- | --- | --- |
   | `{char, int}` | 8 | 4 | 4 |
   | `{int, char}` | 8 | 4 | 4 |
   | `{char, char}` | 2 | 1 | 1 |
   | `{double, char}` | 16 | 8 | 8 |

   The order written is the order laid out, each member sits at its own
   alignment, and the size is rounded up to the whole object's. That is decision
   4, and it is not a choice about aesthetics: it is the layout a C compiler
   produces for the same sequence, which is what makes a future `extern` story
   about aggregates a statement about *one* rule instead of a second one.

2. **Rust's tuple member in debug info is `__0`** (measured with
   `llvm-dwarfdump` on a `rustc -g` binary: `DW_TAG_member`, `DW_AT_name ("__0")`).
   The market leader does not name the member `0` even though `t.0` is how the
   language spells it — because the *language's* access spelling is not the
   type's business, and because a debugger's pretty-printer owns a namespace a
   numeric name can collide with. Decision 10 follows it.

3. **What a debugger does with our record** (measured with clang at
   `-gdwarf-4` and GDB, which is the CU language this compiler emits):

   ```
   ptype g   →  struct {  int __0;  char __1; }
   print g   →  $1 = {__0 = 7, __1 = 1 '\001'}
   ```

   So the ceiling is `{__0 = 7, __1 = 1}` — the members visible, by the name we
   chose, with the values. A Rust binary prints `(7, true)` instead, because GDB
   has a *tuple* printer for `DW_LANG_Rust`; there is no equivalent for C99, and
   this record does not pretend otherwise. What it can promise is that the
   members, their names, their offsets and their types are all there, which is
   what `p t.0` (through a bound name for the member), a memory dump and a
   `ptype` need.

---

## The collision this feature has with a spelling this language already has

`literals.md` decision 6 justifies the hex float with no exponent by saying that
"a `.` followed by a hex digit cannot be anything else in this language — there
is no member whose name is a number", and decision 7 keeps C's leading-dot float
`.5` for the same reason. **Both of those sentences expire here**, and the third
one was already spent in anticipation of exactly this feature: decision 8 refuses
the *trailing* point (`5.` is `5` then `.`) with the words "a trailing point is
the one spelling whose meaning would change if the language grew member access".

Measured today, on the current lexer:

```
$ mincc lex dot.mx     # `return t.0;`
  1:35      34    1  Identifier      -      t
  1:36      35    2  FloatLiteral    -      .0
```

`t.0` is **one float token** — the access the market spells with a dot cannot be
written, and no later stage can repair it, because the information the parser
needs (that `.0` followed an expression) was thrown away by the lexer. Three
answers exist:

| | `t.0` | `.5` | What it costs |
| --- | --- | --- | --- |
| **A (recommended)** | yes | refused | A decimal literal must begin with a digit: `.5` becomes a refusal whose sentence is one character long to obey (`0.5`). `0x.8p3` **survives** — the *token* begins with `0`, and only a token that begins with `.` is affected. So the hex rule of decision 6 and its C-parity reason are untouched |
| **B** | no | yes | Members are reached as `t[0]`. Keeps every numeric spelling, but a component of a tuple is then spelled like an index of an array while a component of a `struct` (next) is spelled `.field` — one concept, two spellings, decided by which type it is, and the bracket gains a *constant* meaning on top of its runtime one |
| **C** | yes | yes | The lexer must look at the previous *significant* token to decide what `.0` is. It has never done that, and it does not stop at one bit: `a.0.1` reads the second `.1` after an `IntegerLiteral`, so the rule becomes "was the previous integer literal itself the position of a member", a *syntactic* fact living in the lexer. Every tool that reads the language — the highlighter, the LSP, a future formatter — would have to reproduce it |

**Recommendation: A**, and the reason is the one the language has used all along:
a dot after a value means "a component of this value", a number begins with a
digit, and the two cannot both be true of the same character. B is defensible
and is reversible *into* A later (a bracket form can be kept as sugar, a dot form
cannot be retrofitted once the tokens are gone); C is refused because it moves a
grammar rule into the scanner to save two keystrokes in a rare spelling.

This is the one decision in this record that **changes something already
shipped**: `literals.md` decision 7 loses its decimal half, one line of
`examples/005_literals.mx` changes, and one test of the literal scanner flips
from accept to refuse. The record that owns the rule (`literals.md`) gets a
pointer to this one, and the refusal carries the reason in its sentence.

---

## The five failure modes, and which decision answers each

This area has broken in the same five ways in every language that tried it:

1. **The product is not a value.** Go's multiple returns, C's `std::tie`, D's
   `Tuple!` before it was a language feature. *Answered by decisions 1 and 13*:
   the type exists in every type position, so the value exists everywhere a
   value exists.
2. **Layout is the compiler's private business.** Rust's `repr(Rust)` may reorder
   fields, Swift's tuples are outside the stable ABI, and both are right to do
   it — and both are why unpacking a tuple in a debugger or across a boundary is
   a documented non-guarantee. *Answered by decision 4*: the order written is the
   order stored, and decision 12 states exactly where the convention stops.
3. **Two values for "return two things".** A language with both multiple return
   syntax and a product type has two spellings for one concept and must define
   which one a caller gets. *Answered by decision 13*: there is no second
   spelling; `fn (i32, bool) f()` **is** the multiple return.
4. **The members are accessed by an index the compiler cannot see.** `t[i]` where
   `i` is a value has no meaning for a product — a tuple's members may have
   different types, so there is no uniform element type for the access to have.
   *Answered by decision 3*: the position is a compile-time constant, and a
   value in that position is its own diagnostic.
5. **Opening the tuple is a second, invisible kind of binding.** C++'s
   structured bindings are references to subobjects; a name that is secretly a
   reference has a lifetime, an aliasing behaviour and a debugger story that all
   differ from a variable, and none of it is visible in the source. *Answered by
   decision 6*: destructuring introduces real bindings, each a copy.

---

## The decisions

| # | Decision | The alternative, and why not |
| --- | --- | --- |
| **1** | **`(T, T, ...)` is a product type of arity ≥ 2.** It is written in every type position — a binding, a parameter, a return, an element, a member, a pointer target, a tuple member. | One member fewer would make the type family total (arity 1..N) at the cost of an identity: a product of one is its member, so `(T,)` and `T` would be two ids for one type, which is the rule `type.h` is built against ("two types with the same structure are the same `TypeId`"). Rust pays that price for a real reason — a generic wants `Vec<(T,)>` to be distinct from `Vec<T>` — and this language has no case for it: a generic that takes one thing writes `T` |
| **2** | **`(T)` in type position and `(a)` / `(a,)` in value position are refused, by name**: "a product of one member is its member: write `T`", "a group of one value is the value: write `a`". `()` is **not** a type: `fn () f()` is a function with no *parameters*, and the value side of "produces nothing" is spelled `void` (`never.md` is the other one). | A unit type (`()`), Rust's answer, would make one bracket read "nothing" in one position and "a product of nothing" in another, and this language already has the two words it needs (`void` for the return, an empty parameter list for none). Leaving `(T)` as a silent grouping would be worse than either: `let x: (i32)` and `let x: i32` would be two spellings of one type with nothing to choose between them |
| **3** | **Members are reached by position, at compile time: `t.0`, `t.1`.** A position that is not a literal is refused ("a tuple's member is chosen at compile time, and `i` is a value"), a position outside the arity is refused ("this tuple has 2 members, so `0` and `1` are its members"), and a name after the dot is refused ("a tuple's members have no names; `0` is the first"). | `t[i]` with a runtime `i` has no typing rule: `(i32, bool)` has no element type for `a[i]` to produce. D's `[i]` with a compile-time index keeps the array spelling for the member, which is the cost `B` above was rejected for; C++'s `get<0>(t)` is a library call for a language primitive and cannot be a language primitive (this library has no templates yet) |
| **4** | **Layout is C's, spelled out: members in the order written, each at its own alignment, the size rounded up to the largest member alignment.** `sizeOf` and `alignOf` answer from that rule, per target, and a test asserts the table measured above. | Rust's field reordering saves bytes in a struct full of mixed widths and makes the layout unnameable — no debugger, no C boundary, no byte-level test can pin it. This language has stated that what the compiler does to a program is a thing the programmer can predict; a layout that depends on a packing pass is not |
| **5** | **Every member must be an object (`isObject`), and nothing else.** `void`, `!`, a function type and a *deferred literal* are refused where the member is written, with the sentence naming which one and why (nothing to store). A tuple may hold a tuple, an array, a slice, a pointer or a scalar. | The array already has this rule (`arrays.md` decision 21) and the slice inherits it (`slices.md` decision 4). A second, laxer rule for members would let `(i32, void)` exist and would make `sizeOf` a question with no answer |
| **6** | **Destructuring introduces real bindings, not aliases**: `let (a, b) = t;` declares `a` and `b` as new objects, each a copy of its member, with the same mutability rules as any `let`. `_` in a position introduces no binding. | C++'s alias-to-subobject (measured above: invisible to the debugger and to the reader). A copy is what `let` already means, one sentence covers it, and an alias form — if the language ever wants one — is a visible spelling (`ref`), not a hidden property of destructuring |
| **7** | **A tuple literal is `(a, b)`, and the comma decides it.** `(` in expression position parses one expression; if the next significant token is `,` it is a tuple, otherwise the group is the parenthesised expression it has always been. No backtracking and no second look: a comma is not an expression token in this grammar, which is the same property that let `<...>` be a binder list in `parser.md`. | Pre-scanning for a comma (a `atTupleStart` beside `atTypedInitializer`) would answer the same question by looking ahead instead of by arriving, and would be a second place that knows what an expression may contain |
| **8** | **A literal's members are typed by their position**, under the rules already in force: a literal adapts to the member type it is being stored into, and a non-literal value never converts to a narrower member in silence. `(1, 2.0)` in a `(f64, f64)` position is two `f64`s; storing an `i32` variable into a `f64` member without a cast is an error. | The rule that already exists for a binding's initializer, a call's argument and an array's element (measured: `let v: f64 = i;` for an `i32` `i` is refused). A tuple is the fourth position of the same rule, and it must not become the first exception to it |
| **9** | **A tuple is an object, an aggregate, not a scalar.** `isObject` true, `isAggregate` true, `isScalar` false. Assignment, argument passing and `return` **copy** it. | The array's side of the divide, not the slice's: `[3]i32` copies and `[]i32` views (`arrays.md` decision 3, `slices.md` decision 2). A tuple is storage, so copying is the only answer that does not need an ownership rule this language has not made |
| **10** | **Debug info: an unnamed `DICompositeType` with one member per position, named `__0`, `__1`, …, offsets from decision 4.** `__` is the compiler's reserved prefix (`__minc__`, `__minc_check_fail` are the two spellings already in the tree), and Rust's measured choice is the same one. Built with the machinery `ir/debug.cc` already uses for the slice descriptor (`createStructType` + `createMemberType` with `OffsetInBits`). | The language's own access spelling (`0`, `1`) is not the type's business, and a debugger's pretty printer owns a namespace a numeric name can collide with. A `type` alias of a tuple gets its `DW_TAG_typedef` from `type_alias.md` unchanged, so `whatis` on a binding declared `Pair` still answers `Pair` |
| **11** | **The `ir` type is an unnamed `llvm::StructType`** (`StructType::get`, never `setName`), built from the members' lowered types, in order. | A name on a literal struct type would be a claim about identity: two different `(i32, bool)`s from two files would print as one name, and the day `struct S { … }` lands it would be a *lie* about which type is which. The tuple has no name in the language, so it has none in the module |
| **12** | **A tuple passes and returns by value under the compiler's internal convention** — the same treatment `[N]T` and `[]T` already get — and **`extern` refuses a tuple in a signature** (`sema-extern-aggregate`, the code that already refuses the two aggregates). `*(i32, bool)` is a pointer and is allowed; so is a tuple *inside* one of this compiler's own functions. | This is the promise the C boundary is waiting for and `slices.md` decision 13 already made it: the stable ABI for aggregates is *stated* as an internal convention and not promised to a foreign compiler, and Swift's measured position on the same question is the reason to say so out loud. `extern` refuses it so that no program can be written whose meaning depends on a layout nobody promised |
| **13** | **There is no multiple-return syntax, and no out-parameter idiom.** `fn (i32, bool) divmod(a: i32, b: i32) { return (a / b, a % b); }` is the whole feature, and the result is a value: it can be bound (`let t: (i32, bool) = divmod(7, 2);`), opened (`let (q, r) = divmod(7, 2);`), read by position (`t.0`), passed on (`fn i32 use(p: (i32, bool))`) and returned again. | Go's return *list* is a subtraction that cannot be undone later; the pointer-out-parameter is the C idiom this record is what lets the language stop needing. And a *second* spelling would mean two ways to return two things, with the checker obliged to keep them equal |
| **14** | **A tuple mentions no names of its own**, so it adds nothing to any namespace: resolution reaches its members through the same type reader that already reads a run of words, and a member that names a type alias resolves to that alias's type. Nothing new in the `Tag` namespace, nothing new in the published record. | A tuple type has no declaration site — it is written where it is used — so it has no scope, no name and no file-order question. The alternative (a per-tuple declaration node) would invent a second class of type declaration for the language's first structural type |
| **15** | **The type reader becomes recursive in exactly one place**: a `(` group inside a run is read as a comma-separated list of member runs, and the member count and nesting are bounded (`kMaxTupleMembers`, `kMaxNestingDepth`). The parser counts a balanced `(` group as one word of a run and consumes it whole, the way it already counts and consumes `[N]`. | A flat run with a nested group is exactly how `[N]T` is represented today, so a tuple is the *second* use of a mechanism that already exists instead of the first use of a new one. Parsing the members into child `Type` nodes would be a different representation for types than the rest of the language has, and the reader — which is shared by `resolve` and `sema` — would then need two shapes |
| **16** | **Component access is one postfix node for the language**, `FieldExpr`, whose operand type decides what the component is: a tuple's is a position, a `struct`'s (next) is a name. `t.0` and `s.field` are the same node with different member spellings. | A second node for each ("TupleIndexExpr", "MemberExpr") would put the same question — *is the component a place, is it addressable, what is its type* — in two switches today and in three after arrays-of-structs. The parser's postfix loop gains `.` once |
| **17** | **No structural equality, no ordering, no hashing, no printing, no `len`**: `t == u` on two tuples is refused ("a tuple has no equality: compare the members"), and the arity is a *type* property with no operator that reads it yet. | `==` on a product would have to mean "every member `==`" and would silently disagree with the language's own refusal of `s1 == s2` on two `str`s (`Equatable` is not universal). The operators that *can* be given per type come from a declared interface, and interfaces are not in the language yet; a built-in rule now is a rule the interface has to unlearn later. `len` is the members-on-types facility (`slices.md` decision 15) and arrives with `struct` |
| **18** | **A tuple is stored nowhere the compiler cannot describe**: it is not a variadic argument (no default promotion for an aggregate), not a `#define`-time constant, and not a global initializer kind of its own — a global tuple is laid out by the same rule and initialized member by member. | The variadic case is the C-boundary case again and is refused for the boundary's reason. A second initializer path for globals is what `arrays.md` decision 27 spent a pass avoiding |
| **19** | **A cast to a tuple type is refused, by name**: `(i32, bool)x` gets "a cast's type is one type, not a product: cast the members, or take the value whole". A cast *from* a tuple is refused for the same reason. | The two parentheses forms are indistinguishable to a reader (`(i32, bool)` is a type there and a literal here), and no conversion between two tuples exists in this language — members may differ in every position. Refusing it by name is the difference between a reader learning the rule and a reader seeing `expected ;` |
| **20** | **Diagnostics carry the member position, not just the tuple**: "this tuple has 3 members, so `0`, `1` and `2` are its members, and `3` is out of range" (a sentence that already exists in the array's shape), and a destructuring count mismatch names both numbers. Every new message states the fix. | `arrays.md`'s bound message and `type_alias.md`'s `(aka …)` are the house style, and a diagnostic that says "invalid tuple index" costs a reader a round trip to the documentation |

---

## The surface

```minc
// --- the type, in every type position ----------------------------------------

let p: (i32, bool) = (7, true);        // a binding
fn f(pair: (f64, f64)) { }             // a parameter
fn (i32, bool) divmod(a: i32, b: i32)  // a return: this *is* multiple return
type Rec = (i32, f64);                 // an alias, unchanged from type_alias.md
let q: ((i32, i32), bool) = ((1, 2), true);   // nesting
let a: [3](i32, i32) = ...;            // as an element
let s: *(i32, bool) = &p;              // pointed at

// --- the value ---------------------------------------------------------------

let t = (1, 2.5);                      // members typed by position
let u = (divmod(7, 2), t);             // a value in a member position
return (0, false);                     // the literal of a return type

// --- reading it --------------------------------------------------------------

let q1 = t.0;                          // by position, compile time
let q2 = t.1;
t.0 = 9;                               // a place, when the tuple is one
let (first, second) = t;               // real bindings: copies (decision 6)
let (only, _) = t;                     // `_` binds nothing

// --- a chain of two reads, written apart -------------------------------------

let nested = ((1, 2), true);
let inner = nested.0 .1;               // = 2; the space is the separator
let same = (nested.0).1;               // the same member, visibly grouped
// nested.0.1   ✓ refused: `0.1` is one *number* to the scanner, and the sentence
//                names both fixes. A member chain is a grammar shape and a
//                fraction is a lexical one, so the scanner is where it is refused
//                (see *As built*)

// --- and what it refuses -----------------------------------------------------

// let x: (i32) = 1;         ✓ refused: a product of one member is its member
// let y = (1,);             ✓ refused: a group of one value is the value
// let z: () = 1;            ✓ refused: `()` is an empty parameter list, not a type
// let w = (1, void);        ✓ refused: no value to store
// t.i                       ✓ refused: a member is chosen at compile time
// t.2                       ✓ refused: this tuple has 2 members
// t.len                     ✓ refused: a tuple's members have no names
// t[0]                      ✓ refused: brackets index an array or a view
// t == u                    ✓ refused: compare the members
// (i32, bool)x              ✓ refused: a cast's type is one type
// extern fn i32 g(p: (i32, bool));   ✓ refused: across the C boundary
```

---

## Where it lands

| Stage | What changes |
| --- | --- |
| `lex` | **one** change, and it is the collision above: a decimal numeric literal must begin with a digit, so `.5` is refused with a sentence. Nothing else: `Dot` exists, and `t.0` lexes as `Identifier` `Dot` `IntegerLiteral` the moment `.0` stops being a float |
| `parse` | `parseType`'s run gains a balanced `(` group (with a `ParseErrorCode` for the unclosed one, the peer of `ExpectedArrayCountClose`); the postfix loop gains `.` → `FieldExpr`; the `(` case in `parsePrimary` gains the tuple branch (decision 7); a cast scan does not gain the comma group, so `(i32, bool)x` reaches the dedicated refusal. New syntax kinds: `TupleExpr`, `FieldExpr` |
| `ast` | lowering of the two nodes, and validation: `FieldExpr` requires a component; `TupleExpr` requires **two or more** elements and refuses a `!` element the way an array refuses it. The `Type` node is unchanged — its run holds the group |
| `resolve` | nothing (decision 14). A test asserts that a member naming an alias resolves through the *same* table the alias published |
| `sema` | the `Tuple` kind: `TypeStore::tuple(...)`, `membersOf`, the predicates (`isObject`, `isAggregate`, `layoutOf`), `spelling`, `sizeOf`, `alignOf`, the literal's typing, `FieldExpr`'s position check and place-ness, destructuring's bindings, the cast refusals, the variadic and `extern` refusals, and the dump line |
| `ir` | the unnamed `StructType`, the aggregate's value path (build, extract, insert, copy), the internal calling convention, and the `DICompositeType` of decision 10 |
| `backend` | nothing beyond what an aggregate already needs: the target's aggregate ABI is asked once, and the debug record rides the existing `-g` path |
| `driver` | nothing new: `check`, `build` and `run` already carry a unit with aggregates |

### What has landed

The seven steps landed. Five places where the implementation is more specific than
the plan above, one *limitation* the plan did not see, and one rule that moved from
the plan's head to the debug record. Each is a fact a later change has to keep.

- **The chain of member reads is written apart: `t.0 .1`, not `t.0.1`.** The plan
  predicted the collision on the *left* of the dot (`.5`) and not the one on the
  *right*: `t.0.1` is `0.1` to the scanner — one fraction — and the two readings are
  the same token, so no stage can tell them apart without splitting a token. The
  scanner is the right place to refuse it (a member chain is a *grammar* shape, and
  the number is a *lexical* one) and the parser names both fixes in one sentence:
  `t.0 .1` or `(t.0).1`. Both work and both are tested. What this is *not* is
  silence: the parser wraps the offending token in an `Error` node, and the checker
  answers a member inside a reported region with the poison and no second sentence.
  Rust pays for the same spelling with a token split in the parser; this record
  declines the split, because a resolver that re-lexes is a second scanner, and the
  cost is a space in a chain that is three characters long.
- **The `Type` node of a product is a *run*, and the run is recursive.** The plan
  said "the type position gains a group"; what landed is `parseTypeRun` split out
  of `parseType`, so a group's member is a run and a group may be a member of one.
  One production, read by both, which is what makes `[2](i32, i32)` and
  `(i32, (bool, u8))` fall out instead of needing arms.
- **`FieldExpr` is checked by the *record* of its base, not by asking again.** A
  member of a place is a place and a member of a value is a value, and the answer
  is the base's own `isLvalue` — copied, the same way `checkArrayLiteral` asks its
  context. The lowering then reads that answer instead of re-deriving it, so the
  two stages cannot disagree about whether `t.0 = 9` is a store.
- **A member access writes an access record** (`recordAccess`), exactly as an array
  subscript does: null and alignment guards in the checked build, and **no bounds
  test**, because a position is a constant the checker already bounded against the
  arity. The extent is still recorded (`Count`, the arity) — it is what the dump
  and a future shadow memory read. This is what makes `t.0` as guarded as `a[0]`
  and never more.
- **`placeProvenanceOf` is the *place* question, and a member asks it.** The
  function was `arrayProvenanceOf`; a product member is the second caller of
  "which object is this access inside", and a name that says *array* while being
  asked about a product is how a second rule gets written by accident.
- **The debug record is one `DW_TAG_structure_type` with no name and `__0`…
  members**, which is decision 14 as written — and it is worth recording what it
  prints, because a reader checks this with a tool:
  `gdb -batch -ex 'ptype p'` on a `(i32, bool)` binding answers
  `type = struct { i32 __0; bool __1; }`. Rust, measured on the same machine,
  reaches DWARF with `DW_AT_name ("__0")` and clang's anonymous struct prints
  `struct { int __0; char __1; }`, so the shape is the market's and not this
  compiler's invention.
- **A pattern at file scope is refused by name** (`ast-pattern-at-file-scope`). The
  plan made the pattern a block-scope form and did not say what happens at the file
  scope; the answer is a sentence with the fix (`const W = 16; const H = 9;`),
  because a file-scope declaration is one object with one name and the item table —
  what every stage reads an item through — records one name per declaration. This is
  the same boundary `extern let` sits behind, and it is written down rather than
  discovered by a program that silently declares nothing.
- **A `const` pattern whose value is a *literal* publishes its members' values.**
  The plan did not say whether `const (W, H) = (16, 9);` makes `W` usable where a
  constant is required. It does, one value per name, through the same table a single
  `const` fills — and only for the literal case, because a member of a value that was
  computed has no value this stage can name, which is exactly what a constant must
  have. A `const` pattern over a non-literal is therefore accepted and its names are
  not compile-time constants, which is reported where one is required.

---

## The implementation, in the order it lands

Each step ends with its tests green and the corpus still passing, because every
step after the second is visible in a `check` of an existing example.

1. **The lexer rule** (decision A): a decimal literal beginning with `.` is
   refused, `literals.md` decision 7 is amended with a pointer here, and
   `005_literals.mx` drops its leading dot. Its own tests: `.5` refused with the
   sentence, `0.5` and `0x.8p3` and `1.` (already `1` `.`) unchanged.
2. **`TypeStore::tuple` and the predicates**, with the store's own tests: the
   same members intern to one id, a different order is a different type, arity-1
   and arity-0 are unconstructible from the reader, the budget is checked *before*
   the insert, and `hashOf`/`equal` need **no** change (they already mix and
   compare the member sequence for every kind). One shared private
   `internSequence` is where a tuple and a function's parameters stop being two
   copies of the same three-part rule; the two fields get the neutral name the
   two kinds share (`firstPart`/`partCount`, the arena `parts_`).
3. **`parse`**: the type group, `TupleExpr`, `FieldExpr` and the unclosed-group
   error, with parser tests for each shape and for the refusals of decisions 2
   and 19.
4. **`ast` + `validate`**, with the two new nodes and the tuple's validation.
5. **`sema`**: typing, access, destructuring, and every refusal — one test per
   decision row, in the harness's own style (`errors_test.cc` for the messages,
   `type_test.cc` for the identities and the layout table).
6. **`ir`** and the debug record, with the checks this record's claims can be
   measured by: the module's struct type is unnamed and the member order is the
   declaration order; `llvm-dwarfdump` shows `DW_TAG_structure_type` with
   `__0`…; a two-member return is one aggregate in registers.
7. **`examples/020_tuples.mx`**, then the docs (`docs/architecture.md`,
   `docs/roadmap.md`, the website's types page), then the gates: `format`, `ci`,
   `sanitize`, `tidy`, `examples`, `docs`.

The generics record is written **after** this one, and the reason is not
convenience: a binder list is a sequence of `(parameter, type)` pairs, which is a
product, and the store gains arity-unknown interning here. What this record owes
generics is exactly one thing, and it is a *bound*: `fn (T, K) makePair<T, K>(…)`
must be writable the day binders land, which is why the member list may hold a
name that is not yet a type (a binder) without the tuple's own rules changing.

---

## The hazards, and what each one already cost someone

| Hazard | Who it cost, and what this record does instead |
| --- | --- |
| **Two values for "return two things"** | Go chose the return list and cannot withdraw it; C++ has `std::pair`, `std::tuple` and `std::tie` for the same job. Decision 13 keeps one spelling and makes it a type |
| **A hidden alias** | C++'s structured bindings are references to subobjects; measured on this machine, GDB prints the declared type as `tuple_element<…>::type &&` and the value is unreachable through it. Decision 6 introduces copies, and a copy is a binding like any other |
| **A layout a debugger cannot follow** | Rust's `repr(Rust)` reorders fields to pack them; the language documents that you cannot rely on the order. Decision 4 keeps the written order, and decision 10's test pins the offsets the DIE carries |
| **An aggregate across a boundary nobody described** | Swift's `(T, U)` is not ABI-stable across a module, and a C++ `struct` passed by value between two compilers of different versions is the same class of bug. Decision 12 states the convention's edge (`extern`) instead of discovering it at link time |
| **A member index that is a value** | Java and C# arrays make an index a runtime value and template languages make a member a compile-time one; mixing them produces either a bounds check that can never fail or a message that never fires. Decision 3 refuses the value in that position by name |
| **A suffix or bracket that changes meaning with context** | The leading-dot collision above is this hazard in miniature, and the fix is to give each character one meaning — which is what decisions 7 (`(`), 3 (`.`) and 16 do |

---

## What this makes impossible

- **A product of one member, or of none.** `(T,)` and `()` do not exist as types
  (decision 2). A generic that wants "one thing, but as a product" cannot be
  written; a tuple in a generic parameter list is a product of what it holds.
- **`t[i]` for a runtime `i`.** A product is not a sequence and has no element
  type. Code that wants to walk a tuple by position is code that wanted an array
  of one type.
- **`t == u`, `t < u`, printing a tuple, using one as a map key.** All refused
  (decision 17) until an interface says what they mean for the type at hand.
- **A tuple across the C boundary in either direction** (decision 12), and a
  tuple as a variadic argument (decision 18). `*T` is the door: a pointer to a
  tuple may cross, because a pointer's representation does not depend on the
  aggregate's layout.
- **A named product.** Members with names are a `struct`, and that is the next
  record: the naming, the nominal identity and the `impl` story are its subject,
  not this one's.
- **`.5` as a decimal float literal** (the collision above). `0.5` and `0x.8p3`
  are unaffected.

---

## What is deliberately not decided here

- **Named members.** That is `struct`, and the two must not be one type with a
  flag: nominal identity is the difference that makes a `struct` a `struct`.
- **Trailing commas.** `(i32, bool,)` is neither accepted nor refused by this
  record; it belongs with the list grammar (`arrays.md`'s typed initializer has
  the same open question), and the answer should be one rule for every list.
- **A `Tuple` type constructor for generic code** (`Tuple<T, U>`, what
  `std.meta.Tuple` is in Zig). It becomes writable when binders land, and its
  spelling is the generics record's decision.
- **Structured *patterns* beyond `let`** — matching, `switch`, a tuple in an
  `if` condition. Destructuring in `let` (decision 6) is the whole of this
  record's pattern story; a pattern language is its own record.
- **Packing and explicit alignment** (`@packed`, `align(8)`), and a
  `repr(C)`-versus-`repr(minc)` distinction. There is one layout (decision 4) and
  no syntax to ask for another.
- **Tuples as constants (`const T = (1, 2);`) and in a `#define`.** The global
  path is decision 18's; the compile-time-evaluable path is the constant
  evaluator's, which is not in the tree.
- **A lifetime rule for a tuple that holds a slice.** `slices.md` decision 21
  already documents that a view's lifetime is unchecked and names the layer that
  will check it; a tuple does not change that, and it does not fix it either.

---

## How the claims above are checked

Every claim in this record is one of four kinds, and each has a kind of check:

1. **An identity or a layout** → a `sema` test: the store interns `(i32, bool)`
   to one id and `(bool, i32)` to another; `sizeOf`/`alignOf` match the measured
   table for the members this compiler has, per target triple; `(i32, bool)` is
   an object, an aggregate and not a scalar.
2. **A sentence** → an `errors_test.cc` case per refusal row: the arity-1
   product, the empty type, the non-literal position, the out-of-range position,
   the named member, the bracket on a tuple, the equality, the cast, the
   `extern` signature, the variadic argument, the `void` member, the
   destructuring count mismatch.
3. **What the compiler produces** → an `ir` test on the module text (the struct
   type is unnamed, members in order) and a `llvm-dwarfdump` assertion on the
   emitted object (`DW_TAG_structure_type`, `__0`…, the offsets of decision 4),
   following the shape of `type_alias.md`'s IR tests.
4. **What a program does** → an example (`020_tuples.mx`) that returns a tuple,
   reads a member, destructures one, stores one in an array, passes one by
   pointer, and asserts the layout with `sizeof` — run by `mincc run`, so the
   claims are the compiler's own and not a comment.

The market facts this record quotes are attributed in the table above; the three
*local* measurements (C's layout table, GDB's rendering of an unnamed struct with
`__0` members, and the lexer's `.0` token) are reproducible with the commands
recorded in this session, and the third one is the reason the lexer rule is the
first item of the plan: it is the only one of the twenty decisions that changes
something a program could already write.

---

## References

- The Rust Reference, *Tuple types* (`( ( Type , )+ Type? )`, numeric fields,
  "1-ary tuples require a comma", structural equivalence) and *Type layout*
  (`repr(Rust)` field reordering, `repr(C)` declaration order, size a multiple of
  alignment).
- cppreference, *Structured binding declaration* ("a structured binding is an
  alias to an existing object"; the uniquely-named variable it introduces).
- Swift, *ABI Stability Manifesto* (internal layout may change; what is promised
  is the public interface) — the shape of decision 12's boundary.
- The Go FAQ and the language's own specification on multiple return values (no
  tuple type) — decision 13's alternative.
- Zig documentation: anonymous structs with `"0"`, `"1"` fields and
  `std.meta.Tuple` — decision 10's representation, reached independently.
- C23 6.7.2.1 (structure and union specifiers) and 6.4.4.4 (floating constants)
  for the layout rule of decision 4 and the `.5` of the collision.
- Measured locally in this session: the C layout table (clang + `offsetof`);
  `llvm-dwarfdump` on a `rustc -g` binary (`DW_AT_name ("__0")`); GDB on an
  `-gdwarf-4` object under a C99 CU (`{__0 = 7, __1 = 1 '\001'}`); `mincc lex` on
  `return t.0;` (`FloatLiteral .0`).
- In-tree: `literals.md` decisions 6–8 (the leading and trailing dot — the first
  two are amended here, the third anticipated this record), `arrays.md` decisions
  3, 6, 21, 27, `slices.md` decisions 2, 4, 13, 15, 21, `type_alias.md` (the
  `DW_TAG_typedef` a tuple alias rides), `memory.md` (what a pointer to one
  means), `never.md` (`!` as a member), `parser.md` (the `<...>` binder rule this
  record's decision 7 shares a reason with).
