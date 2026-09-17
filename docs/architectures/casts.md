# Casts — three spellings, one meaning, and the one conversion that keeps a precondition

A **cast** is the source saying *convert this value, and I know what that costs*.
The language already converts implicitly — the promotion and assignment rules of
`convert.h` — and it deliberately refuses exactly one pair in either direction: an
integer and a float do not convert into each other. A cast is where that refusal
is discharged, and it is also where every other conversion the language will not
do silently lives: a pointer to an integer, a `str` to a `*u8`, an `i64` to an
`i8`.

Three spellings, and they are not three features:

| form | spelling | binds | what it is |
| --- | --- | --- | --- |
| **modern** | `x as T` | postfix, below unary | an operator, so it is unambiguous by construction |
| **C** | `(T)x` | prefix, above postfix | the same conversion, and unambiguous because type names are *reserved* |
| **literal** | `10u8`, `12f`, `0xFFu32`, `1.5L` | the literal itself | the type of a literal, decided where it is written |

`x as T` and `(T)x` produce **the same node** (`ast::NodeKind::CastExpr`), the
same record, and the same instruction. They differ in where the type is written
and in nothing else, which is why the rest of this record says "a cast" and not
"an `as`-cast". The third form is not a cast of a value at all — it is the *type*
of a literal — and it is included here because it is the only way to write "this
number is an `u8`" without a cast, and because a language whose literal has a
deferred type needs a way to break the deferral.

## The decision, in one paragraph

**A cast is a `Coercion` the source wrote down.** The checker records
`(from, to)` at the cast node exactly as it records an implicit conversion at the
operand that needed one, the lowering materialises it through the machinery that
already exists (`Lowering::convert`, whose `SIToFP`/`FPToSI`/`ptrtoint` arms are
written and reachable today from no program), and the *constant* folder folds it
through the same pair → instruction table so a global initializer and a run-time
cast cannot disagree. Nothing in the IR stage learns a new concept; what it gains
is a set of pairs it could not be asked for before. The one conversion that needs
new machinery is float → integer, because LLVM's `fptosi` on an out-of-range
operand is *poison* and this language has no poison: that one is guarded and
traps, exactly as division by zero already is.

## What the market did, and what each answer cost

**C — one cast, seven meanings.** `(T)x` is a value conversion, a pointer
conversion, a truncation, a sign change, and — when `T` is a pointer and `x` is an
integer — a reinterpretation, all with the same spelling. The ambiguity is
grammatical as well as semantic: `(i32)-1`, in C, is a cast iff `i32` is a
`typedef` *visible at that point*, so the parser needs the symbol table, and the
class of bugs that follows (`(A)(B)` — call or cast?) is old enough to have a
name. C also defines out-of-range float → integer as **undefined behaviour**,
which is the answer this language exists to not give.

**C++ — the same syntax plus named casts**, and the names are the honest part:
`static_cast` (a conversion), `reinterpret_cast` (the bits), `const_cast` (a
qualifier), `bit_cast` (a bit copy). It is the market admitting that one spelling
for three meanings was a mistake, and it is why this record keeps the *value*
conversion and leaves the *bits* one for a name of its own.

**Rust — `as` for everything, and the price is silence.** `as` truncates, wraps,
changes sign, and since 1.45 **saturates** a float → integer cast at the bounds
(rust#10184: the old rule was UB). It is defined, it is fast, and it is quiet:
`1e30 as i32` is `i32::MAX` with nothing said. Rust's answer to "I want to know"
is a *different* API (`TryFrom`, `try_into`), which is a good answer and one this
language cannot copy: there is no trait system here.

**Zig — a name per operation, and safety where it can afford it.**
`@intCast` (checked in `Debug`/`ReleaseSafe`), `@truncate` (always truncates),
`@bitCast` (the bits), `@as` (widening only). The cost is that the *reader* has to
know which one they meant, which is the opposite of a single `as`; the benefit is
that the safe one is the default and the unsafe one is spelled.

**Go — `T(x)`, no `as`, no suffixes, and conversions that are mostly explicit
where C's are implicit.** The cost is verbosity (`int(x)` reads like a function
call and is not one) and the benefit is that everything is visible.

**Swift and Odin** put the *checks* in the conversion: `Int(x)` traps on a value
that does not fit, `cast(T)` vs `transmute(T)` in Odin is the named split again.

**Literal suffixes are a separate lineage, and C's is the complete one.**
`10u`, `10L`, `10ULL`, `1.5f`, `1.5L` — with two suffixes whose *type is
target-dependent* (`long`, `long double`), which is exactly the property minc+'s
target table already models. C23 added `wb`/`uwb` for `_BitInt` (N2775), MSVC
keeps an `i64` suffix from its own history, Rust writes the *whole type name* as a
suffix (`10u8`, `1.0f32`), and Zig has none at all — which is why Zig code is
full of `@as`.

**What minc+ takes:** C's three spellings (familiarity is a feature), Rust's
*whole-type-name* suffixes beside C's short ones (the user of a language with
`i8..i128` should be able to write `10i64`), Zig's insistence that the bit-level
operation has a name of its own — and **none** of the silence: an explicit cast
does not need to warn, but it does have to be *countable*, so the losses are named
by an opt-in lint and float → integer is a precondition with a guard.

## The three forms

### `x as T` — the operator

Grammar: a postfix operator at the unary level, below `parsePostfix`'s calls and
indexing and above every binary operator — Rust's precedence, and C's for a cast.

```
a as i64 * 2      is  (a as i64) * 2
-a as i64         is  (-a) as i64       // unary binds tighter
a as i32 as i64   is  (a as i32) as i64 // left-associative, chains allowed
f(a as i64, b)    is  f((a as i64), b)
```

