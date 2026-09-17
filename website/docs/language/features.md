---
title: Feature checklist
---

# Feature checklist

What `.mx` is, and what it will be. This is the whole surface of the language in
one place — the authoritative list of what is **decided**, what is **planned**,
and what is still an **open** question. It is not the build order: the order the
compiler is built in is `docs/roadmap.md` in the repository.

Legend: `[x]` **decided** — the design is fixed · `[ ]` **planned** · `[?]`
**open** — needs a decision.

"Decided" is about the *design*, not about whether the compiler implements it
yet. Implementation status is `docs/roadmap.md`; on this site, a page that
describes something the compiler does not do yet says so in a **Not implemented
yet** note.

Start from the project's identity: a *minimal C* dialect — C is the baseline, not
a stripped-down imitation of it — plus a small, documented set of extras, and
full C interoperability in both directions.

## Syntax and files

- [x] `.mx` source files compiled by `mincc`
- [x] `fn` function declaration form (`fn i32 main() { ... }`), as in
      `examples/001_main_func.mx`
- [x] `extern fn` declarations — a function defined elsewhere (another unit, a
      library, the C runtime): `extern fn i32 puts(s: str);`, as in
      `examples/010_extern.mx`
- [x] Variadic declarations (`extern fn i32 printf(fmt: str, ...);`), with the
      ABI's default argument promotions at the call site, as in
      `examples/011_variadics.mx`; a *definition* may not be variadic
      (`va_start` does not exist yet)
- [x] Block statements and `return`
- [x] `//` line comments
- [x] `let` bindings are **mutable**, with a colon type annotation or
      inference (`let x: i32 = 0;` / `let y = 10;`), as in
      `examples/002_variables.mx`
- [x] `const` bindings are **immutable** and have the same shape as `let`:
      `const x: i32 = 0;` or `const y = 10;` — no separate `mut`/`var`
- [ ] C-style function and declaration syntax alongside `fn`
- [ ] Doc comments attached to declarations
- [ ] Attributes/annotations on declarations `[?]`

## Types

Primitive names state their width instead of inheriting C's
implementation-defined ones. Both this set and the C spellings below are
first-class types; the examples use the primitive names. See
[Types](/language/types).

- [x] Signed integers: `i8`, `i16`, `i32`, `i64`, `i128`, `isize`
- [x] Unsigned integers: `u8`, `u16`, `u32`, `u64`, `u128`, `usize`
- [x] Floats: `f32`, `f64`, `f80`
- [x] `bool`
- [x] `char` — a distinct 8-bit byte type represented as `u8`, always
      unsigned (see *C-compatible type names*)
- [x] `str` — NUL-terminated, C-like; a scalar type, not `char*` yet
- [x] `void` — a return type and (later) `void*`; never a value type: no
      object has it and no arithmetic is defined on it
- [x] `!` — the bottom type, and a return type only: a function declared
      `fn ! name(...)` never returns to its caller. A call to one *is* an
      expression of type `!`, which converts into every other type, so it can be
      an argument, an assigned value or the arm of a `?:`, and the code after it
      is unreachable. The body is checked (a reachable `return`, or a body that
      can reach its end, is an error) and the promise is never inferred. See
      [The bottom type](/language/never)
