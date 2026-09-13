# Semantic analysis — `src/sema`

The design record for the stage the pipeline puts between `resolve` and `ir`:
`architecture.md#the-pipeline` says it receives a **resolved AST** and returns a
**typed AST**. This document fixes what "typed AST" means, how a type is
represented, which conversions are implicit, which errors this stage owns (and
which it does not), and what is deliberately left out until the syntax that
needs it exists.

Research behind it: the rustc dev guide (AST → HIR lowering, `Ty<'tcx>`
interning, and THIR as the *fully typed* HIR between HIR and MIR), the Zig
compiler (`AstGen` → ZIR → `Sema` → AIR, where AIR is the first *typed* IR and
`Type`/`Value`/`TypedValue` are separate concepts), Roslyn's binder and
conversion APIs, Swift's `Sema`, Clang's `Sema`/conversion implementation, and
the C17 standard's conversion rules in 6.3.1.8 (usual arithmetic conversions),
6.3.1 (integer promotions and boolean conversion) and 6.5.16 (simple
assignment's constraint).

**Status: shipped.** `src/sema` implements everything below but the parts the
*Non-goals* section names, `mincc check` is its view, and the example corpus
passes it clean. Two things the plan did not have, and three it described
differently, are recorded where they belong: the compilation-wide `Context`
(§ *The typed AST*), the shorthand type names (§ *The type-specifier grammar*),
the store-enforced type budget and the token-tag spelling (§ *Decisions*, 14 and
15), and the corrected claim rows (§ *How the claims above are checked*).

## Why this stage exists, and what it is not

Three jobs, and each needs the whole file before it can be done at all:

1. **Resolve the spellings.** A `Type` node is an identifier run (`unsigned long
   long int`); turning that run into an actual type is the first time the
   compiler decides whether a program names a type that exists.
2. **Give every expression a type.** Nothing downstream — sizing a value,
   choosing a division instruction, deciding whether a store is legal — can
   begin until every expression has one.
3. **Convert.** C's implicit conversions are the language's arithmetic, not an
   optimisation; they are specified, and a compiler that gets them wrong
   computes a different program.

It is **not**:

- **a name resolver.** Every `PathExpr` already carries the declaration it
  denotes (`resolve.md`, decision 18). Sema asks that declaration for its type;
  it never searches a scope.
- **a shape checker.** Structural rules the parser could not make — a `let`
  with no type and no initializer, a `const` with no value — are `validate`'s
  (`ast_error.h`). Sema does not repeat them.
- **a syntactic error reporter.** Unterminated literals, missing `)`, missing
  `}` are the lexer's and the parser's; they already carry codes and spans.
- **a dataflow analysis.** "Is this variable initialised when read?", "does
  every path return?", "is this code unreachable?" are control-flow questions
  and belong with the IR (or with the specific checks that need a CFG). Sema
  answers only what a single pass over the tree can answer *exactly* — see
  *Reachability*, below, for the one place this matters today.

## Which stage owns which error

This is the answer to the question that usually starts this discussion, because
"the compiler should say something clear" is four different stages' job, and a
rule put in the wrong one is either unreachable or reported twice.

| Input | Stage | Code | What the reader sees |
| --- | --- | --- | --- |
| `fn i32 f(` | parser | `parse-expected-token` | `expected ')'`, caret where the token is missing |
| `"abc` | lexer | `lex-unterminated-string` | `unterminated string literal` |
| `{` with no `}` | parser | `parse-expected-token` | `expected '}'` |
| `fn main()` | parser | `parse-expected-type` | `expected a return type before the function name` |
| `fn i33 main()` | **sema** | `sema-unknown-type` | `i33` is not a type — with a suggestion when one is close |
| `fn i32 int main()` | **sema** | `sema-malformed-type` | `i32` and `int` name different types; pick one |
| `f(1, 2)` where `f` takes none | **sema** | `sema-argument-count` | `f` takes no arguments, 2 given |
| `1 = 2` | **sema** | `sema-invalid-assignment` | the left side of `=` is not a place a value can be stored |
| `const c = 1; c = 2;` | **sema** | `sema-assign-to-const` | `c` is a `const` |
| `fn i32 f() { return "x"; }` | **sema** | `sema-return-mismatch` | `str` returned from a function returning `i32` |
| `fn i32 f() { }` | **sema** | `sema-missing-return` | a function returning `i32` can reach its end without returning a value |
| `fn f64 main() { }` | **sema** | `sema-main-signature` | `main` must be `fn i32 main()` |
| `1 / 0` | **sema** | `sema-division-by-zero` | division by zero |

The two rules that keep this split honest, and that are testable:

- **Each condition has exactly one owning stage**, and the owning stage is the
  *earliest* one that can decide it without guessing. A type name is not a
  lexical fact, so only sema can reject `i33`; whether `(` is closed is a
  grammar fact, so only the parser can be right about it.
- **No stage reports a region an earlier one already reported.** The lowered
  AST carries `Node::inError`, inherited down the tree, and sema skips those
  nodes exactly as `validate` and `resolve` do. A syntax error never becomes a
  type error as well.

## The surface this stage must cover today

The nodes are the ones that exist (`parse/syntax_kind.h`), not the ones planned.
Everything below is reachable from a real input today, which is the project's
rule for a check: a rule lands when its syntax does, never as a placeholder.

| Node | What sema computes |
| --- | --- |
| `File` | the unit; checks the file-scope items in order |
| `Error` | skipped (`inError`); never a second diagnostic |
| `FnDecl` | the function's type (`ret`, `params`), then the body against `ret` |
| `ParamList` | empty today; a non-empty one is already an `Error` region the parser reported |
| `Param` | reserved, never produced by the grammar |
| `Block` | checks its statements in source order |
| `LetStmt` | the binding's type: the annotation, or the initializer's type defaulted |
| `ConstStmt` | the same, plus "this name may not be assigned"; `const` with no initializer is `validate`'s |
| `ReturnStmt` | the expression converts to the enclosing function's return type; `return;` requires `void` |
| `ExprStmt` | the expression's type; the value is discarded (so any type is allowed) |
| `EmptyStmt` | nothing |
| `Name` | the *declaration*, not an expression: its type comes from its `Def` |
| `Type` | the identifier run → `TypeId`, through the type-specifier grammar below |
| `LiteralExpr` | `char`, `str`, `bool` from the token; integer/float are *deferred* (see *Literals*) |
| `PathExpr` | the denoted declaration's type; `true`/`false` are predefined `bool` constants |
| `ParenExpr` | the inner expression's type; still an lvalue when the inner is one |
| `PrefixExpr` | `-` `+` `!` `~` `++` `--` |
| `PostfixExpr` | `++` `--`; requires a *modifiable* lvalue |
| `BinaryExpr` | the 18 operators of the parser's table, by class (arithmetic, shift, bitwise, comparison, logical, `,` absent until it parses) |
| `ConditionalExpr` | `?:` — condition `bool`, arms unify, result is an lvalue only if both arms are |
| `AssignExpr` | the 11 forms; the left side must be a modifiable lvalue, the right side converts to it |
| `CallExpr` | the callee has a function type; argument count matches |
| `ArgList` | the arguments, checked in order |
| `MacroCall`, `TokenTree`, `Attribute` | reserved: the grammar does not produce them. Sema must not crash if one appears — it reports nothing and yields `Error` |

Two things the grammar deliberately does **not** have, and that sema therefore
does not check: `*`/`&` (the parser's prefix set excludes them — `token_class.h`
says so — so there is no pointer expression to type), and parameters (a
non-empty `ParamList` is already an error). Pointers and parameters are the next
two increments; the type model below reserves their place without pretending to
implement them.

## Type representation

**A type is an interned value, compared by identity.** `TypeId` indexes a
`TypeStore` — structural, hash-consed, exactly like `support::Interner` for
names. Two types with the same structure are the *same* `TypeId`, so "are these
the same type?" is an integer compare, and it never depends on how the type was
spelled. `i32`, `int`, and `signed int` are one `TypeId`; that is what makes the
C spellings genuinely interchangeable instead of merely accepted.

```cpp
// A small enumerator plus data, not one kind per C spelling. `long` is not a
// kind: it is `Int{Signed, 64}` on SysV and `Int{Signed, 32}` on LLP64, so the
// width belongs to the *target table*, not to the type language.
enum class TypeKind : std::uint8_t {
  Error,     // the poison type (below)
  Void,
  Bool,
  Char,      // distinct from i8/u8, always unsigned (README, decided)
  Int,       // signedness + width in bits
  Float,     // width in bits; f80 is 80
  Str,       // NUL-terminated, C-like; scalar, not `char*` yet
  Function,  // return TypeId + parameter TypeIds
  // IntLiteral / FloatLiteral: the deferred literal types (below)
  IntLiteral,
  FloatLiteral,
  // Reserved with room: Pointer, Array, Struct, Enum, Union.
};

struct Type {
  TypeKind kind;
  bool isSigned;
  std::uint16_t width;         // bits, for Int/Float
  TypeId pointee;              // reserved
  TypeId returnType;           // Function
  std::span<const TypeId> params; // Function (arena-owned)
  support::SymId name;         // reserved: named types
};
```

- **Built-ins are pre-registered with stable ids** (`kTypeVoid`, `kTypeI32`,
  `kTypeF64`, …), so a dump is byte-stable across runs and the common compares
  are against a constant.
- **`Error` is a type, not a `nullopt`.** A type-checking failure must still
  leave behind a type that every later operation can consume silently.
- **The store lives beside the compilation, not beside a file.** A type must be
  comparable across files (a function in one file returning `i32` matches
  another). Today one `sema::Context` per run owns it; when translation units
  multiply it moves next to `TyCtxt`'s job — the session-level context — and
  nothing about this design changes.

### The poison type

`TypeKind::Error` is the whole mechanism behind "no cascades", and it is worth
being precise about, because it is the difference between one clear error and
twenty:

- Every operation that consumes an `Error` operand yields `Error` and reports
  **nothing**.
- `Error` is convertible to and from every type, silently. It never produces a
  mismatch.
- Exactly one expression — the one that could not be typed — produces the
  diagnostic. Everything above it inherits the poison and stays quiet.

This is rustc's `{error}` type and Clang's `RecoveryTy` by the same reasoning,
and it is a *type* here rather than a special case in every checker function
because that is the only version that cannot be forgotten in one place.
A type that fails to resolve (an unknown type name) poisons in the same way, so
`let x: i33 = 1 / 0;` is two independent errors, not four.

## The typed AST

The lowered AST is a **value**: arena-backed, immutable, hashable, and free of
semantics (`resolve.md`, decision 3, is why the `NameRef`s live in a parallel
array rather than in the node). Sema keeps that property. It does **not** add a
`type` field to `ast::Node`, for the same reason resolution did not: `src/ast`
would then depend on the type language, and the tree would stop being a pure,
comparable value that a formatter or the LSP can hold.

The typed artifact is a **parallel array**, indexed by `AstId`:

```cpp
struct TypedFile {
  std::vector<TypeId> typeTable;          // one per node, indexed by AstId
  std::vector<ExprInfo> exprFacts;        // lvalue / constant, indexed by AstId
  std::vector<FunctionInfo> functionTable; // per FnDecl: TypeId, return type
};

struct ExprInfo {
  bool isLvalue;          // `x`, `(x)`, `x++` is not; `1` is not
  bool isConstant;        // every operand was a literal or a constant
  bool hasIntValue;       // ... and the value is an integer
  ConstInt value;         // the folded value when hasIntValue
};
```

It does not hold the tree it describes. The checker borrows the lowered file for
the duration of the check and the *reader* is handed both, which is what keeps
the artifact a value a cache can own on its own.

Three properties fall out of this and are worth stating because they are the
reason for the shape:

- **`typeOf(e)` is total.** It is either a real type or `Error`, and it answers
  `Error` — rather than reading out of range — for an id from another unit;
  there is no "sema did not get here".
- **The tree stays hashable**, so the `(FileId, revision)` cache and the
  "typing a body never invalidates another body" invariant of `resolve.md`
  keep holding one stage higher.
- **`--ast` is a dump of `typeTable`,** not a second tree, so the printed form
  cannot drift from the checked form.

### `Context`, the compilation's checker

The stage has two ways in, and the difference is the point. `checkUnit` is pure:
same inputs, same output, no state kept, which is what the tests use. `Context`
is what a compilation uses: it owns the **one** `TypeStore` — a type has to mean
the same thing in every unit that compares against it — and it caches a unit's
answer per `(FileId, revision)`, returning the *same* `TypedFile` rather than a
second check that happens to agree.

The difference from `resolve`'s store is deliberate and is a fact about types
rather than a taste: resolution may reuse a unit when the **item tree** is
unchanged, because a body edit cannot change which names are visible. A type
*does* come out of a body, so there is no signature-level shortcut to take here,
and the revision is the whole key. Pretending otherwise would be the one bug this
store could have.

## Literals

A literal's type is the one place where C's rules are subtle enough to be
quoted, and where this language's own type set changes the answer.

- **`char` literal → `char`.** Not `i32` as in C, because `.mx` `char` is a
  distinct type and `let c: char = 'a';` must not be a conversion.
- **String literal → `str`.** `str` is scalar here, not `char[N]`; there is no
  array-to-pointer decay because there are no arrays yet.
- **`true` / `false` → `bool`** (they are predefined constants, `resolve.md`).
- **Integer and float literals are *deferred*.** A literal of `7` or `1.5` has
  `TypeKind::IntLiteral` / `FloatLiteral` until something decides it, exactly as
  C gives an unsuffixed integer constant its type by context ("the first type in
  the list that can represent its value") and modern compilers do with an
  unconstrained integer variable. Consequences:
  - `let x: u8 = 255;` is legal and `x` is `u8`, with no cast.
  - `let x = 7;` defaults the deferred type to `i32`; `let y = 1.5;` to `f64`.
  - `1 + 2.0` is `f64`, because one operand is a real `Float` and the deferred
    `IntLiteral` adopts it.
  - A deferred type that never meets a context is defaulted at the end of the
    initializer / argument / return expression: `i32`, `f64`.
  - **The value must fit the type it ends up with.** `let x: u8 = 256;` is one
    diagnostic at the literal (`sema-literal-out-of-range`), not a silent
    truncation. This is a deliberate divergence from C, where the constant is
    narrowed and the reader never learns.
- **No suffixes.** The lexer has none (`u`, `L`, `f` are not part of a literal
  token), so a literal never *demands* a type; context or the default decides.

## Conversions

Three conversion sites, and each names its rule once:

1. **Usual arithmetic conversions** (binary arithmetic, bitwise, relational,
   shift, and `?:`), per C17 6.3.1.8, with two adjustments for this language:
   - **integer promotions** first: `bool`, `char`, `i8`/`i16`, `u8`/`u16` →
     `i32` (int can represent every value of all of them, `char` and `u8`
     included because it is unsigned and 8-bit).
   - then the common type by signedness and *rank*, where rank is by width;
   - `bool`, `char` and `str` are **not** arithmetic, so any arithmetic
     operator on them is `sema-invalid-operands` rather than a silent promotion.
     (C promotes `bool` to `int`; this language does not — the checklist's
     `char`-unsigned decision is the precedent: a footgun C inherited is not
     automatically kept.)
2. **Assignment conversion** (initializer, assignment, argument, `return`): any
   arithmetic type converts to any arithmetic type, silently, because C's
   implicit narrowing is what makes C code compile at all and there are no casts
   in the grammar yet. This is the one place C's permissiveness is kept
   wholesale, and it is kept deliberately:
   - `examples/003_types.mx` returns a sum of `int`, `uint`, `long`, `i8`,
     `u8` and more from a function declared `i32`. Without casts, the only
     honest options are "silent like C" or "the documented corpus does not
     compile", and a documented example must compile.
   - The diagnosis a reader wants here is a *lint*, not an error:
     `-Wconversion` is designed for, off by default, and lands with parameters,
     where the argument case becomes the common one.
3. **Boolean conversion**: a `bool` converts to `bool`, and an arithmetic type
   **does not** convert to `bool` implicitly. A condition (today only `?:`)
   requires `bool` (`sema-condition-not-bool`), and `!` / `&&` / `||` require
   `bool` operands. C's "any scalar is a condition" is exactly the kind of
   implicit conversion this language rejects; the escape is explicit and one
   keystroke longer: `x != 0`.

`str` is scalar but not arithmetic: it may be assigned, returned, passed, and
compared with `==`/`!=` **only against `str`**, and it may not be added,
incremented, or ordered. Ordering and content comparison are library calls, not
operators — C's `str1 == str2` comparing addresses is the footgun this rule
exists to remove.

## Expressions, by class

| Class | Rule | Failure |
| --- | --- | --- |
| `-`, `+`, `~` | operand arithmetic; result is the promoted operand (so `-x` on `u8` is `i32`) | `sema-invalid-operands` |
| `!` | operand `bool`; result `bool` | `sema-condition-not-bool` |
| `++`, `--` (prefix and postfix) | operand is a *modifiable* lvalue and arithmetic | `sema-incdec-not-lvalue` / `sema-assign-to-const` |
| `* / %` | both arithmetic; `%` requires integers (C requires it too) | `sema-invalid-operands` |
| `+ -` | both arithmetic (pointer arithmetic lands with pointers) | `sema-invalid-operands` |
| `<< >>` | both integers after promotion; count is not itself an error (C leaves an out-of-range shift undefined — that stays a runtime property, documented as such) | `sema-invalid-operands` |
| `& \| ^` | both integers | `sema-invalid-operands` |
| `< <= > >=` | both arithmetic, same conversion; result `bool` | `sema-invalid-operands` |
| `== !=` | both arithmetic, or both `str`, or both `bool`; result `bool` | `sema-invalid-operands` |
| `&& \|\|` | both `bool`; result `bool`; short-circuit is the IR's business | `sema-condition-not-bool` |
| `?:` | condition `bool`; arms unify by usual arithmetic conversion, or are the same type; result is an lvalue only when both arms are lvalues of the same type | `sema-condition-not-bool` / `sema-invalid-operands` |
| assignment | left is a modifiable lvalue; right converts to the left's (unqualified) type; the whole expression is not an lvalue | `sema-invalid-assignment` / `sema-assign-to-const` |
| call | callee's type is `Function`; argument count matches; each argument converts to its parameter | `sema-not-a-function` / `sema-argument-count` |

**Value category** is a property of an expression, not of its type: a name, a
parenthesised name, and a `?:` whose arms are both lvalues, are *modifiable
lvalues* unless the declaration is a `const`. A `const` declaration's name is an
lvalue that is **not** modifiable; assignment and `++`/`--` to it are
`sema-assign-to-const`, which is exactly why `resolve` records
`DefKind::Constant` rather than folding `const` into `let`.

## Statements and functions

- **`let x: T = e;`** — `e` converts to `T`. `let x = e;` — `x`'s type is `e`'s
  type after defaulting the deferred literals. `let x: T;` — legal (an
  uninitialised binding); reading it before assignment is a *definite
  assignment* question and therefore the IR's, not sema's — recorded as a
  deliberate boundary rather than an oversight.
- **`const`** — identical typing; the difference is the modifiability above.
- **`return`** — `return e;` converts `e` to the function's return type;
  `return;` is legal only in a `void` function; `return e;` in a `void`
  function is `sema-return-void-value`.
- **Missing return** — a non-`void` function whose body can reach its end is
  `sema-missing-return`. **The check is exact today and will not stay that way:**
  with no `if`/loops in the grammar, the only way to fall off the end is to not
  end with a `return`, so the rule is a look at the last statement, and it is
  right for every program the parser accepts. When branches land it becomes a
  reachability question and moves to the IR with the other flow analyses; this
  is stated now so the move is a known improvement rather than a surprise.
- **Unreachable code** — a statement after a `return` in the same block is
  `sema-unreachable-code`, a warning today because the same thing will be a real
  flow result later.
- **`main`** — a file-scope `fn` named `main` must be `fn i32 main()`:
  `sema-main-signature` otherwise. A program with no `main` is not an error
  here: sema sees one translation unit and the entry point is a program
  property, which is `link`'s.
- **`void`** — decided with this stage, because a language without it cannot
  write a function that returns nothing. `void` is a type; it is not a value
  type: no object may have it (`let x: void` is `sema-type-not-value`), no
  arithmetic is defined on it, a `void` expression may not be used as a value,
  and `return;` is its only meaningful statement form.

## The type-specifier grammar

A `Type` node is an identifier run, so sema parses it. The grammar is small, and
the point of writing it down is that **every rejection has a sentence**:

```
type        := primitive | shorthand | c-specifier-seq
primitive   := i8|i16|i32|i64|i128|isize | u8|u16|u32|u64|u128|usize
             | f32|f64|f80 | bool | char | str | void
             | size_t | ssize_t | ptrdiff_t        (aliases for usize/isize)
shorthand   := uint      (unsigned int)
             | __int128 | unsigned __int128      (the C spelling of i128/u128)
c-specifier := signed | unsigned
             | short | long | long long
             | int | char | float | double
```

- A **shorthand is expanded, not special-cased**: `uint` becomes the words
  `unsigned int` before anything is interpreted, so it cannot reach a code path a
  spelled-out type does not and `uint i32` earns the same refusal
  `unsigned int i32` does. Matching is longest-first, which is the only way
  `unsigned __int128` can mean `u128` instead of "`unsigned` applied to `i128`" —
  the exact combination the reader exists to refuse.
- At most one of each group; `int` is implied when a base is absent
  (`unsigned` = `unsigned int`, `long` = `long int`).
- A primitive is a *whole* type and may not combine: `unsigned i32`,
  `i32 int`, `long u8` are `sema-malformed-type`.
- Floats take no signedness: `unsigned float`, `unsigned double` are refused.
- `long long long` is refused; `short long` is refused.
- `char` may take signedness: `signed char` → `i8`, `unsigned char` → `u8`,
  `char` → `char` (the unsigned one, per README).
- The **widths come from a target table**, selected for the target ABI, never
  from `#ifdef`s in the checker: on SysV AMD64 `long` is `Int{Signed,64}` and
  `long double` is `f80`; on LLP64 `long` is `Int{Signed,32}` and `long double`
  is `f64`. One table, one place, and the compiler's own platform is not
  consulted — the *target's* ABI is. This is the cross-platform rule the rest of
  the project already follows for `support/fs`, applied to types.
- An unknown word is `sema-unknown-type`, with a suggestion when the edit
  distance is small — the one bounded edit distance in `support/text`, shared
  with resolution rather than written twice.
- The suggestion table is exactly the words the reader understands, and a test
  asserts every one of them is a type on its own: suggesting a spelling that
  then fails would be the worst possible answer to a typo.
- `char` is both a primitive and a C specifier word, so a run that is *all* C
  specifier words is handed to the state machine before the primitive scan: that
  is what makes `signed char` an `i8` rather than "a primitive combined with
  another word".

## Errors as values

Same contract as every other stage: `minc_sema` links no diagnostics, and
`sema_report` is the only target that turns the table into `DiagBag` entries.

| Code | Severity | Condition |
| --- | --- | --- |
| `sema-unknown-type` | error | an identifier run in a type position names no type |
| `sema-malformed-type` | error | type specifiers that cannot combine |
| `sema-type-not-value` | error | `void` (or another non-value type) where a value is required |
| `sema-literal-out-of-range` | error | a deferred literal does not fit the type context gives it |
| `sema-condition-not-bool` | error | a condition, `!`, `&&` or `\|\|` operand that is not `bool` |
| `sema-invalid-operands` | error | an operator applied to types it does not accept |
| `sema-invalid-assignment` | error | the left side of an assignment is not a modifiable lvalue |
| `sema-assign-to-const` | error | assignment or `++`/`--` to a `const` declaration |
| `sema-incdec-not-lvalue` | error | `++`/`--` on something that is not an lvalue |
| `sema-not-a-function` | error | calling a value whose type is not `Function` |
| `sema-argument-count` | error | wrong number of arguments |
| `sema-return-mismatch` | error | the returned expression does not convert to the return type |
| `sema-return-missing-value` | error | `return;` in a non-`void` function |
| `sema-return-void-value` | error | `return e;` in a `void` function |
| `sema-missing-return` | error | a non-`void` function can reach its end |
| `sema-main-signature` | error | `main` is not `fn i32 main()` |
| `sema-division-by-zero` | error | a constant division or remainder by zero |
| `sema-unreachable-code` | warning | a statement after `return` in the same block |
| `sema-limit-types`, `sema-limit-type-depth`, `sema-limit-errors` | error | a budget was reached |

The rules that carry over verbatim from the stages below:

- **One name, one diagnostic.** No cascades; `Error` swallows everything above.
- **Every code is reachable.** One named input per code, asserted by a sweep
  over the table, so a code with no trigger fails a test instead of shipping.
- **Diagnostics in `(file, offset)` order**, tables in declaration order, no
  hash iteration in output.
- **Budgets are always on**, checked before the allocation, and the deep-input
  test runs under ASan/UBSan. Sema recurses only within the depth the parser
  already bounded (asserted at entry, as lowering does), and the type store has
  a depth bound of its own so a type built by later pointer/array syntax cannot
  make a comparison or a dump recurse without limit.
- **Errors are values with a span and a stable code**; the message is written
  once, in the checker, next to the rule that produced it.

## Constant folding, and the one evaluator

Sema folds expressions whose operands are all constants: literals, `const`
bindings with constant initializers, and arithmetic on them. Two things use it
today — `sema-division-by-zero` needs one, and `--ast` prints the folded value —
and three more will: array bounds, `enum` values, and `static` initialisers.

**It must not be a second definition of "integer arithmetic".** The
preprocessor already evaluates 64-bit constant expressions for `#if`
(`src/pp/const_expr.cc`) with decided semantics: wraparound defined, division by
zero diagnosed, short-circuit branches not evaluated. Sema needs the same
arithmetic over a different input (a typed AST instead of PP tokens). The design
is therefore to **extract the numeric core** — a `ConstInt` with defined
operations and its overflow/division rules — into `support`, and have both
stages call it. Two evaluators that agree today are two evaluators that
disagree the first time one is fixed; this is the same centralisation argument
that made the lexer own `##`'s re-lexing, and the refactor is part of this
stage's work rather than a later cleanup.

## The command

`mincc check` is the stage's view: preprocess, parse, lower, validate, resolve,
then type-check; diagnostics on stderr, the tables on stdout; exit `Failure`
when there is an error, `Ok` when there is only a warning.

| Flag | Shows |
| --- | --- |
| default | the type table, then one summary line per file, and every diagnostic, with a caret |
| `--ast` | the typed tree: the function table as checked, then every node with the type it was given and the folded constant where there is one |
| `--types` | only the type table: every type the compilation knows, with its spelling, size and alignment |
| `--target NAME` | the ABI the C spellings are read against (`systemv-amd64`, `windows-x64`) |
| `-Wconversion` | warn on the implicit narrowing of the assignment conversion (off by default) |

`-Wunused` and `-Wshadow` are accepted too and forwarded to resolution, which
is where they are decided: a warning a user asked for must not depend on which
command they happened to run.

The typed dump is deliberately the lowered dump plus a type column, so a reader
comparing `mincc resolve --ast` and `mincc check --ast` sees *only* what sema
added. The typing comes from the compilation's `Context`, not from a one-off
call, so the cache the language server will live on is exercised by the command
that proves the stage.

## Decisions

The ones this stage had to make, each with the reason and the cost of the other
answer. They are recorded in the `README.md` checklist (section *Types* and
*Scopes and names*), which is the user-facing copy.

| # | Question | Decision | If the other way |
| --- | --- | --- | --- |
| 1 | Is a type a spelling or an identity? | **Interned identity** (`TypeId`); `i32`, `int`, `signed int` are one type | Comparing spellings means `int` and `i32` are different types, and C interop breaks at the first declaration |
| 2 | Does the typed AST mutate the lowered tree? | **No** — a parallel `TypeId` array beside it | A `type` field on `ast::Node` makes `src/ast` depend on the type language and stops the tree being a comparable value |
| 3 | What does a failed expression type as? | **`Error`, a real type that silently unifies** | Every checker function needs its own "was this already reported?" flag, and one of them will be forgotten |
| 4 | Do integer/float literals have a type, or a *deferred* one? | **Deferred**, decided by context, defaulted at the end | C's fixed list makes `let x: u8 = 255;` a narrowing conversation, and needs suffixes the lexer does not have |
| 5 | Is narrowing implicit? | **Yes at assignment**, as in C, with `-Wconversion` for the diagnosis | An error without casts means the documented examples do not compile |
| 6 | Does an arithmetic value convert to `bool` implicitly? | **No** — conditions require `bool` | C's "any scalar is a condition" is a footgun this language rejects elsewhere |
| 7 | Is `bool` arithmetic? | **No** — `!`, `&&`, `\|\|`, `==`, `!=` only | C promotes `bool` to `int`; `true + true` is then a silent 2 |
| 8 | Is `str` arithmetic, and does `==` compare contents? | **`str` is scalar, not arithmetic; `==` is pointer-shaped and therefore refused** — use a library call | C's `s1 == s2` compares addresses; keeping it would keep the bug |
| 9 | Does `void` land now? | **Yes** — a function that returns nothing is not optional | Deferring it means every unit's functions return a value, which is not a language |
| 10 | Is falling off the end of a value-returning function an error? | **Yes**, while it is exact (no branches in the grammar yet) | C makes it UB with a warning; minc+ rejects C's UB by default |
| 11 | Does sema check definite assignment? | **No** — the IR's, with the other flow analyses | Doing it here without a CFG means either a false negative or a wrong error |
| 12 | One constant-arithmetic implementation or two? | **One**, in `support`, shared with `#if` | Two evaluators disagree the first time one is fixed |
| 13 | Where does the C spelling's width come from? | **The target's ABI table**, never the host's `#ifdef`s | `${host}` widths make a cross-compile silently wrong, which is the failure mode this project is built to avoid |
| 14 | Who owns the type budget? | **The `TypeStore`**, checked before every insert, with `SemaOptions::maxTypes` lowering it | A budget the checker owned would be enforced after the allocation, and the two could disagree about which limit was hit |
| 15 | How does the checker name a token, given that tokens and nodes share one tag space? | **As integer tags** (`tokens.h`), so the switches on them switch on a number | A token value is not an `SyntaxKind` enumerator, so a switch on it is either a `-Wswitch` error under CI or a second enumerator per token that has to be kept in step |
| 16 | Does a `void` binding get one diagnostic or two? | **One**, reported before the initializer is checked | `let x: void = 1;` would otherwise also report "cannot be assigned to `void`", which is the same mistake said twice |

## Non-goals

- **Pointers, arrays, structs, `enum`, function pointers, casts.** The syntax
  does not exist yet; the type model reserves their kinds and nothing pretends
  to check them. `*` and `&` are absent from the prefix set on purpose.
- **Parameters.** The parser reports a non-empty `ParamList`; sema skips the
  `Error` region. When parameters land, `sema-argument-type` joins the table and
  the `Function` type already has the shape for it.
- **Generic inference.** No Hindley–Milner: one pass, one direction, and the
  only inference is the deferred literal and the initializer's type. A type
  flowing *backwards* from a later use is out of scope by construction.
- **Optimisation.** Folding for diagnosis and for `const` propagation to the IR
  is not an optimiser; the constant value is evidence, not a transformation.
- **A `sema` command of its own.** The stage's view is `mincc check`; a second
  name for one pipeline prefix is a name to keep in sync.

## How the claims above are checked

| Claim | Checked by |
| --- | --- |
| Every example type-checks | the whole `examples/` corpus through the real pipeline, zero diagnostics, in `sema/examples_test.cc`; and `mincc check` is run over the corpus by `make examples` |
| Every code is reachable | one named input per `SemaErrorCode`, plus the sweep asserting the set of reached codes *equals* `allSemaErrorCodes()` |
| Types are identities | spelling-variant test: `int` and `i32` produce one `TypeId`; `i32`/`u32`/`i64` are distinct |
| Conversions match C | `convert_test.cc`: one case per rule — promotion of every small type, equal-rank unsigned winning, a wider signed winning, a float winning from either side, and the two deliberate departures (`bool`/`str`) |
| Deferred literals default | `1`, `1.5` as whole initializers are `i32`, `f64`; `let x: u8 = 255` is `u8`; `let x: u8 = 256` is one error; a value past 64 bits is accepted only where the context can hold it |
| No cascades | an unknown type in a signature produces one error however many calls sit on top of it; a name resolution already reported adds nothing here; a parse error adds nothing here; `let x: void = 1;` is one diagnostic, not two |
| The boundary with `validate` holds | a `let` with no type and no initializer and a `const` with no value are `ast-missing-type` / `ast-const-without-value`, and `sema` has no code for either |
| The tree is untouched | *by construction*: the checker holds `const ast::LoweredFile&` and the artifact is a separate array, so there is no mutating path to test — and `resolve --ast` output still matches after checking |
| Deep input is a diagnostic | an expression 5 000 levels deep is checked under ASan/UBSan without a crash, and the depth bound is reported as `sema-limit-types` when the parser lets it through |
| Determinism | the same unit dumped twice is byte-identical: no addresses, no hash-order iteration |
| The store is a cache and not a second opinion | the same `(FileId, revision)` returns the same pointer (hits/misses counted); a new revision rechecks even for structurally identical text; two files share one type store and neither evicts the other |
| `main` | `fn i32 main()` passes, `fn f64 main()` and `fn void main()` are `sema-main-signature`, and a unit with no `main` is not this stage's finding |