The type after `as` is a **complete type**, read by the reader every other type
position uses — a binding's annotation, a parameter, a return type, an alias's
target. `x as [4]i32`, `p as *u8`, `s as []u8`, `q as (i32, bool)`, `v as Vec<u8>`
and `p as Pair<T, K>` all read, and each is refused or accepted by its own rule one
stage down. Nothing here is special to casts, and that is the point: an earlier
draft gave the cast a reader of its own — constructors, then words, and stop —
which made `as [4]i32` a type and `as (i32, i32)` not one. That difference was an
artifact of which function ran, not a decision about the language.

Two tokens delimit that read, and each is a decision of its own:

- **the extent.** The type is the constructors, then one word, then its argument
  list — and there it stops, because the expression around the cast begins there:
  `a as i32 * 2` is a multiplication and not a pointer whose pointee never came,
  and `a as i32 < 3` is a comparison (decision 18);
- **the `<`.** In this one position a `<` after a word may be a list or the
  comparison operator it also is, and the tokens around it settle which
  (decision 19) — with no symbol table, and with the same answer after `import`
  lands. Where a list and a comparison would be illegible together the program is
  refused by name: a chain (decision 20).

`as` is a keyword, which is a new one in a list that is deliberately tiny; it earns
its place the same way `let` does (it changes what the following tokens mean).

### `(T)x` — the C spelling, and why it is unambiguous here

The parser decides between a cast and a parenthesized expression with two
questions, both lexical:

1. is the run inside the parentheses a **complete type** (`atCastStart` reads it:
   `i32`, `*i32`, `[]u8`, `[4]i32`, `!`, nested combinations — **reserved names
   only**, which is what keeps `(x) + 1` a parenthesised expression), **and**
2. does the token after `)` **start an expression** (`isExpressionStart`, which
   already exists for statements).

Both yes → a cast whose operand is the unary expression that follows. Otherwise →
a parenthesized expression, as today.

Question 1 is only decidable because **the type names are reserved**. `i32` is not
a keyword in the lexer — it is an `Identifier`, and the type reader recognizes it
by spelling, exactly as it does today — so `(i32) + 1` could in principle be a
parenthesized reference to a variable called `i32`. The rule that removes it is
the one the project already uses for `__builtin_*`: **a name in the type table may
not be declared**. `let i32 = 5;` is refused at its declaration
(`resolve-reserved-identifier`, the existing machinery), and the cast grammar then
needs no symbol table — the failure mode C pays for with the typedef table and the
"most vexing parse" cannot be written here.

That reservation is worth its own sentence, because it is the reason this form
exists at all: minc+ takes C's *syntax* and refuses C's *reason for needing a
symbol table in the parser*.

The refusal is also a better diagnostic than what happens today: with the names
reserved, `i32` alone in a value position is not "unknown name `i32`" but "a type
is not a value; did you mean to cast?".

### `10u8` — the literal suffix

A suffix is part of the **literal token**: `10u8` is one token, `10 u8` is two.
The lexer claims a trailing run of identifier bytes as a suffix **only when the
run is a known suffix**; otherwise the literal ends where it ended before and the
next token is what it was. So:

- `10u8` → one `IntegerLiteral` whose spelling is `10u8`;
- `1.5f32` → one `FloatLiteral`;
- `1else` → `1` then `else` — the existing, tested rule that a number ends at the
  first byte that cannot continue it, kept by making the suffix set a *closed*
  table rather than "any identifier";
- `0xG` → `0x` (flagged `MissingDigits`) then `G` — unchanged.

The table lives in `support/consteval` beside the literal readers, because it is
the same fact for both the scanner (which bytes are the token) and the reader
(what the token means), and `minc_lex` may link `minc_consteval` — a leaf library
with no diagnostics dependency, which is what keeps "lexing never reports an
error, it flags one" true. An unknown suffix adjacent to a literal (no
whitespace) is reported by the **parser** as `parse-invalid-literal-suffix`, which
has the spans to tell `10z` from `10 z` and which can name the set. The alternative
— swallowing any identifier run into the number — is what the lexer deliberately
does not do, and the existing `NumberStopsBeforeAnIdentifier` test is the record of
that decision.

**Integer suffixes** — the value's type:

| suffix | type | note |
| --- | --- | --- |
| `i8` `i16` `i32` `i64` `i128` `isize` | that type | the language's own names (and MSVC's historical `i64`) |
| `u8` `u16` `u32` `u64` `u128` `usize` | that type | |
| `u`, `U` | `unsigned int` — `u32` on every target this compiler names | C |
| `l`, `L` | `long` — **the target's** (64 on the Unices and Darwin, 32 on Windows) | C |
| `ll`, `LL`, `lL`, `Ll` | `long long` — `i64` | C |
| `ul`, `lu`, `UL`, `uLL`, `LLU`, `lLu`, … | the unsigned counterpart of the above | C's combinations, in any order and case |
| `wb`, `uwb` | **refused by name** | C23's `_BitInt` suffix, and minc+ has no bit-precise type: the sentence says so and names `i64` |

**Float suffixes** — the value's type:

| suffix | type | note |
| --- | --- | --- |
| *(none)* | `f64` | the language's default for a float literal (`TypeStore::defaultOf`) |
| `f`, `F` | `f32` | C |
| `f32`, `f64`, `f80`, `f128` | that type | `f80` where the ABI has no x87 row is refused by the type, with the type's own sentence |
| `l`, `L` | `long double` — **the target's**: x87 `f80` on System V and `x86_64-apple-darwin`, `f64` under MSVC and on `aarch64-apple-darwin`, IEEE `f128` on aarch64/riscv64 Linux | C |

Three rules make the two tables one grammar:

- **A float suffix on an integer-spelled literal makes it a float** whose value is
  exact: `12f` is `12.0` as `f32`, `12f32` is the same, and neither rounds. This is
  Rust's behaviour (`1f32`) rather than C's — C refuses `12f`, and a language whose
  floats are `f32`/`f64`/`f80` should not make the user type a `.0` to say which
  one.
