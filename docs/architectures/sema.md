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
| `ParamList` | the parameters, each a binding `name: type`, checked in order |
| `Param` | the parameter's type; a `void` parameter is refused (there is no value to pass) |
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
| `PrefixExpr` | `-` `+` `!` `~` `++` `--`, plus `&` (the address of a **modifiable** lvalue) and `*` (a place of the pointee's type, which is one access) |
| `PostfixExpr` | `++` `--`; requires a *modifiable* lvalue — a pointer is *stepped*, an arithmetic value incremented, and the scaling belongs to the type |
| `IndexExpr` | `p[i]`: `p` is a pointer, `i` an integer converted at the pointer width; the result is a place of the pointee, and one access |
| `BinaryExpr` | the 18 operators of the parser's table, by class (arithmetic, shift, bitwise, comparison, logical, `,` absent until it parses) |
| `ConditionalExpr` | `?:` — condition `bool`, arms unify, result is an lvalue only if both arms are |
| `AssignExpr` | the 11 forms; the left side must be a modifiable lvalue, the right side converts to it |
| `CallExpr` | the callee has a function type; argument count matches |
| `ArgList` | the arguments, checked in order |
| `MacroCall`, `TokenTree`, `Attribute` | reserved: the grammar does not produce them. Sema must not crash if one appears — it reports nothing and yields `Error` |

Sema types the pointer forms the grammar produces -- `*T` at any depth, `&x`,
`*p`, `p[i]`, the stepping, the comparison, `null` and `*void` -- and publishes
one `AccessObligation` per dereference for the lowering to read. Two things the
grammar still does **not** produce, and that sema therefore does not check:
aggregates (arrays, `struct`, and the decay that comes with them) and the
operations that join a pointer and an integer (`expose` /
`with_exposed_provenance`, `memory.md`'s stage three). The type model below
reserves their place without pretending to implement them.

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
  Never,     // the bottom type `!`: no values at all (never.md)
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
  are against a constant. The list is **append-only**: `!` is id 20, added last
  rather than beside the `Void` its kind sits next to, because every constant
  above it is a promise to the dumps and the tests.
- **`Error` is a type, not a `nullopt`.** A type-checking failure must still
  leave behind a type that every later operation can consume silently.
- **The store lives beside the compilation, not beside a file.** A type must be
  comparable across files (a function in one file returning `i32` matches
  another). Today one `sema::Context` per run owns it; when translation units
  multiply it moves next to `TyCtxt`'s job — the session-level context — and
  nothing about this design changes.

### The bottom type

`TypeKind::Never`, written `!`, is the type of an expression that never produces
a value — a call to a function that does not return. It is not `void`, and the
difference is exactly what the two words say: `void` is "produces nothing and
comes back", `!` is "never comes back". At every consumer that means a *value*
where a value is expected, and the two types are refused for different reasons:
`void` cannot be stored because there is nothing there, `!` cannot be stored
because the store is never reached.

The rule is one arm of `convertible`: **`!` converts into every type, and nothing
converts into `!`.** A value of type `!` is never produced, so "it becomes a `T`"
is vacuous rather than false; the other direction has no reading at all. That one
arm is what makes `c ? 1 : die()` an `i32` and `return die();` legal in a `void`
function, and it is why flow needs no second mechanism: `diverges(expr)` is
`typeOf(expr) == !`, the answer the checker already wrote down.

`!` is *written* only where a return type goes — `let x: !` and `f(x: !)` are
refused — and a body that claims it is **proved** divergent rather than believed
(a reachable `return`, or a body that can reach its end, is `sema-never-returns` /
`sema-never-body-completes`). Full record:
[`never.md`](never.md).

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
  // The conversions, in `(consumer, operand)` order, plus a per-consumer index.
  std::span<const Coercion> coercions() const;
  std::span<const Coercion> coercionsOf(ast::AstId consumer) const;
  const Coercion* coercionAt(ast::AstId consumer, std::uint8_t operand) const;
};

struct ExprInfo {
  bool isLvalue;          // `x`, `(x)`, `x++` is not; `1` is not
  bool isConstant;        // every operand was a literal or a constant
  bool hasIntValue;       // ... and the value is an integer
  ConstInt value;         // the folded value when hasIntValue
  TypeId opType;          // `op=` only: the type the operation happens at
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

### What the artifact publishes

The IR cannot re-derive three things without holding a second copy of this
stage's rules, and all three are therefore part of the artifact (`ir.md`, *The
coercion record*, and `memory.md`, *The record the lowering is not allowed to
re-derive*):

- **Every implicit conversion, as a pair of types, keyed on the consumer that
  applies it.** `coercionAt(consumer, operand)` answers "what does this node do
  with this operand", and `from` is always the operand's own final type, so the
  lowering is a *materialiser*: it reads a pair and emits `sext`/`zext`/
  `trunc`/`sitofp`/`fptosi`, with no rule of its own. A conversion that changes
  nothing is not recorded — "no entry" means "no conversion" — and neither is a
  pair the language refuses.
- **The operation type of a compound assignment** (`ExprInfo::opType`). It is
  the one fact the tree cannot show: `x <<= 9` on a `u16` is typed `u16` and
  *operates* at `i32`, and a lowering that read the node's type would emit an
  out-of-range shift. The conversion of the result back into the target is
  implied by `opType != typeOf(expr)`.
- **Every access through a pointer** (`TypedFile::accesses()`, written by
  `src/sema/access.cc`). One `AccessObligation` per dereference — `*p` and
  `p[i]` — carrying what is accessed (its type is the access's width and
  alignment) and the `ProvenanceKind` the compiler could *prove*: `Object` for
  the address of an object this unit named and moved by arithmetic since,
  `Foreign` for everything it cannot name. The proof is syntactic and therefore
  sound and incomplete on purpose, and the direction it errs in is the only safe
  one. `accessAt(node)` is the question, keyed on the node the lowering stands
  on; a refused dereference is not recorded, because a tree with a `poison`
  access is never lowered.

... and one guarantee the record depends on, which is a property of the whole
table and not of any one entry:

> **No node in the artifact carries a deferred literal type.** A deferred type
> has no width, so it has no LLVM type at all; an operand left undecided is not a
> missing conversion but a wrong instruction.

Deciding happens twice, and the two are complementary rather than redundant. The
**seams** decide where the context is known — that is what makes `let x: u8 = 255;`
a `u8` with no conversion, and what `adaptTo` already did. Then a **sweep** walks
the unit's tree once, after the check, and decides whatever a seam did not reach:
a node with a concrete type is what its operands take after, so `1 + 2.0` in an
`f64` binding is two `f64`s and not an `i32` beside an `f64`. Anything the sweep
reaches with no context at all is decided at the language's default. The sweep is
what makes the guarantee true of paths nobody has written yet, and the examples
test asserts it over every node of every example.

Two consequences of "the context reaches the operands" are worth naming, because
they are where a reader could expect the opposite:

- **A literal the context cannot hold is an error, however deep the context
  reaches.** `let y: u8 = 300 / 3;` is refused at the `300`
  (`sema-literal-out-of-range`) rather than dividing the *truncated* `44` and
  storing `14` while the folded value says `100`. The rule is the README's — a
  literal that does not fit the type its context gives it is an error — applied
  to the operands the context reaches indirectly.
- **A negation is the exception, and it is not a special case in the rule but in
  the reading:** `-128` in an `i8` is representable where `128` is not, and both
  are the same literal, so the check reads the value through the parity of the
  unary minus between it and the context.

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
    initializer / argument / return expression: `i32`, `f64`. The decision is
    made at the seam *and* swept down the tree afterwards, so a literal the
    context reaches indirectly is decided too, and no deferred type is left in
    the artifact at all (*What the artifact publishes*).
  - **The value must fit the type it ends up with.** `let x: u8 = 256;` is one
    diagnostic at the literal (`sema-literal-out-of-range`), not a silent
    truncation. This is a deliberate divergence from C, where the constant is
    narrowed and the reader never learns. The same rule applies to the operands
    the context reaches through an operation: `let y: u8 = 300 / 3;` is refused
    at the `300`, because the alternative is emitting a division of the
    truncated value whose result no longer matches the constant the checker
    folded. A negation is read as the negation (`-128` in an `i8` is legal, and
    `128` in an `i8` is not).
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

4. **A compound assignment computes at the common type** of its target and its
   operand and stores the result converted back to the target's type (C
   6.5.16.2). So `x <<= n` shifts at `promote(x)`, not at `x`'s own type — which
   is why `let x: u16 = 1; x <<= 9;` is legal: the shift happens at `i32` and the
   result narrows on the store. **The operation type is part of the answer this
   stage publishes**, and not left for a later stage to recompute
   (`ir.md`, *The fourth fact nobody recorded*): a lowering that read the
   target's type instead would emit an out-of-range shift, and in LLVM that is
   not a diagnostic — it is a poison value the optimiser is licensed to replace.

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
| `+ -` | both arithmetic, **or** a pointer and an integer (scaled by the pointee, index materialised at the pointer width), **or** two pointers to one type for `-` (result `isize`) | `sema-invalid-operands` / `sema-pointer-mismatch` / `sema-pointer-void-arithmetic` |
| `<< >>` | both integers after promotion; a **constant** count at or past the width of the promoted left operand (or negative) is an error, because the alternative is a poison value in LLVM — see *Integer arithmetic at the edges* | `sema-invalid-operands` / `sema-shift-count-out-of-range` |
| `& \| ^` | both integers | `sema-invalid-operands` |
| `< <= > >=` | both arithmetic, same conversion; result `bool` | `sema-invalid-operands` |
| `== !=` | both arithmetic, or both `str`, or both `bool`; result `bool` | `sema-invalid-operands` |
| `&& \|\|` | both `bool`; result `bool`; short-circuit is the IR's business | `sema-condition-not-bool` |
| `?:` | condition `bool`; arms unify by usual arithmetic conversion, or are the same type; result is an lvalue only when both arms are lvalues of the same type | `sema-condition-not-bool` / `sema-invalid-operands` |
| assignment | left is a modifiable lvalue; right converts to the left's (unqualified) type; the whole expression is not an lvalue | `sema-invalid-assignment` / `sema-assign-to-const` |
| call | callee's type is `Function`; the count matches exactly — or is *at least* the declared count when the callee's type is variadic; each argument converts to its parameter, and an argument past the last parameter gets the ABI's default promotion | `sema-not-a-function` / `sema-argument-count` |