- [x] Pointers — deliberately complete and C-level, see
      [Pointers and raw memory](#pointers-and-raw-memory); stage one of the
      memory model is what the compiler implements today
- [x] Fixed-size arrays — `[N]T` with the count part of the type, no decay,
      value semantics, both literal forms (`[1, 2, 3]` typed by its context,
      `[3]i32{1, 2, 3}` complete, `[_]u8{...}` with the count from the elements,
      `[64]u8{0; 64}` as a fill), element access with the constant bounds check,
      the by-value copy, and **file scope** — `const TABLE = [_]i32{1, 2, 3};` is
      a table like any local one. See [Arrays](/language/arrays)
- [x] Slices (pointer + length) `[]T` — a `{ptr, len}` **view** taken from an
      array with `a[l..r]` / `a[l..]` / `a[..r]` / `a[..]`, indexed with its own
      `0`, writable through, passed and returned by value, refused at an `extern`
      boundary. See [Slices](/language/slices)
- [x] Tuples `(T, T, ...)` — a structural product of two or more types: a
      return type (`fn (i32, bool) divmod(...)`), a binding, a parameter, an
      element; access by position at compile time (`t.0`), destructuring into
      real bindings (`let (q, r) = divmod(7, 2);`, `_` for a skipped member),
      C's field order, and an unnamed composite record in `-g`. A chain of two
      reads is written apart — `t.0 .1` — because `0.1` is one number to the
      scanner. Refused across the C boundary: the aggregate's layout is this
      compiler's internal convention. See
      [`docs/architectures/tuples.md`](https://github.com/carlosdlw/mincplus/blob/main/docs/architectures/tuples.md);
      generics come **after** it, because a binder list is itself a sequence of
      pairs and the store gained arity-unknown interning here
- [x] Generics `<T>` on a `type` and on a function — a use **substitutes** into
      the declaration, so `Pair<i32, bool>` *is* `(i32, bool)` and the check is an
      identity; a generic function is a template whose body is checked once,
      instantiated once per distinct argument list. A binder list after the name
      being declared (`fn T identity<T>(value: T)`, `type Pair<T, K> = (T, K);`),
      plain `<...>` in a type position and `::<...>` at a call site, inference
      from the arguments and from what the context wants, one function plus one
      debug record per instance (`break identity` stops in all of them), and a
      doubling structure refused by name rather than built. See
      [`docs/architectures/generics.md`](https://github.com/carlosdlw/mincplus/blob/main/docs/architectures/generics.md)
- [x] Constraints `T: Class` — seven classes (`Any`, `Eq`, `Ordered`, `Number`,
      `Integer`, `Float`, `Pointer`), each a set of types *and* a promise of
      operations. `fn T twice<T: Number>(x: T)` is checked once and accepts every
      type in the class; an operation the class does not grant is one sentence
      naming the class to write; a type argument outside the class is refused at
      the call; and a literal in a binder's position is decided by the class
      (`Integer` takes `1`, `Float` takes `1.0`). See
      [Generics](/language/generics)
- [ ] `struct`
- [ ] `union`
- [ ] `enum` constants and tagged unions `[?]`
- [ ] Function types and function pointers
- [x] Type aliases `type Name = T;` — a name for an existing type, in both
      positions (file scope, where order does not matter, and a block, where it
      does). The name is transparent: `Meters` and `f64` are one type, so no rule
      anywhere knows the difference, and `-g` still answers `whatis` with the name
      the source wrote. See [Types](/language/types#type-aliases)
- [x] `const` bindings (see *Syntax and files*); immutability is a binding
      property, not a type qualifier yet
- [ ] Optional/nullable types and null safety `[?]`
- [x] Generics / parametric types — see *Generics* above; user-declared
      constraints (`interface`, a concept) stay open

`f80` is the x87 80-bit extended format. It is in the set because that is what
C's `long double` is on System V AMD64, and it is the one type whose existence is
a property of the **machine**: it is x86's arithmetic, so `x86_64` and `i386`
have it and AArch64 and RISC-V do not. `mincc check` is where that is decided —
the type-specifier reader refuses the word with `sema-malformed-type`, naming the
triple and the spelling to use instead — because a program the checker accepts is
a program the rest of the pipeline must compile. `long double` is the portable
spelling, and on a target without x87 it is the format that target does state
(IEEE binary128 on AArch64 Linux, a `double` under MSVC).

### C-compatible type names

The C spellings are a second, interchangeable way to name the same types, so
`.mx` code can be written in C style and C headers map onto it cleanly.

- [x] `int`, `uint`, `short`, `long`, `signed`, `unsigned`, `float`, `double`
- [x] Multi-word forms: `long int`, `long long int`, `unsigned long long int`,
      `short int`, `signed char`, `unsigned int`, ...
- [x] Widths follow the **target ABI**, not a fixed table, so linking against C
      behaves as the ABI requires
- [x] Interchangeable with the primitive names in declarations
- [ ] A portability lint (off by default) for ABI-dependent C spellings in code
      built for more than one target

Mapping on the current interop target (System V AMD64, LP64):

| C spelling | `.mx` type | Note |
| --- | --- | --- |
| `char` | `char` (`u8`) | raw byte 0..255; signedness is not inherited |
| `signed char` | `i8` | the signed byte escape hatch |
| `unsigned char` | `u8` | |
| `short` | `i16` | |
| `int` | `i32` | |
| `long`, `long long` | `i64` | both 64-bit on LP64 |
| `unsigned int`, `unsigned long` | `u32`, `u64` | |
| `float`, `double` | `f32`, `f64` | |
| `long double` | the target's extended float | `f80` on x86, IEEE binary128 on AArch64 Linux, `f64` under MSVC |
| `size_t` / `ssize_t`, `ptrdiff_t` | `usize` / `isize` | pointer-sized |
| `__int128` / `unsigned __int128` | `i128` / `u128` | compiler extension in C |

**`char` is unsigned — decided.** `.mx` `char` is a distinct 8-bit type whose
representation is `u8`; it never inherits C's implementation-defined
signedness (signed on x86-64, unsigned on ARM). The reason is that `str` is a
NUL-terminated byte string and UTF-8 code units are 0..255, so sign-extending
a byte is a bug generator rather than a feature. Consequences:

- C `char` maps to `.mx` `char`: the value crossing the boundary is the raw
  byte, so `str` indexing agrees with C bytes in both directions.
- C `signed char` maps to `i8`. Code that depends on C's signed-`char`
  behavior must say `i8` explicitly; the sign is never implicit.
- `char` stays **distinct** from `i8`/`u8`, mirroring C, so `char*` and `u8*`
  are not silently interchangeable and conversions between them are explicit.

**`long`, `unsigned long`, and `long double` are ABI-dependent — accepted.**
That is inherent to C's names, not a `.mx` quirk: `long` is 64-bit on LP64
(System V AMD64) and 32-bit on LLP64 (Windows x64), and `long double` is
80-bit on SysV but 64-bit under MSVC. The resolution is to keep the C
spellings for C-facing declarations and to treat them as non-portable by
definition; code that must compile unchanged for more than one target uses the
fixed-width primitives (`i32`, `i64`, `isize`) instead. Note the contrast with
`isize`/`usize`: they are pointer-sized too, but they are *named* for that
intent, which is exactly what makes them portable where `long` is not.

### Conversions and literal typing

The design record is `docs/architectures/sema.md` (`#conversions`), and these are
the language decisions it makes. They are *semantics*, so they are recorded here
as well.

- [x] A type is an **identity, not a spelling**: `i32`, `int` and `signed int`
      are one type, so the C spellings are interchangeable rather than merely
      accepted
- [x] **Narrowing is implicit at assignment** (initializer, assignment,
      argument, `return`), as in C, and a documented example must compile. A
      `-Wconversion` lint (off by default) reports it; a **cast** is how a
      program asks for the narrowing on purpose
- [x] **Casts**, in three spellings with one meaning — `x as T`, `(T)x`, and the
      literal suffix (`10u8`, `12f`). Pointer to integer and integer to pointer
      are casts too, and they *are* the model's `expose` /
      `with_exposed_provenance`, counted by `-Wprovenance`. See
      [Casts](/language/expressions#casts)
- [x] **Integer and float literals are context-typed**: `let x: u8 = 255;` is a
      `u8` with no conversion, and `let x = 7;` / `let y = 1.5;` default to
      `i32` / `f64`. A literal that does not fit the type its context gives it is
      an **error**, not a silent truncation (unlike C)
- [x] `char` literals are `char` and string literals are `str` — never `i32`
      or a byte array
- [x] Integer promotions follow C (smaller than `i32` widens to `i32`) and the
      usual arithmetic conversions follow C17 6.3.1.8, with ranks by width
- [x] **An integer and a float never convert into each other**, in either
      direction: the class of a number is the class of its *spelling* (`1` is an
      integer, `1.0` is a float), so `let a: f64 = 1;` and `1 + 2.0` are errors
      rather than silent widenings, and crossing is a cast. This is the one place
      the arithmetic departs from C, and the departure is on purpose: C's answer
      is a value the reader did not write, with a rounding they cannot see
- [x] **A condition must be `bool`** — an arithmetic value does not implicitly
      convert (`x != 0` is the explicit form). C's "any scalar is a condition"
      is rejected
- [x] `bool` is **not arithmetic**: `!`, `&&`, `||`, `==`, `!=` are defined; a
      promotion to `int` as in C is not
- [x] `str` is **not arithmetic** and `==`/`!=` on it are **refused**, because
      C's `s1 == s2` compares addresses — content comparison is a library call
- [x] A `const` name is an lvalue but **not modifiable**; assigning to it or
      applying `++`/`--` is an error
- [x] A non-`void` function that can reach its end without returning a value is
      an error, not C's undefined behavior with a warning
- [x] `main`, when declared, must be `fn i32 main()`

## Pointers and raw memory

Pointer control is complete and unchecked by design. `minc+` has to express
allocators, buffers, device registers, and anything else that sits on top of
the C ABI, so raw pointers are a first-class language feature and not a
hidden escape hatch.

The rules underneath it — what an object is, what a pointer carries, what
aliasing may be assumed, alignment, lifetime, and what counts as a violation
rather than as undefined behavior — are decided in `docs/architectures/memory.md`
in the repository, and **stage one of that model is implemented**: `*T`, `&x`,
`*p`, `p[i]`, the stepping, the comparison, `null` and `*void`, with the
per-access provenance record the checker publishes for the lowering to read.
Everything below is what lands on top of that model. See
[Pointers](/language/pointers) and [The memory model](/language/memory-model).

**Pointer types**

- [x] Pointers to any object type, at any depth (`**T`, `***T`)
- [ ] Function pointers, including calling through them
- [x] `void*` (untyped) as `*void`; pointers to incomplete/opaque types still to
      come
- [ ] Qualifiers on the pointee, if `const`/`volatile` land `[?]`
- [x] Pointer syntax: `*T` in type position, a positional prefix constructor that
      cannot collide with multiplication

**Addressing and access**

- [x] Address-of `&` and dereference `*`
- [ ] Member access through a pointer
- [x] Arrays and `&a[0]` — **there is no implicit decay.** An array is a value
      and a pointer is named (`&a[0]` is a `*i32`, `&a` is a `*[4]i32`), so
      `sizeof` cannot lie about a parameter and a bound can be checked
- [x] Raw loads and stores through a pointer; type punning still to come
- [x] **Slices** (`[]T`): a `{ptr, len}` **view** with no capacity and no literal,
      taken with `a[l..r]` / `a[l..]` / `a[..r]` / `a[..]` from an array, another
      view or a pointer (where both bounds are written), its own index `0`,
      writable through, passed and returned by value, refused at an `extern`
      boundary — see [Slices](/language/slices)
- [ ] `len(x)` and `sizeof(x)`: the reading operators, each awaiting its own form
      in the grammar (a call whose argument is a place, a type in an expression)

**Arithmetic and comparison**

- [x] `p + n`, `p - n`, `p1 - p2`, `++p` / `--p`, `p[i]`
- [x] Element-based scaling by `sizeof(*p)`, not byte stepping
- [x] Pointer comparison and ordering
- [x] `isize` as the pointer-difference type

**Conversions and casts**

- [x] Pointer to integer and integer to pointer, sized by `usize`/`isize`. They
      are **cast-only** (`p as usize`, `addr as *u8`), because that cast *is* the
      model's `expose` / `with_exposed_provenance`: never implicit, and counted
      by `-Wprovenance`. The integer side must be a **value**: a constant address
      is `sema-address-from-constant` (zero included — the null address is
      spelled `null`, or `null as str` for a `str`)
- [x] Reinterpret cast between pointer types (`p as *u8`): with opaque pointers a
      type change and no instruction at all
- [x] `*void` to and from any object pointer, **implicit in both directions**
      (writable as a cast as well)
- [x] `str` to and from `*u8` by cast, and a byte view over any object
      (`(*u8)&x`, `(*u8)p`)
- [ ] Function pointer to and from `void*` `[?]`

**Aliasing, alignment, and optimization**

- [ ] Every object is byte-addressable; `u8*` and `char*` may alias anything
- [ ] `restrict` / noalias annotation `[?]`
- [ ] Volatile accesses for memory-mapped I/O `[?]`
- [ ] Aligned vs unaligned access guarantees `[?]`
- [x] The pointer provenance/aliasing rules the optimizer may assume — decided,
      and published per access by the checker as `Object` or `Foreign`; the
      module scan that enforces the closed list lands with the lowering

**Safety model**

- [x] Raw pointers are unchecked and need no keyword — no borrow checker, but
      every access carries a written obligation (an access is **defined** where
      it stays inside the object; it is the obligation that is the programmer's)
- [x] **The checked build** (`-fcheck`, on at `-O0`): a null dereference, a
      misaligned access, and an index outside an array's count or a view's length
      are traps that print the site and stop — `memory-null`,
      `memory-misaligned`, `memory-out-of-bounds`. The release build emits none of
      them ([the memory model](/language/memory-model#what-a-checked-build-reports))
- [ ] Optional non-null pointer type `[?]`
- [ ] A *pointer*'s own extent: the object it names is not in this unit, so the
      check needs the shadow memory above

## Declarations and modules

- [x] Top-level functions, including `main`
- [x] Function parameters, written `name: type` — the same shape as `let`, and
      only that: the C order `type name` is rejected rather than guessed at
- [x] File-scope `const` — a typed constant of the unit:
      `const maxUsers: i32 = 4096;`, with the same shape as a block-scope
      binding (annotation or inference)
- [x] File-scope `let` — a mutable object with static storage:
      `let requests: u64 = 0;`. See
      [Variables](/language/variables#file-scope)
- [x] A file-scope initializer must be an **initializer constant expression**
      (a literal, arithmetic over constants, or a reference to another file-scope
      constant). **There is no dynamic initialization at file scope**: nothing
      runs before `main`, so the static initialization order fiasco is
      unrepresentable rather than avoided. A value that must be computed is
      initialized in `main`
- [x] Constant initializers are evaluated in **dependency order** (`const a =
      b + 1;` may name a `b` defined further down, since file-scope names are
      order-independent), and a cycle is an error
- [x] A `let` with no initializer is **zero-initialized** (the C ABI's `.bss`);
      a `const` with no initializer is an error
- [x] A file-scope binding has **external** linkage, `let` and `const` alike: it
      is a member of the unit's namespace, and a constant is a value a module
      exports. Visibility is the **module system's** question, not a linkage
      default's — `pub`/`private` filters lookup without rewriting linkage
      (`resolve.md` decision F)
- [x] `static` makes a binding **internal to this unit** — the one word that
      narrows linkage, and the answer to a shared `.mx` file included twice
- [x] `const` protects the **name**, not the memory: a global is never
      `readonly`/`constant` in the IR on the strength of it
- [x] `#define` is not the constant and the constant is not a `#define`: the
      preprocessor is for what must be seen before the grammar (guards,
      conditionals, pasting), a typed constant is for what the type checker must
      see. The design record is `docs/architectures/globals.md`
- [ ] `extern let` / `extern const` — a declaration of storage defined in
      another unit, a library, or the C runtime (`extern let environ: *str;`),
      the same word and meaning as in `extern fn`. **Refused by name today**
      (`parse-extern-binding`): `extern fn` is the declaration form that exists,
      and a file-scope binding is defined in this unit
- [ ] Thread-local storage (a storage-duration question for
      `docs/architectures/memory.md`'s concurrency section)
- [ ] Aggregate initializers for a file-scope constant (`const t: [4]i32 = ...`)
- [ ] C symbols bound by name other than `extern` (an `@symbol` attribute) `[?]`
- [ ] Visibility (`pub` / `private`) and namespaces
- [ ] Module system and imports `[?]`
- [x] Variadic functions, including calling C variadics
- [ ] Default arguments or named arguments `[?]`

## Scopes and names

How a name is tied to the declaration it means. These are *semantic* decisions,
so they are recorded here as well as in `docs/architectures/resolve.md`
(`#decisions-the-language-owns`), which is where the algorithm that depends on
them lives.

- [x] A file-scope name is visible **independently of order** (as in Go's
      package block, not C's point of declaration): a call may name a function
      defined further down, and mutual recursion needs no prototype
- [x] A `let`/`const` initializer sees the **outer** binding, not the one being
      declared (`let x = x + 1;` shadows; `let x = x;` is the outer `x`, never
      the uninitialized new one)
- [x] Scopes are lexical and block-based; a block is a scope, and a function's
      parameters form one with its body's outermost block
- [x] Declaration-before-use is **not** required within a scope's *body*, but a
      `let` is in scope from the statement after it, as in C
- [x] `fn` **cannot** be declared inside a `fn` — the grammar has no nesting
- [x] Shadowing is **allowed**; diagnosed only under `-Wshadow`
- [x] Unused declarations are diagnosed under `-Wunused`
- [ ] `goto` and labels — the `Label` name space is reserved, the feature is
      not
- [ ] Visibility (`pub`/`private`) filters lookup rather than nesting scopes

## Statements and control flow

See [Statements](/language/statements).

- [x] Blocks and `return`
- [x] `if` / `else` / `else if` — the condition takes no parentheses (and accepts
      them), and each arm is a block
- [x] `while`, and C-style `for` (`for init; cond; step`), parentheses optional
- [ ] `do`/`while`
- [ ] Range/`for`-in iteration `[?]`
- [ ] C `switch` and/or pattern `match` `[?]`
- [x] `break` / `continue`
- [ ] Labels on `break` / `continue` `[?]`
- [ ] `goto` and labels (C compatibility)
- [ ] `defer` `[?]`
- [ ] Assertions and checked runtime conditions

## Expressions and operators

See [Expressions](/language/expressions) and the
[operator table](/reference/operators).

- [x] Arithmetic, bitwise, comparison, and logical operators with C precedence
- [x] Short-circuit `&&` / `||` — parsed and typed (`bool` operands, `bool`
      result); the short-circuit *evaluation* is the IR's
- [x] **No undefined behaviour in integer arithmetic**: signed and unsigned
      overflow wrap (two's complement), and the lowering must not emit `nsw`
- [x] **Integer edges are diagnosed where they are constant and defined where
      they are not**: a constant that does not fit its type, a division or
      remainder by zero, `INT_MIN / -1`, and a shift count that is negative or
      at or past the width are all errors at compile time and traps at runtime
      (`INT_MIN % -1` is `0`) — never a value the optimizer may invent
- [x] **Evaluation order is specified**: operands and argument lists evaluate
      strictly left to right, and `&&`/`||`/`?:` evaluate only the side taken
      (C leaves the order unspecified)
- [x] Assignment and compound assignment, with the left side checked to be a
      modifiable place
- [x] Conditional expression `?:`, with both branches unified to one type
- [x] Casts, in three spellings: `x as T`, `(T)x`, and the literal suffix —
      see [Casts](/language/expressions#casts)
- [ ] `sizeof`, `alignof`
- [x] Address-of and dereference (full set in *Pointers and raw memory*)
- [x] Indexing: `a[i]` on an array, `p[i]` on a pointer, `s[i]` on a slice
- [x] Slicing: `a[l..r]` / `a[l..]` / `a[..r]` / `a[..]`
- [ ] Member access (`.` and `->`) — it needs `struct`
- [x] Literals: integers (bases, **suffixes**, digit **separators**), floats
      (decimal and hex, with an optional exponent and an optional integer part),
      chars, strings — read by one shared reader, so the type checker and the
      preprocessor cannot disagree about what `0x10`, `0755`, `1_000` or `10u8`
      means — see [Literals](/language/expressions#literals)
- [x] Escape sequences in character and string literals: `\n` `\r` `\t` `\v`
      `\f` `\b` `\a` `\e` `\?` `\"` `\'` `\\`, octal (`\101`, `\o{101}`),
      hex (`\x41`, `\x{41}`), `\uXXXX` / `\UXXXXXXXX` / `\u{...}`, and a
      backslash before the end of a line (the literal continues). An unknown
      escape is refused by name (`lex-unknown-escape`) rather than passed
      through as the character, `\N{...}` is refused because the name table is a
      dependency this compiler does not carry, and an escape above one byte is
      refused by the string reader or — in a `char` — by the checker, which
      names the fix
- [ ] Raw and multiline strings `[?]`, and adjacent literal concatenation
      (`"a" "b"`)
- [ ] String interpolation/formatting `[?]`

## Builtins

See [Builtins](/language/builtins), and `mincc builtins` for the compiler's own
list. A builtin is an operation the language cannot express as a function; a
library symbol is a declaration, not a builtin.

- [x] **Two spellings, two owners**: the bit operations are ordinary names bound
      in the file scope (`clz`, `ctz`, `popcount`, `bswap`, `rotl`, `rotr`) — a
      local shadows one, a file-scope declaration of it is a redeclaration — while
      anything beginning with `__builtin_` cannot be declared or `#define`d at all
      (`resolve-reserved-identifier`, `pp-reserved-identifier`)
- [x] **Defined answers where the instruction is undefined**: `clz(0)`/`ctz(0)` are
      the width, a rotate's count is taken modulo the width, and the test suite
      proves both by running them
- [x] **A family is one row**: the width comes from the argument, with no
      conversion, so one row serves `i8` through `i128` and `isize`/`usize` at the
      target's own width
- [x] `__builtin_trap()`, typed `!` — the primitive a runtime's `assert` is built
      on, and what makes a body ending in it keep its return type
- [x] **A width the operation does not exist for is refused at the call site**:
      `bswap` of a `u8`, which LLVM's verifier would otherwise reject as a module
- [x] The table is the *only* place a builtin is known by spelling: the checker, the
      lowering, `mincc builtins` and the reference page all read the same rows, and
      a source scan keeps a second copy from appearing
- [ ] `sizeof`, `alignof`, `static_assert` — front-end operators: they take a type,
      so they are grammar rather than rows
- [ ] The checked-arithmetic family (`__builtin_{add,sub,mul}_overflow`)
- [ ] `offsetof`, which needs `struct`

## Memory and lifetime

- [x] Manual allocation interoperating with C: `extern fn *void malloc(n:
      usize);` and a cast to the object pointer is the whole of it
- [ ] Owning `alloc` / `free` in the language itself
- [ ] Allocators/arenas exposed to the language `[?]`
- [ ] Deterministic cleanup (`defer` or destructors) `[?]`
- [ ] Move semantics `[?]`
- [ ] Ownership/borrow checking `[?]`

## Error handling

- [ ] C-style error codes and `errno`
- [ ] `Option` / `Result` types `[?]`
- [ ] Error propagation (`?` / `try`) `[?]`
- [ ] Panics vs. recoverable errors `[?]`

## C interoperability (language surface)

- [x] C calling convention and ABI — taken from the target's **triple** and
      provided by the LLVM backend rather than written here, so adding a target
      does not add an ABI implementation
- [x] Calling C functions from `.mx`, by declaring them: `extern fn i32
      puts(s: str);` — see `examples/010_extern.mx`
- [x] **Variadic** C functions (`extern fn i32 printf(fmt: str, ...);`), with
      the ABI's default argument promotions applied to the arguments past the
      fixed ones — see `examples/011_variadics.mx`
- [x] `str` and `*void` at the boundary, and `*u8` byte views over any object
- [ ] Exporting `.mx` symbols that C can call
- [ ] Struct layout compatibility, passing and returning aggregates by value
- [ ] `extern let` / `extern const` — reading a C global from `.mx`
- [ ] Function pointers interoperating with C callbacks
- [ ] Opaque C types and forward declarations
- [ ] Importing C headers `[?]`
- [ ] Declaring links to libraries from source `[?]`
- [ ] Bitfields `[?]`

## Standard library surface

- [ ] Core types: string, slice, optional/result
- [ ] I/O (print, files)
- [ ] Collections (list, map)
- [ ] Math and string utilities
- [ ] Formatting

## Safety

- [x] Bounds-checked indexing in the checked build: `a[i]` against the count in
      the type, `s[i]` against the descriptor's `len`, and both against a
      constant index at compile time. The opt-out is the build flag
      (`-fno-check`), not a spelling at the subscript
- [ ] A checked build with the shadow memory: unwritten bytes, dead allocations,
      a pointer's extent
- [x] **Definite assignment**: a `let` with no initializer holds no value until
      an assignment reaches the read on *every* path, and reading it is
      `sema-use-before-assignment` — an error, as in Java, C# and Swift, not C's
      warning-if-you-are-lucky; the accepted side of the rule, including the
      `while true` plus `break` idiom, is
      `examples/008_definite_assignment.mx`
- [ ] Overflow checks in debug builds `[?]`
- [ ] Null safety `[?]`
- [ ] Type safety at C-interop boundaries `[?]`

## Tooling exposed in the language

- [ ] In-language tests (`test` blocks) `[?]`
- [ ] Doc comments feeding generated documentation
- [ ] Deprecation and stability attributes