- **An integer suffix on a float-spelled literal is refused** (`1.5u`,
  `1.5usize`): there is no such number, and the sentence says to write the
  conversion (`(u32)1.5` / `1.5 as u32`) if that is what was meant.
- **A suffix makes the literal immediately typed, and an unsuffixed literal stays
  deferred.** This is the whole point of the third form: `let x = 10u8;` is a
  `u8`, not an `i32` that happens to fit, and `let y = 10;` is still the deferred
  literal whose context decides. A suffixed literal that does not fit its own type
  is `sema-literal-out-of-range` (`300u8`), naming the type and the value — the
  same code `let x: u8 = 300;` already produces.

Suffixes on character and string literals are refused: there is **one** character
type and **one** string type, and `'a'u8` says nothing that `'a' as u8` does not.

## Decisions

| # | Decision | Why |
| --- | --- | --- |
| 1 | **A cast is a `Coercion` the source wrote** — one record, one materialiser | The rule the coercion record already exists for: the lowering materialises decisions, and a second place that decides how a pair converts is a second answer |
| 2 | **One node, two spellings** (`as T`, `(T)x`) | They are the same operation; two nodes would be two code paths in five stages for no expressive gain. The *parse tree* keeps the tokens, so a formatter still round-trips |
| 3 | **`as` binds postfix, above every binary operator** | Rust's rule, and C's for a cast: `a as i64 * 2` is `(a as i64) * 2`, unary binds tighter, chains are left-associative |
| 4 | **Type names are reserved identifiers, and that is what makes `(T)x` unambiguous** | The alternative is C's typedef table in the parser — the "most vexing parse". A reserved name costs a user one spelling and buys a grammar with no symbol table |
| 5 | **The C form is delimited by the *reserved* set; a user-defined type name will be `as`-only** | The parser cannot know what a declaration introduced (it runs before resolution), and it must not be taught to guess. Named in § *How this stays correct* so the day `struct` lands it is not a surprise |
| 6 | **The cast matrix is one function over `(from, to)`**, shared by the checker, the lowering and the constant folder | `conversionFor` already made this argument for implicit conversions; a cast that folds one way in a global initializer and another at run time is the bug the shared table removes |
| 7 | **Every arithmetic pair is castable**: int ↔ int, float ↔ float, int ↔ float | "An integer and a float do not convert" is a rule about *implicit* conversion; the cast is exactly where it is discharged |
| 8 | **Float → integer is guarded and traps; a constant is refused at compile time** | `fptosi` out of range is LLVM poison, and this language has no poison. It is the same shape as division by zero (defined: a trap, guard always emitted, scanned) — and for a constant the compiler proves the violation, so it is a diagnostic instead of a trap |
| 9 | **Not saturated.** Rust saturates because it has no checked build to fall back on; here a silent `i32::MAX` would be a wrong answer that no flag can find | `1e30 as i32` = `INT_MAX` is a *value*, and a value that is not the mathematical result is a lie the language otherwise refuses to tell |
| 10 | **Adjacent: `11` and `12c` do not exist; `bool` ↔ integer is explicit; float → `bool` is refused** | `int → bool` is defined (≠ 0) and useful; `float → bool` has NaN, which is neither true nor false, and the sentence sends the reader to `x != 0.0` |
| 11 | **Pointer ↔ pointer emits nothing**; pointer ↔ integer *is* `expose`/`with_exposed_provenance` and is counted by `-Wprovenance` | `memory.md` decision 7 says the two joins are named operations and never implicit; a cast is the name. Opaque pointers make the pointer-to-pointer case a *type* change with no instruction at all |
| 11a | **The two joins have different losses, and they are read from the target**: `ptrtoint` truncates when the integer is narrower; `inttoptr` truncates when it is *wider*, and reinterpret a **signed** narrower integer's sign because it zero-extends | The first implementation mirrored one rule into the other, so `u8 → *u8` claimed "bits are dropped" and `u128 → *u8` said nothing. `ztests` is where it surfaced. The widths come from `target().pointerBits`, so `i32 → *u8` is a sign loss on x86_64 and nothing at all on i386 — a rule that cannot be written without the target |
| 11b | **An `int → ptr` conversion from a *constant* is refused**, zero included (`sema-address-from-constant`) | Naming an address is an assertion about *where the value came from*: an address is obtained — from an object (`&x`), from an exposed pointer, or from the system — and a constant is a number the program never obtained. It is decision 8's shape on the other join: the compiler can *see* the value, so it refuses instead of emitting a program whose first access crashes with no sentence anywhere. `let x = (str)1; printf(x);` passed `check`, built, and died of SIGSEGV — a failure with no diagnostic is the one outcome this project does not allow. Zero is not an exception because the null address has a spelling of its own (`null`), and `0 as *T` is `inttoptr` of a zero: permission over *every* exposed allocation, which is not what a reader writing `null` means. A **value** is never refused, however it was arrived at: `memory.md`'s answer to "where did this come from" is to *count* the operation (`-Wprovenance`), not to guess. What the refusal does not break: MMIO and fixed addresses stay writable through a value the program obtained (`mmap`, a table, a parameter), which is how a portable program gets one anyway |
| 12 | **No reinterpretation in a cast.** `x as i32` on an `f32` is a *value* conversion; the bits are a name of their own, reserved for that item (`bitcast`/`transmute`) | This is C++'s `reinterpret_cast`/`bit_cast` split and Zig's `@bitCast`: the one operation where "convert" and "reinterpret" cannot share a spelling |
| 13 | **Aggregates are refused, each with the sentence that says what to write** | `[N]T` → `*T` is no decay (arrays.md decision 4) and the workaround is `&a[0]`; a slice → pointer needs the extent, and there is no literal slice (`slices.md`, decision 4) |
| 14 | **A cast that the language would have done implicitly emits nothing** | A cast is a *statement about the program*, not an instruction: `let x: i64 = a as i64;` is one conversion in the record, and the flag-and-materialiser produce the same code it always did |
| 15 | **Constant casts fold through the same table** | A global initializer must be a constant; the folder and the run-time path sharing one answer is the only way `const A = (u8)300;` and `f((u8)300)` agree |
| 16 | **Explicit casts are silent by default; `-Wcast` (new, opt-in) names the loss** | `-Wconversion` has one rule — an *implicit* conversion that loses — and folding casts in would make it fire on the mask idiom (`(u8)x`) that a cast is written for. `-Wcast` names *which* loss: range, sign, precision, truncation |
| 17 | **Literal suffixes are lexical and one table**, shared by the scanner and the reader | C's rule: `10u8` is one token. Two tables would drift the day a width is added |
| 18 | **The type after `as` is read by the one type reader, inside a bound the scan finds** | One grammar for a type in every position: the cast cannot accept a type the rest of the language refuses, and cannot refuse one it accepts. The scan answers the two questions a reader surrounded by an expression cannot — how far the type reaches, and whether the `<` is a list — and both are questions about tokens, which is what a scan is for |
| 19 | **`<` is a list only when the tokens say so.** Four facts, in order of cost: a list hangs off a *word*; a **reserved** type word takes no arguments, so a `<` after one is a comparison; the contents must be type-shaped (a number is a type only inside `[N]`); and what follows the closer must be able to follow a complete cast — an expression after a *stray* `>` is a shift, a word or a literal after an exact one is an ordering comparison | `W < ... > ...` is two programs written the same way, and the decision has to be lexical. A symbol table would make it *order-sensitive* (C's typedef problem) and wrong the day `import` lands, while this reads the shape of what is written. The corner the facts leave — an unterminated list whose first argument is a reserved word — is read as the list it can only have been, and the other reading of that shape is illegal for a reason of its own (decision 20) |
| 20 | **A comparison does not chain, and the sentence names the chain** | `a < b > c` is `(a < b) > c`, which the type rules refuse anyway — with a sentence about an operand that never names the shape the reader wrote. It is in this record because it is what makes decision 19 **total**: of the two readings of `x as W < ... > ...`, the comparison reading is a chain whenever the list reading is not the answer, so no legal program is ever read the other way. `clang` makes a chain a hard error too (`-Wparentheses`); minc+ makes it a sentence with the fix in it. `(a < b) > c` is not a chain: the parenthesis is the reader saying they meant it |

## The conversion matrix

The whole surface, from the types the language has today. "Emits" is the LLVM
instruction; **guarded** means the instruction is reached through a test that
traps; *nothing* means the conversion is a type change in the record with no
instruction.

| from → to | emits | defined/checked | notes |
| --- | --- | --- | --- |
| `iN`/`uN` → wider integer | `sext`/`zext` (by the **source**'s signedness) | defined | value-preserving; no loss, no lint |
| `iN`/`uN` → narrower integer | `trunc` | defined (wraps) | `-Wcast: truncation` |
| `iN` ↔ `uN` (same width) | *nothing* | defined | the bits are the value; `-Wcast: sign` |
| `bool` → integer | `zext i1` | defined | `true` is 1, `false` is 0 |
| integer (and `char`) → `bool` | `icmp ne 0` | defined | `char` is an integer type, so `'a' as bool` is `true` |
| `char` ↔ `u8` | *nothing* | defined | `char` **is** `u8` in representation (`README`, *Types*); the two names differ in intent, not in bits |
| `f32` ↔ `f64` ↔ `f80` ↔ `f128` | `fpext`/`fptrunc` | defined | IEEE rounding, as the hardware does it; `-Wcast: precision` |
| integer → float | `sitofp`/`uitofp` | defined (rounds to nearest) | `-Wcast: precision` when the source holds values the destination's mantissa cannot (`i32`→`f32`, `i64`→`f32`/`f64`, `i128`→anything) |
| `bool` → float | `uitofp i1` | defined | `0.0`/`1.0`, and no reason to refuse what `bool → integer` allows |
| float → integer | `fptosi`/`fptoui` | **guarded** | truncation toward zero for an in-range value; NaN and out of range **trap** (§ below) |
| float → `bool` | — | **refused** | NaN is neither true nor false: `x != 0.0` |
| `*T` → `*U`, `*T` ↔ `*void` | *nothing* | defined | opaque pointers: a pointer cast is a type change. `*void` already converts implicitly |
| `str` ↔ `*u8` | *nothing* | defined | the *sentinel* is a documented obligation, not a type: `str → *u8` drops the guarantee that the bytes are terminated, `*u8 → str` asserts it |
| `*T`/`str`/`*void` → integer | `ptrtoint` | defined | `memory.md`'s `expose`: the value is the address, and provenance is recorded as *exposed*. Counted by `-Wprovenance`; `-Wcast: truncation` when the integer is narrower than the pointer |
| integer → `*T`/`*void`/`str` | `inttoptr` | defined | `memory.md`'s `with_exposed_provenance`: permission over every allocation whose provenance has been exposed, and no other. Counted by `-Wprovenance`. **Not the mirror of the row above**: `-Wcast: truncation` when the integer is *wider* than the pointer (the low bits are kept), and `-Wcast: sign` when it is a **signed** integer *narrower* than it — `inttoptr` zero-extends, so `-1 as *u8` is `0x0000_0000_FFFF_FFFF` on a 64-bit target, measured and not assumed. **A constant operand never reaches this row**: it is refused (11b), because the compiler can see that the program never obtained the address |
| `!` → anything | *nothing* | defined | the operand never produces a value, so the conversion is vacuous — the rule `never.md` already ships |
| `[N]T`, `[]T`, a function, `void` → anything | — | **refused** | § below |

## Float → integer: the one conversion with a precondition

The value is in the destination's range, or the program does not run. That is the
same shape as division by a non-zero divisor — *defined* when the precondition
holds, and a **trap** when it does not (`sema.md`, which already chose the trap
over LLVM's poison) — and it is chosen over both alternatives for the reasons the
market paid for:

- **not undefined behaviour** (C): `fptosi` out of range is poison in LLVM, and a
  language whose selling point is "no undefined behavior that the programmer cannot
  see" cannot emit one;
- **not saturation** (Rust, and .NET since 9.0): `i32::MAX` from `1e30` is a
  *wrong value* that propagates, and no flag will find it. Saturation is also
  measurably slower on the targets that have a one-instruction conversion, which is
  the other half of why Rust chose it — a reason that applies to a language with no
  checked-build story and not to this one.

Mechanically:

- **Constant operand.** The compiler folds the conversion; if the value is not
  representable (out of range, or NaN) it is a **diagnostic**
  (`ir-cast-out-of-range`), naming the value and the range. The code is the
  *lowering's*, not the checker's, because that is where the fold happens: the
  checker has no value for `1e30 as i32` and does not guess, so `mincc check`
  passes a program that `mincc ir` and `mincc build` refuse. One stage decides,
  and the stage that knows the value is the one that speaks. A constant that *is*
  representable folds to the constant the run-time path would have produced.
- **Run-time operand.** `lowerOperand` emits a range test (`fcmp` against the
  destination's bounds, NaN included) and the trap path, exactly as the division
  guard does. The `fptosi`/`fptoui` is only reached when the test passed, so no
  poison is ever created.
- **The scan grows a row.** `invariants.cc`'s assumption list requires that every
  `fptosi`/`fptoui` is reached through a test of its operand, in the shape of the
  existing unguarded-division row, and `invariants_test.cc` trips it on a module
  this compiler built and then broke by hand.
- **The checked build names the site** (`cast-out-of-range`), which is the
  vocabulary the violation table already uses; the release build traps, which is
  what "defined as a trap" means.

## Pointer and integer: `expose` and `with_exposed_provenance` get a spelling

`memory.md` states both directions as *named* operations and refuses them as
implicit conversions; this record is where the names attach to the surface:

```
let addr: usize = p as usize;        // expose(p)
let p2: *u8 = addr as *u8;           // with_exposed_provenance(addr)
```

Two properties are kept because they are the reason the model names them at all:
the operations are **never implicit** (the assignment conversion still refuses
`let a: usize = p;`), and they are **counted** — `-Wprovenance` names every site,
which is the mechanism `memory.md` chose instead of an `unsafe` keyword. The
diagnostic's text uses the model's own words (*expose*, *with exposed
provenance*), so a reader who finds the operation here can read the rule there.

**The literal-zero case is now decided, and the space is filled.** `null` exists
and is a `*void`, so a cast from a literal zero (`0 as *u8`) is `inttoptr` and
means "provenance over everything exposed" — the opposite of what a reader writing
`null` means. So it is refused, together with every other constant, by decision
11b below, and the sentence names `null` (`null as str` where the target is a
`str`, which is not a `*void`).

## What is refused, and the sentence that replaces it

A refusal that does not say what to write instead is a refusal that gets worked
around. Each of these is a named code with a sentence:

| written | said |
| --- | --- |
| `arr as *i32` | an array is not a pointer, and it does not decay: write `&arr[0]` for the address of the first element and carry the length beside it |
| `s as *i32` (a slice) | a view is a pointer *and a length*: `&s[0]` is the address of its first element, and the extent is `s`'s own business |
| `p as []i32`, `n as []u8` | there is no slice of a pointer without a length, and no literal slice: take the view from an object that has one |
| `[4]i32 as [4]u8` | an array converts element by element or not at all; the byte-level copy is an operation this language spells (and does not yet have). The sentence the reader actually gets is the array/no-decay one above, because the pointer test is asked first — one refusal, and it already names the fix |
| `f as bool` | NaN is neither true nor false: write `f != 0.0` |
| `x as void`, `x as fn(...)` | `void` is the absence of a value and a function type is not an object |
| `1.5u` | a float literal cannot have an integer suffix: `(u32)1.5`, or `1.5 as u32` |
| `10wb` | C23's bit-precise suffix has no type here: use `i64` (or `i128`) |
| `1 as *u8`, `0 as *i32`, `(str)1` | an address cannot come from a constant: nothing in the program obtained it. Write `&x` for an object, cast an **exposed** value back for an address that came from somewhere, or `null` for the null address (`null as str` when the target is a `str`, which is not a `*void`) |

`(i32)` with no operand is not a cast at all — it is a parenthesized type name,
and the diagnostic is "a type is not a value; did you mean to cast?" rather than
"expected an expression".

**`"abc" as u8` is not on that list, and the matrix above is why.** `str` is a
pointer, and the `*T`/`str`/`*void` → integer row is `expose`: it is a defined
cast whose result is the address, truncated when the integer is narrower, which
`-Wcast: truncation` names. An earlier draft refused it and told the reader to
write `("abc" as *u8) as u8` — two casts where the matrix already defines one,
and a refusal standing against the row it was meant to serve. The measured
behaviour is the matrix row: `ptrtoint (ptr @str to i8)` with the truncation
reported under `-Wcast`.

## Where it lands in the compiler

| where | what it gains | why there |
| --- | --- | --- |
| `include/support/consteval/literal.h` + `literal.cc` | the **suffix table** (spelling → `{kind, width, signedness, target-dependent}`), read from the literal's spelling; the existing `u`/`l` acceptance becomes the real taxonomy, and `wb`/`uwb` join as *refused* | It is already the one reader of a literal's spelling, shared by `sema` and the preprocessor; the table is the same fact for the scanner |
| `src/lex/literal_scanner.cc` | a trailing **known** suffix is claimed into the token (`IntegerLiteral`/`FloatLiteral`); an unknown run still ends the number | C's rule, and the existing `NumberStopsBeforeAnIdentifier` invariant is what limits it |
| `include/lex/token_kind.h` | `KwAs` | A keyword that changes what the following tokens mean, which is the test the lexer's own comment states for one |
| `src/parse/expression.cc` | `parseCast` at the unary level (`as` postfix, `(T)` prefix), one `SyntaxKind::CastExpr`; the `as` type is read through `scanTypeRun` under a `TokenBound` | One node, and the type child is the `Type` node a binding already has — read by the reader every other position uses, bounded by the scan |
| `src/parse/type_scan.h` | the scan of a type run, moved out of the declaration reader and given the cast's two questions (`RunKind::Cast`, `typeArgListIsReal`) | Two readers ask what a run's shape is, and a spelling that closes a list is one fact about the language; a second copy of it is a second answer waiting to drift |
| `src/parse/declaration.cc` | nothing to add: `scanTypeRun` is the run the declaration reader already used | It was written for declarations and answers the same question there — with `<` unconditional, because a declaration's run has no expression in it |
| `include/parse/parse_error.h` | `InvalidLiteralSuffix`, `ExpectedCastOperand` | The parser owns adjacency (it has the spans) and the operand |
| `include/ast/node.h`, `src/ast/lower.cc` | `NodeKind::CastExpr` (a `Type` child and one expression) | The AST is where the two spellings become one thing |
| `include/sema/convert.h`, `src/sema/convert.cc` | `enum class CastKind`, `castable(types, from, to)`, `castKindFor(types, from, to)`, `castLoss(...)` | The matrix, as pure functions over types, beside the implicit rules it extends |
| `src/sema/check_expr.cc` | `checkCast`: the matrix, the constant rule, the refusal sentences, and the record | The checker decides and records; it does not emit |
| `include/lex/token_kind.h`, `src/resolve/*` | the reserved set (`i8`…`usize`, `bool`, `char`, `str`, `void`, the C spellings) shares the `__builtin_*` machinery | One predicate, two callers — the shape the builtin reserved names already use |
| `include/sema/sema_error.h` | `CastInvalid`, `CastLoses`, `ProvenanceCast` | Named codes, in the enumeration the tests sweep. `CastOutOfRange` is **not** here: it is `include/ir/ir.h`'s, because the fold that proves a constant unrepresentable happens in the lowering |
| `src/ir/types.cc`, `src/ir/expr.cc` | the arms the record newly reaches (`sitofp`, `uitofp`, `fptosi`, `ptrtoint`, `inttoptr`, `icmp ne 0`), the **float → integer guard**, and `bool → float` as `uitofp i1`; a `!` operand converts to a **poison of the consumer's type** with no instruction, and nothing else | It materialises the record; the guard is new because the model forbids poison, and the `!` arm is `never.md`'s rule reaching a value position — one lookup of the record, so no path can be forgotten |
| `src/ir/invariants.cc` | the row: no `fptosi`/`fptoui` without a range test | The assumption list is closed and scanned, and this is a new assumption-shaped emission |
| `src/driver` | `-Wcast` (opt-in) and the `-Wprovenance` text | Both are flags over decisions the checker made |
| `docs/architecture.md`, `docs/roadmap.md`, the site | the item, and the surface page | The record is linked from the two places a reader starts |

## The implementation, in the order it lands

1. **The suffix table and the reader** (`support/consteval`), with its own tests:
   every spelling in the table reads to the type named, `wb` is refused by name,
   `10u8` fits, `300u8` does not. No parser change yet, so this lands alone.
2. **The lexer claims known suffixes**; the existing "number ends at the first
   byte that cannot continue it" tests stay green, and a new one asserts `10u8` is
   one token while `10z` is two.
3. **The parser**: `KwAs` → `as` postfix, with `scanTypeRun` + `TokenBound` for the
   type; `(T)x` prefix behind `atCastStart` + `isExpressionStart`;
   `parse-invalid-literal-suffix`; the `CastExpr` node and its lowering to the AST.
4. **The reserved type names** in `resolve`, which is what makes step 3
   unambiguous — and the point at which `(i32)` stops being an unknown name and
   starts being a clear refusal.
5. **The matrix in `sema/convert`**, with a table-driven test: every pair, one
   assertion each, in the shape of the coercion-record enumeration test.
6. **`checkCast`** and the record: legality, the constant rule, the refusal
   sentences, and the suffixed literal's type.
7. **`ir`**: the newly reachable arms, the float → integer guard, and its scan row.
8. **`-Wcast` and `-Wprovenance`**, one test per loss kind and per counted site.
9. **Examples** (`examples/017_casts.mx`), the site page, the roadmap item, and
   `architecture.md`'s module list.

Each step is a commit that passes the gates on its own; steps 1–4 are the surface,
5–7 are the semantics, and nothing in the IR stage grows a concept.

## Tests

- **The universe, in one place.** `tests/unit/casts/universe.h` holds every type
  the language has today (the `iN`/`uN`/`fN` families, `bool`, `char`, `str`,
  `void`, `!`, four pointers, an array, a slice, and the two deferred literals),
  the name→`TypeId` lookup, and the "which instruction does this pair need" table.
  Two suites read it, so they cannot drift into testing different alphabets. The C
  spellings are deliberately absent: they resolve to the *same* `TypeId`s, and a row
  per spelling would test nothing a second time.
- **The whole product, every pair.** `sema/cast_test.cc` walks *every* ordered
  pair of that universe — 676 of them — and asserts the properties a matrix of this
  shape has to have whatever its rows say: `ok` and `CastKind` agree; an accepted
  pair carries no message and a refusal carries one; **a cast never refuses a pair
  of decided types that the language converts by itself** (`convertible ⇒
  castable`, the direction that would otherwise let a conversion exist that no
  reader can spell — a deferred literal is the state of a literal and not a type a
  value has, so it is outside the question and `checkCast` decides it before
  asking the matrix); a cast to the
  same type is the identity and loses nothing; and the loss reported is the loss the
  *kind* has and no other, so `-Wcast` cannot lie about what happened to a value.
  The count of accepted pairs is pinned, so a pair that stops being answered is a
  line in a diff rather than a suite that quietly covers less.
- **The rows, pinned.** The hand-written table beside it names the pair, the kind
  and the loss for the cases a reader would ask about one by one — including the
  four refusals a reader *will* attempt (an aggregate, `void`, a `bool` from a float,
  a deferred literal).
- **Every accepted pair, in the module.** `ir/cast_test.cc` writes every pair the
  matrix accepts into one program, each as a function taking the source as a
  **parameter** — a run-time value, so nothing folds and no pair can pass by having
  been decided as a constant — and then reads the module back: the instruction the
  kind calls for, and no other, for each function. The trap is counted against the
  number of guarded pairs, which is how a site that lost its guard is caught even
  though LLVM would still verify the module.
- **Nesting, the bottom type, and constants, in one build.** `ir/cast_test.cc`
  lowers `((u8)((x as i64) as u32)) as u8` and the int→float→int chain beside it,
  and asserts each step is the instruction its own pair asks for — nesting adds no
  instruction of its own. In the same module: a `!` operand in a value position
  (implicitly and through a cast) becomes a poison of the consumer's type with no
  instruction, and a function whose return type is `!` ends in `ret void` — never a
  `ret` fed the `void`-typed call its body ends in, which is a module the verifier
  rejects.
- **The position, in the parser.** `parse/cast_type_test.cc` walks every type shape
  after `as` (primitive, pointer, array, slice, product, use, use of a use, `!`), and
  then the shapes where the same characters are two programs: a primitive followed by
  `<`, contents that cannot be a list, a stray `>>` with an expression after it, a
  `>>` that closes exactly the lists that were open, and a list followed by a
  comparison. Each case asserts the *tree* — a list is a `TypeArgList` node, a
  comparison is not — and the reconstruction, because an extent that is one token
  too long still parses, it just parses something else.
- **The row sweep.** `invariants_test.cc` gains the unguarded-`fptosi` input, so
  the new row is tripped like the twelve beside it.
- **The guard, both builds.** A program that casts an out-of-range float traps in
  `-O0` **and** at `-O2` (a check the optimizer can delete is not a check), and a
  constant out-of-range cast is a *diagnostic* with no module.
- **The two spellings agree.** For every pair, `(T)x` and `x as T` produce the same
  record and the same instruction — the assertion that keeps "one node" true.
- **Suffixes, as data.** A test iterates the table and asserts each spelling
  round-trips through the scanner and the reader; another asserts the deferred
  rule (`let x = 10u8;` is `u8`, `let y = 10;` is decided by context).
- **Pointer and integer casts are `expose`.** A test asserts the `-Wprovenance`
  count at each site, and that the implicit assignment still refuses both
  directions. Their *losses* are pinned per target: `i32 → *u8` is `sign` on
  x86_64 and nothing on i386, and `u128 → *u8` is `truncation` on both — the pair
  of answers a rule written as one mirror image gets wrong.
- **A constant address is refused, and a value is not.** `cast_test.cc` walks one
  case per spelling of a constant (a literal, zero, a folded expression, a `const`
  name, a negative number) and asserts the same code and the same sentence for
  each, including the null spelling the *target* type takes (`null` for a `*T`,
  `null as str` for a `str`); then it asserts that an object's address, an
  `expose`d pointer cast back, a parameter, and a value from an `extern` call are
  all accepted, because a value is where the address came from.
- **Regression on the ambiguity.** `(x) + 1` for a variable `x`, `(i32)-1`,
  `(u8)(i32)1`, `f((i32)1)` and `(a)(b)` (a call of a parenthesized name) each
  parse to the tree they are supposed to, which is the test C's typedef rule would
  need a parser symbol table to pass.
- **Cross-target.** The suffix table's target-dependent rows (`10L`, `1.5L`) read
  as `long`/`long double` on each named triple, and `(i64)p` truncates the same way
  as `ptrtoint` on i386 and x86_64.

## Hazards, and who paid for them

- **One spelling, several meanings.** C's `(T)x` is why C++ has four named casts.
  Here the meanings are separated *by rule*: a value conversion is a cast, the bits
  are a name of their own, and pointer/integer joins are counted.
- **`as` that saturates silently.** rust#10184 and its fix; a reader of
  `x as i32` in a language that saturates cannot tell "converted" from "clamped".
  This record traps instead, and names the loss under `-Wcast`.
- **Out-of-range float → integer.** Undefined in C, poison in LLVM, and
  implementation-defined in Go. It is the single most common source of "it worked
  at `-O0`" reports in C-family code.
- **The parser that needs a symbol table.** `(A)(B)`: cast, or call? C answers
  with the typedef table and pays with the most vexing parse; the reserved-name
  rule is what makes the question have one answer here.
- **`-Wconversion` on every mask.** `(u8)flags & 7` is the idiom a cast exists for
  and an implicit-conversion lint fires on it; a separate `-Wcast` keeps both rules
  honest.
- **A cast that the language would have done anyway.** The most common `as` in Rust
  code is a no-op; this record makes it one in the IR as well (decision 14), so an
  explicit cast never costs an instruction.
- **A deferred literal that a suffix silently types.** `10u8 + 200` is `i32` after
  promotion, not a `u8` overflow — the promotion is the existing rule and the
  suffix does not make the *expression* narrow, only the literal.

## Cross-platform

- **`l`, `ll` and the float `l` are the target's**, which is C's LP64/LLP64 story
  and the reason `--target` exists: `10L` is `i64` on Linux and `i32` on Windows,
  and `1.5L` is `f80` on System V, `f64` under MSVC, `f128` on aarch64 Linux. The
  reader answers a *descriptor* and the checker (which owns the target table)
  resolves it, so the target is consulted once, in the stage that owns it.
- **`u` is `unsigned int`, not "32 bits"**: the width comes from the target's
  `intBits`, which is 32 on every triple this compiler names and is still read
  rather than assumed.
- **`ptrtoint` is the pointer's width**, and `p as i64` on i386 is a zero
  extension; `p as u8` is a truncation the `-Wcast` lint names. Nothing here
  assumes 64 bits.
- **The same cast has two answers on two targets**, and that is the test
  `castResult`'s `inttoptr` row ships with: `i32 → *u8` is a sign loss on x86_64
  (the zero-extension turns `-1` into `0x0000_0000_FFFF_FFFF`) and *no loss at
  all* on i386, where the two are the same 32 bits. A rule written as "the integer
  is narrower than the pointer" answers the second one wrong.
- **`f80` availability is the type's rule, not the suffix's**: `1.5f80` on a target
  whose table has no x87 row is refused by the same sentence `let x: f80` gets.
- **The guard is `fcmp` + a trap**, both target-independent, and the trap is the
  same runtime call division already emits — so a new target needs no new code
  here.

## How this stays correct when the language grows

1. **The matrix is a switch with no `default:`** over the pair's kinds, so a new
   `TypeKind` is a build error in `castable`/`castKindFor` rather than a silent
   "not castable".
2. **The suffix table is one table**, and the test walks it: a width added to the
   language without a suffix fails the walk, and a suffix without a reader fails
   it the other way.
3. **The reserved set is derived from the type table**, not written twice: the day
   a type name is added, `atCastStart` and the reservation both see it because both
   read the same place — and `typeArgListIsReal` asks that same predicate from the
   other side (a reserved word cannot be an operand either).
4. **A user-defined type name is `as`-only, and this is the decision, not an
   oversight.** When `struct Point` lands, `(Point)p` cannot be parsed without a
   symbol table, and this record refuses to give the parser one: the C form stays
   delimited by the reserved set, and a user type is written `p as Point`. The day
   it is tempting to relax this, the sentence to read is decision 5.
5. **A new loss kind is a new enumerator in `CastLoss`**, and `-Wcast`'s test
   asserts one input per enumerator — the shape every enumeration in this project
   uses.
6. **The guard is a scan row**, so a future lowering that emits `fptosi` directly
   (a `-fno-check` fast path, an intrinsic) fails CI instead of shipping poison.
7. **Bit-level reinterpretation arrives as a name** (`bitcast`), not as a wider
   cast matrix: the reserved word is in decision 12, and the cast matrix never
   grows a column for it.

## Not in scope, with the reason

- **`bitcast`/`transmute`** (f32 ↔ i32 bits, pointer → pointer with a size
  change): its own item, one name per operation, and *not* a cast — the reason is
  decision 12 and C++'s split.
- **Pointer ↔ slice, array ↔ array, any aggregate conversion**: each needs
  something the language does not have yet (a length in the type, a byte copy
  operation, a layout rule for a differently-typed view).
- **C++-style `static_cast<T>(x)` / `reinterpret_cast`**: `as` is the modern
  spelling and a second form of the same thing would be a second thing to learn.
- **`const`/`volatile` qualifiers on a cast** (`(const *u8)x`): qualifiers are
  their own roadmap item; a cast in this record never changes a qualifier.
- **User-defined conversions** (a `struct` with an `as`): they arrive with
  operator overloading, which is not designed.
- **A cast as a file-scope constant expression.** `const a: u8 = (u8)300;` folds
  (the checker folds integer pairs with the same core `#if` uses), and so does a
  cast of an earlier constant (`const b: i32 = a as i32;`). But `const half: f64 =
  (f64)2;` and `const n: i32 = (i32)1.5;` are refused with
  `sema-global-not-constant`: the file-scope walk folds today reads a *literal* or a
  *name*, and a cast involving a float has no value in the 64-bit core the checker
  folds with. The refusal is honest and names the shape ("a float value is read from
  the literal that spells it", so write `2.0`), and the fix is its own item: teach the
  initializer walk to carry a cast the way it carries a literal, and fold it in the
  lowering with `llvm::ConstantFoldCastInstruction` — the machinery the global path
  already uses for a conversion between two integer types.
- **`sizeof`/`alignof` in a cast expression** (`x as sizeof(...)`): a type name is
  what follows `as`, and the grammar family that reads a type in a value position
  is its own item.
- **A null pointer spelling.** § above; it is a *keyword or builtin* question, and
  `0 as *T` is deliberately not made into sugar for it.
- **The `-Wcast` flag's exact name** is decided here; whether it later merges into
  a `-Wconversion` family is a driver question with no language content.

## References

Read for this record, and cited where the wording or the cost is the argument:

- **cppreference, *Integer constant*** (C and C++): the suffix grammar, the `u`/`l`
  combinations and their order-independence, C23's `wb`/`WB` for bit-precise
  integers.
- **N2775**, *Literal suffixes for bit-precise integers* (WG14): why `wb` exists,
  and what a language without `_BitInt` should do with the spelling.
- **rust#10184** and its resolution in 1.45: float → integer casts saturate, and
  the UB they replaced.
- **Rust Reference, *Type cast expressions*** and *literal expressions*: `as`
  precedence, the loss list, and `10u8`/`1.0f32` as suffixes.
- **Zig Language Reference, *Builtin Functions***: `@intCast`, `@truncate`,
  `@bitCast`, `@as` — the named-per-operation position, and where safety checks
  live.
- **C++ — `static_cast`, `reinterpret_cast`, `bit_cast`**: the named split the
  value/bit decision (12) follows.
- **MSVC numeric literals**: the historical `i8`/`i16`/`i32`/`i64` suffixes, which
  is the precedent that a language may spell a width by name.
- **`docs/architectures/memory.md`** decisions 7 and 11: the two named joins, and
  alignment as a property of the access.
- **`docs/architectures/sema.md`**: the defined-trap rule for division, which the
  float → integer guard follows rather than reinvents.
- **`docs/architectures/ir.md`**: the coercion record and the assumption list, the
  two mechanisms this record reuses instead of adding to.