**Value category** is a property of an expression, not of its type: a name, a
parenthesised name, and a `?:` whose arms are both lvalues, are *modifiable
lvalues* unless the declaration is a `const`. A `const` declaration's name is an
lvalue that is **not** modifiable; assignment and `++`/`--` to it are
`sema-assign-to-const`, which is exactly why `resolve` records
`DefKind::Constant` rather than folding `const` into `let`.

## Statements and functions

- **`let x: T = e;`** — `e` converts to `T`. `let x = e;` — `x`'s type is `e`'s
  type after defaulting the deferred literals. `let x: T;` — legal (an
  uninitialised binding), and reading it before an assignment reaches it is
  `sema-use-before-assignment`: see *Definite assignment* below, which is where
  that question is answered and where the earlier "the IR's" decision is
  reversed and why.
- **`const`** — identical typing; the difference is the modifiability above.
- **`return`** — `return e;` converts `e` to the function's return type;
  `return;` is legal only in a `void` function; `return e;` in a `void`
  function is `sema-return-void-value`.
- **Missing return** — a non-`void` function whose body can reach its end is
  `sema-missing-return`. The question is reachability, and `terminates()`
  answers it exactly rather than conservatively: a `return`; a block whose last
  reachable statement terminates; an `if` whose **both** arms terminate; a loop
  whose condition is constantly `true` and whose body has no `break` aimed at
  it. So `fn i32 f(c: bool) { if c { return 1; } else { return 2; } }` has no
  diagnostic and `fn i32 f() { while true { } }` has none either. Two facts are
  load-bearing and both already exist: the conditions are folded (so "constantly
  true" is not a guess), and a `break` belonging to *this* loop is a question the
  statement walk answers (`hasBreakForThisLoop`). It stays here rather than
  moving to the IR because it is not a CFG question in this language: the
  constructs are structured, and the answer is exact, which is strictly better
  than deferring it to a stage that would have to re-derive it.
- **Unreachable code** — a statement after one that never completes, in the same
  block, is `sema-unreachable-code`, a warning rather than an error: it is a
  statement the program cannot reach, which is a smell and not a defect, and the
  analysis is deliberately per block so the sentence says "the statement before
  this one never completes".
- **`main`** — a file-scope `fn` named `main` must be `fn i32 main()`:
  `sema-main-signature` otherwise. A program with no `main` is not an error
  here: sema sees one translation unit and the entry point is a program
  property, which is `link`'s.
- **One function, one definition and one signature.** A name may be declared
  more than once — `extern fn i32 f();` above `fn i32 f() { }`, or a header
  included twice — and the declarations are checked against each other here,
  because what has to agree is a *type*: two bodies are
  `sema-function-redefinition` and two signatures are
  `sema-signature-mismatch`, each pointing at the second declaration with the
  first as a note. The check is what makes the identity `resolve` computed
  *safe* rather than merely true: with every declaration of a name carrying one
  type, `defTypes_` has one value no matter which declaration wrote it last, so
  the ordering question disappears instead of being answered
  ([`extern.md`](extern.md)).
- **A variadic call** — a declaration whose parameter list ends in `...` accepts
  *more* arguments than it names, so the count check is a minimum for one and an
  equality for every other function. The arguments past the last parameter have
  no parameter to be checked against, and the only rule left is the ABI's
  **default argument promotion** (`bool`/`char`/`i8`/`i16`/`u8`/`u16` to `i32`,
  `f32` to `f64`, everything else as it is), recorded as a conversion so the
  lowering materialises it like any other. The marker is part of the *function
  type* and not a flag beside it (`fn i32(str, ...)`), which is what makes a
  variadic declaration and a fixed definition a `sema-signature-mismatch` rather
  than one function — and what stops a variadic call from type-checking against
  a callee that cannot read the extra arguments ([`extern.md`](extern.md)).
- **`void`** — decided with this stage, because a language without it cannot
  write a function that returns nothing. `void` is a type; it is not a value
  type: no object may have it (`let x: void` is `sema-type-not-value`), no
  arithmetic is defined on it, a `void` expression may not be used as a value,
  and `return;` is its only meaningful statement form.

## Definite assignment

`let x: i32;` is the C idiom — declare, then assign in the branch that knows the
answer — and it is the one hole through which this language could read an object
nobody ever wrote. It is now closed here, in a pass of its own
(`src/sema/check_flow.cc`), and reading such a binding on a path that never
assigned it is `sema-use-before-assignment`.

**This reverses decision 11 below**, which left the question to the IR, and the
reason it reverses is what the research settled: the analysis is *not* a CFG
question. Every language that closed this hole closed it the same way — Java
specifies it in full (JLS 16), C# and Swift make it an error rather than a
warning, Rust refuses to hand out a value it cannot prove was written — and all
of them answer it from the *structure* of the language, not from a dataflow
fixpoint. The constructs here are structured, the conditions already fold
(telling `while true` from `while c` is not a guess), and a `break` belonging to
which loop is a question this stage already answers. So the answer is exact, and
leaving it to the IR would have meant a stage that has the tree anyway
re-deriving a fact the tree already decides — and, until it did, an `undef`
lowered as if it were a value.

**The rules, per construct.** The state is the set of definitions that hold a
value, and a conditional merges by intersection because only one side runs:

| Construct | State after it |
| --- | --- |
| `S1; S2` | the state after `S1` is the state before `S2` |
| `if (c) A else B` | `after(A) ∩ after(B)` |
| `if (c) A` | the state before the `if` — the arm may not run |
| `while (c) S` | the state before the loop: it can run zero times, so nothing the body assigns survives |
| `while (true) S` | nothing falls out of the bottom, so the exit state is the intersection of the states at its own `break`s; with no `break`, nothing after the loop is reachable and there is nothing to prove |
| `for (i; c; s) S` | the state after `i`; the step is checked with what the body ended in, and contributes nothing to the exit (the exit that skips the body is always possible) |
| `a && b`, `a \|\| b` | intersection: `b` may not run |
| `c ? a : b` | intersection: only one arm runs |
| `x = e` | the state before, plus `x` — a store, and the target is not read |
| `x op= e`, `x++`, `++x` | the read is checked first, then the store |
| `f(a, b)` | the callee and then the arguments, left to right |

**What it is deliberately conservative about.** A value assigned inside a loop
body and read by the loop's *condition* on a later iteration is "not proved", and
the code after a loop never inherits what the body assigned. A dataflow fixpoint
would be more precise in both places and would also be a second implementation
of control flow that could disagree with `terminates()`; the conservative side is
the one with no false negatives, and the precision that matters — the
`while true` plus `break` idiom — is bought exactly, not approximated.

**One diagnostic per binding.** The fix is one assignment before the reads, so
five uninitialised reads of one binding are one sentence rather than five: the
same "one mistake, one diagnostic" rule the poison type exists to enforce.

**And the IR does not have to re-check it.** A read that is not proved is an
error, an error means the front end does not hand a typed tree to lowering, and
so the lowering never has to decide what an uninitialised value is worth. That
is the whole point of doing it here: the boundary the earlier decision drew was
a promise the IR could not keep on its own.

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
| `sema-function-redefinition` | error | two definitions of one function |
| `sema-signature-mismatch` | error | two declarations of one function with different signatures |
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
  a depth bound of its own so a type built by later array/aggregate syntax cannot
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

## Integer arithmetic at the edges

The language has **no undefined behaviour in integer arithmetic**, and this stage
is where that stops being a slogan: every edge case below is either diagnosed
here or defined with a sentence the backend must honour. The alternative — inheriting
the backend's answer — is worse than C's UB, because LLVM's poison value does not
merely "do something"; it lets an optimizer *change a comparison* based on it.
The market's answers, for reference: Rust panics on overflow in debug and is
specified to wrap in release, Zig traps by default and offers `+%` for wrapping,
Go/Java/C# wrap, C and C++ leave signed overflow undefined.

| Case | Decide, and what the IR must emit |
| --- | --- |
| Signed overflow at runtime (`a + b` past `MAX`) | **Wrap**, two's complement. The lowering therefore emits **no `nsw`**: promising no wrap would be a lie, and `nuw`/`nsw` are exactly the promise that turns a defined wrap into a poison value. |
| Unsigned overflow | **Wrap**, as C defines it, for the same reason and with the same constraint (`nuw` is not a promise this language can make). |
| A constant that does not fit its type (`2147483647 + 1`, `INT_MIN / -1`) | **Error** here (`sema-constant-out-of-range`), on both paths: the deferred literal one, where the context decides the type, and the already-typed one, where nothing downstream would re-check the result. The runtime operation wraps; a constant the compiler can see does not fit is a mistake it can point at. Rust and Zig refuse it for the same reason. |
| `x / 0`, `x % 0` at runtime | **Traps**, deterministically. The compile-time case is `sema-division-by-zero`. LLVM's `sdiv`/`srem` by zero is poison, so the lowering must emit an explicit zero test and a trap rather than the bare instruction. |
| `INT_MIN / -1` at runtime | **Traps**: the quotient is not representable in the type. LLVM is poison here too, so it is the same explicit test. |
| `INT_MIN % -1` | **0**, and not a trap: the remainder is representable, and LLVM's `srem` defines it. |
| Shift count negative, or at or past the width of the value moved | **Error** when constant (`sema-shift-count-out-of-range`), **trap** at runtime. LLVM gives poison for an out-of-range count, which is a `>>` that silently does not shift. The width is the *promoted left operand's* — `u8 << 8` is 32 bits' worth of shift, not 8 — and a deferred literal left operand uses `i32`, the language's default for a bare integer. |

**Evaluation order is decided, not inherited.** The language guarantees strict
left-to-right evaluation of operands and of argument lists, and `&&`, `||` and
`?:` evaluate only the side they take. C leaves the order of most operators
unspecified (and reserves the right to change it between two evaluations of the
same expression); Java, C# and Go specify left to right, and a language whose
result must be reproducible for the same source cannot do otherwise. The
lowering must produce that order — and now that this is written down, it is a
requirement rather than an accident of the tree's shape.

**One forward note the lowering depends on.** `str` is a NUL-terminated string
and therefore an *address*, but the language has no pointer type yet. It does not
need one: every LLVM the backend supports here uses **opaque pointers**, so `str`
maps to `ptr`, a literal becomes a private global, and no pointer syntax has to
appear in `.mx` for the IR to have one. Nothing to decide now; recorded so the
`ir` stage does not invent a language change it does not need.

## The command

`mincc check` is the stage's view: preprocess, parse, lower, validate, resolve,
then type-check. Diagnostics go to stderr in the usual format with a caret; the
exit code is `Failure` when there is an error and `Ok` when there is only a
warning.

**Stdout is empty unless something is asked for**, which is the behaviour a
compiler has and the reason the command is usable from a build script: a clean
file says nothing, and the exit code is the answer.

| Flag | Prints |
| --- | --- |
| *none* | nothing — the diagnostics are the output |
| `--stats` | one summary line per file: `# <path>  (scopes 2, defs 5, refs 1, functions 1)  0 error(s), 0 warning(s)` |
| `--types` | the type table, once for the invocation: every type the compilation knows, with its spelling, size and alignment |
| `--ast` | the typed tree: the function table as checked, then every node with the type it was given and the folded constant where there is one |
| `--target TRIPLE` | nothing by itself; it picks the ABI the C spellings are read against, as an LLVM triple in `arch-vendor-os[-env]` form (`x86_64-unknown-linux-gnu`, `x86_64-pc-windows-msvc`, `aarch64-unknown-linux-gnu`). A triple this compiler does not state, or one written in a shape that is ambiguous (`x86_64-linux-gnu`), is **refused with a sentence** rather than defaulted |
| `-Wconversion` | nothing by itself; it warns on the implicit narrowing of the assignment conversion (off by default) |

Each flag prints exactly one thing, so `--types --stats` is the table and the
summary and nothing more. `-Wunused` and `-Wshadow` are accepted too and
forwarded to resolution, which is where they are decided: a warning a user asked
for must not depend on which command they happened to run.

The typed dump is deliberately the lowered dump plus a type column, so a reader
comparing `mincc resolve --ast` and `mincc check --ast` sees *only* what sema
added — which is also why `--ast` does not print the type table: the table is
what `--types` is for. The typing comes from the compilation's `Context`, not
from a one-off call, so the cache the language server will live on is exercised
by the command that proves the stage.

## Decisions

The ones this stage had to make, each with the reason and the cost of the other
answer. They are recorded in the language feature checklist
(`website/docs/language/features.md`, sections *Types* and *Scopes and names*),
which is the user-facing copy.

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
| 10 | Is falling off the end of a value-returning function an error? | **Yes**, and exactly: `terminates()` follows the branches, so both arms returning is enough | C makes it UB with a warning; minc+ rejects C's UB by default, and an inexact answer here would reject working code |
| 11 | Does sema check definite assignment? | **Yes** — a structured pass over the body, the rules JLS 16 spells out, not a CFG — see *Definite assignment* | **This row used to say no**, with the reason "without a CFG, a false negative or a wrong error". Both halves were wrong: the answer is exact for this grammar's constructs, and leaving it to the IR means an `undef` the lowering has no way to recognise |
| 12 | One constant-arithmetic implementation or two? | **One**, in `support`, shared with `#if` | Two evaluators disagree the first time one is fixed |
| 13 | Where does the C spelling's width come from? | **The target's ABI table**, never the host's `#ifdef`s | `${host}` widths make a cross-compile silently wrong, which is the failure mode this project is built to avoid |
| 14 | Who owns the type budget? | **The `TypeStore`**, checked before every insert, with `SemaOptions::maxTypes` lowering it | A budget the checker owned would be enforced after the allocation, and the two could disagree about which limit was hit |
| 15 | How does the checker name a token, given that tokens and nodes share one tag space? | **As integer tags** (`tokens.h`), so the switches on them switch on a number | A token value is not an `SyntaxKind` enumerator, so a switch on it is either a `-Wswitch` error under CI or a second enumerator per token that has to be kept in step |
| 16 | Does a `void` binding get one diagnostic or two? | **One**, reported before the initializer is checked | `let x: void = 1;` would otherwise also report "cannot be assigned to `void`", which is the same mistake said twice |
| 17 | What happens on signed overflow? | **It wraps**, and the lowering emits no `nsw` | `nsw` trades a defined wrap for a poison value, and LLVM's poison can change a *comparison*: silently wrong is the failure mode this project is built to avoid. Trapping by default (Zig, Swift) is the alternative and needs a checked-operator pair, which is a later `[?]` |
| 18 | Division by zero, `INT_MIN / -1`, and an out-of-range shift count? | **Error when constant; trap at runtime**, `INT_MIN % -1` is 0 | The alternative is inheriting LLVM's poison, where `x / 0` is not a crash but a licence for the optimizer to delete the branch that guarded it |
| 19 | Is the evaluation order of operands and arguments specified? | **Yes — strict left to right**, with `&&`/`\|\|`/`?:` evaluating only the side they take | Leaving it unspecified (C) makes the same source mean two programs, which is incompatible with the "same input, same output" the stage contract is built on |
| 20 | How does `str` become an LLVM value, with no pointer type in the language? | **Opaque `ptr`** — every supported LLVM uses opaque pointers, so a literal is a private global and no pointer syntax has to be invented | A typed pointer would force a pointer *type* into the surface before the language has decided its pointer syntax |
| 21 | At which type does a compound assignment compute? | **The common type of the target and the operand** (`promote` then `usualArithmetic`), with the result converted back to the target's type, and that operation type is **published** as part of the typed AST | C 6.5.16.2. Without it `u16 <<= 9` has no width anybody stated, so the lowering's only options are to re-derive the rule or to guess — and a guess here is an out-of-range shift |
| 22 | Does a later stage recover a conversion from the two types it sees? | **No — every conversion is recorded as a pair, keyed on the consumer that applies it** (*What the artifact publishes*) | A shared `promote()` is one implementation of the rule, but the lowering still holds arithmetic-conversion logic, and a context-dependent conversion (an implicit receiver, a `str`-to-`slice` change) is not recoverable from a pair of types at all |
| 23 | May a deferred literal type survive to the next stage? | **No — decided at the seam, then swept down the tree**; the examples test asserts it over every node | A deferred type has no width and therefore no LLVM mapping, so an operand left undecided is a wrong instruction, not a missing one; and the record's two ends have to be types the IR can name |
| 24 | What decides the operands the context reaches through an operation? | **The same context, walked down from each node with a concrete type** — `1 + 2.0` in an `f64` binding is two `f64`s | The alternative (each operator typing its operands independently) makes the operation's type and its operands' disagree, and LLVM rejects `add f64` with an `i32` operand; the *checker* would have folded a value the emitted code never computes |
| 25 | Is a target a name in a private enum or a **triple**? | **The canonical LLVM triple** (`x86_64-unknown-linux-gnu`), with the ABI facts derived from its components by rule | A two-name enum cannot name aarch64 or a 32-bit target, and the string `codegen` needs anyway would then be a second spelling to keep in step. A triple the table does not state is **refused with a sentence**; defaulting it is how a cross build becomes silently wrong |

## Non-goals

- **Arrays, structs, `enum`, function pointers, casts.** The syntax does not
  exist yet; the type model reserves their kinds and nothing pretends to check
  them.
- **The checked layer of the memory model.** `&T` / `&mut T` / `slice<T>`,
  `restrict`, `volatile`, `unaligned`, and the `expose` /
  `with_exposed_provenance` pair are stage two and three of `memory.md`; the
  access record already has the slots they will fill (`AccessKind`,
  `ProvenanceKind`).
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
| Deferred literals are decided everywhere, including through an operation | `coerce_test.cc`: no node of a checked unit is left deferred; `1 + 2.0` in an `f64` binding is two `f64`s; `1 + 2` in a `u8` binding is `add i8` over two `u8`s; `let y: u8 = 300 / 3` is one `sema-literal-out-of-range` at the `300`, and `-128` in an `i8` is accepted |
| The conversion record is complete and consistent | `coerce_test.cc`: one case per consumer kind (initializer, assignment, `return`, argument, `?:` arm, binary and shift operand, promotion, compound assignment); every entry's `from` equals `typeOf(node)` and neither end is deferred or poisoned; an equal pair is not recorded; and **enumeration** over every ordered pair `convertible` permits produces exactly the enumerated set, in one run |
| The operation type of every `op=` is published | `coerce_test.cc`: `u16 <<= 9` has `opType` `i32` while the node's type is `u16`; the target records the `u16 -> i32` pair; `<<= 31` on a `u8` is legal and `<<= 40` is one `sema-shift-count-out-of-range` |
| No cascades | an unknown type in a signature produces one error however many calls sit on top of it; a name resolution already reported adds nothing here; a parse error adds nothing here; `let x: void = 1;` is one diagnostic, not two |
| The boundary with `validate` holds | a `let` with no type and no initializer and a `const` with no value are `ast-missing-type` / `ast-const-without-value`, and `sema` has no code for either |
| The tree is untouched | *by construction*: the checker holds `const ast::LoweredFile&` and the artifact is a separate array, so there is no mutating path to test — and `resolve --ast` output still matches after checking |
| Deep input is a diagnostic | an expression 5 000 levels deep is checked under ASan/UBSan without a crash, and the depth bound is reported as `sema-limit-types` when the parser lets it through |
| Determinism | the same unit dumped twice is byte-identical: no addresses, no hash-order iteration |
| The store is a cache and not a second opinion | the same `(FileId, revision)` returns the same pointer (hits/misses counted); a new revision rechecks even for structurally identical text; two files share one type store and neither evicts the other |
| `main` | `fn i32 main()` passes, `fn f64 main()` and `fn void main()` are `sema-main-signature`, and a unit with no `main` is not this stage's finding |
