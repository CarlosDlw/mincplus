# External declarations — `extern fn`

This record decides how a `minc+` program names something that is *defined
elsewhere*: in a library, in another translation unit, in the C runtime. The
form is

```minc+
extern fn i32 puts(s: str);
```

and the whole design is one sentence: **`extern` is the declaration form of a
function, `fn` is its definition form, and the two are one function.** Everything
below is the consequence of taking that seriously.

What this record does *not* own is the ABI: the layout of an aggregate that
crosses the boundary, the calling convention, the C type mapping, and the
translation of a C header. Those are `src/cinterop`'s facts (roadmap §8) and they
arrive with their own record. The line between the two is drawn in *§ What a
declaration is not*, and it is the reason this record exists separately.

## The two questions, and why they are one form

A language with a linker has two things to say about a function:

* **what its callers may assume** — its name, its parameters, its return type;
* **what actually runs** — its body.

In C these are the same production (`declaration`), told apart by whether a body
follows, and `extern` is a *storage class* that on a function means almost
nothing: a file-scope C function has external linkage whether or not the word is
written. That is why `extern int f(void);` and `int f(void);` are the same
declaration in C, and why a reader has to know the standard to know that
`extern int g;` (a declaration) and `int g;` (a tentative definition) are *not*
the same thing.

`minc+` has one visibility rule (file-scope is external; `static` will be the
word that says otherwise) and no tentative definitions, so C's `extern` would be
a word that means nothing on a function and something subtle on a variable. The
decision here is to make it mean **exactly one thing**: *the definition is
elsewhere, so this has no body.*

## What the references do

| Language | The form | What it gets right | What we do differently |
| --- | --- | --- | --- |
| C (and C++) | `extern` storage class; `int f(void);` is already a declaration | one production, a header can contain it, a declaration and a definition are one entity with one type | `extern` is a no-op on a function, and its variable meaning (declaration vs tentative definition) is a footgun we do not need |
| Zig | `extern fn ...;` | the declarator *is* the declaration, the default calling convention is the C one, and `export` is the separate word for the other direction | taken as the shape; we have no `callconv` yet and will not add one until there is an ABI that needs it |
| Rust | `extern "C" { fn ...; }` in an external block, `#[link_name]` to rename | one block declares a whole C interface; the ABI is stated rather than assumed | a block is a second grammar construct and a `link_name` attribute needs attributes; and since `minc+` mangles nothing, the declared name *is* the symbol name and no rename is owed yet |
| Go | `//go:linkname`-free: `func f()` in assembly or cgo | cgo generates the declaration from the C header | generation from a header is `src/cinterop`'s job, and it will *produce* this form rather than replace it |
| D, Swift | `extern(C)` / `@_cdecl` | the ABI is named where it matters | same answer: the ABI arrives as a second field once there is a second ABI |

The one thing every reference agrees on, and the property we take: **a
declaration is a promise about a symbol, not a definition.** It has no body, it
needs no body, and the linker is the stage that decides whether the promise was
kept. Zig's `extern fn` and Rust's `extern "C" { fn }` say precisely that; C
blurs it with a storage class. This language states it.

## The form

Two productions, and nothing else changed:

```
FnDecl      ::= 'fn' Type Name '(' ParamList ')' Block
ExternDecl  ::= 'extern' 'fn' Type Name '(' ParamList ')' ';'
```

* **`extern` is a keyword** (`KwExtern`), not an identifier. It is lexically
  classified for the same reason `if` is: the parser is trivia-blind and must
  decide which of the two forms it is reading from the first token.
* **Both forms produce `SyntaxKind::FnDecl`** — *one* node kind, not two. The
  shapes are identical except for the body, and a consumer that had to try two
  casts to ask "what functions does this file declare" would be worse at every
  call site. The `extern` token stays a child of the node, so the tree is still
  lossless and the word the reader wrote is still findable (a future rename or
  hover needs it).
* **`bodyOf(decl)` absent *is* "this is a declaration"**, and
  `syntax::FnDecl::isExtern()` is the readable spelling of it. The two are
  guaranteed to agree by the grammar, which is what lets every stage below ask
  one question instead of two.
* **Parameter syntax is unchanged** (`name: type`), which follows from the
  language's one rule for "a name with a type" rather than from anything
  `extern` decides.

### The two wrong forms are reported where they are decided

The grammar has both directions of the mistake, and the parser reports both
because both are *spelling*: the parser is the stage that holds the word and the
token.

| What was written | Code | Message |
| --- | --- | --- |
| `fn i32 f();` | `parse-missing-extern` | a function with no body is a declaration; write `` `extern fn`  `` |
| `extern fn i32 f() { ... }` | `parse-extern-with-body` | `` `extern` `` declares a function that is defined elsewhere, so it has no body |

Neither is a hard error for the *tree*: a bodyless `fn` is still built as a
`FnDecl` with no body, and an `extern fn` with a body is still built with one.
That is the project's rule for recoverable mistakes — one mistake, one
diagnostic, and every byte still belongs to a node — and it is why neither
direction produces a cascade from the stages below.

**Why not let `fn i32 f();` mean a forward declaration?** Because it would be
unnecessary: file-scope resolution finishes before any body is checked, so a call
may name a function defined further down (the same reason `sema` has a signatures
pass at all). A body-less `fn` would therefore have exactly one use — declaring a
function defined in a *different* file — which is what `extern` says out loud.
The rule costs nothing and removes a class of "the body is missing" mistakes.

## One function, one identity

This is the part that is not decoration. A name may be *declared more than once*
and must still be one function:

```minc+
extern fn i32 f();      // a declaration
fn i32 f() { return 7; } // the definition
```

`resolve` has always chained repeated function declarations onto a canonical
`DefId` (a header included twice declares its functions twice), but the chain
lived only in the *scope* lookup. A stage indexing **declaration sites** — `sema`
and `ir` both build an offset → def table so a lookup is a hash instead of a
scan — got back the site's own def id. Two sites meant two ids, and two ids meant
two of everything downstream.

Measured, before this change:

```
$ cat f.mx
extern fn i32 f();
fn i32 f() { return 7; }
fn i32 main() { return f(); }
$ mincc run f.mx
/usr/bin/ld: unit0.o: in function `main`:
  undefined reference to `f`
```

and the module said why:

```llvm
declare i32 @f()

define i32 @f.1() {      ; the *definition*, under a name nothing calls
entry:
  ret i32 7
}
```

The lowering created two `llvm::Function`s for one def; LLVM renamed the loser
`f.1`; the call site referred to the one that stayed a declaration. A program
that the checker accepted failed to link, which is the one thing the pipeline
promises not to do.

The fix is a field and two readers, and it is stated as an invariant rather than
as a patch: **`Def::canonical`** is the identity every lookup of the name
answers — the declaration itself for the one that introduced the name, the first
declaration for a repeat — and `resolve::canonicalOf` is the one place that
decides it. `sema`'s `defByNameOffset_` and `ir`'s `defByName_` now store the
identity rather than the site, and `ir::declareFunctions` creates at most one
`llvm::Function` per def. Both exist so that a *third* reader cannot get this
wrong by copying the old code.

`nextRedundant` stays: it is the chain of sites, which is what the IDE wants, and
it points the other way for a non-function redeclaration. Two fields, two
questions, and the comment on each says which is which.

## What a declaration claims, and what checks it

A declaration and a definition of one name have to be **the same function**, so
the two claims must agree. That is a check only `sema` can make — what has to
agree is a *type* — and it is now made for every name declared twice:

| What was written | Code | Note |
| --- | --- | --- |
| two bodies for one name | `sema-function-redefinition` | *the first definition is here* |
| two signatures for one name | `sema-signature-mismatch` | *the earlier declaration is here* |

Both are errors and both point at the second declaration, with the first as a
note: which of two definitions to delete is not a question a message can answer,
and a reader with both lines in front of them can.

The check is *exactly* what makes the identity safe rather than merely true. Once
every declaration of a name has one type, "the type of this name" has one answer
no matter which declaration wrote it last, and the ordering question that would
otherwise exist — `defTypes_` is written once per declaration, so the last one
would win — cannot be observed. That is the shape this project prefers: make the
ambiguity impossible instead of picking a winner and documenting it.

`main` is not special-cased here. A program whose `main` is only declared is a
link-time fact ("undefined reference to `main`"), and the stage that knows which
translation units exist is the linker, not the checker.

## What a declaration is not

* **Not a linkage change.** A file-scope function is external either way, and
  `Def::linkage` says `External` for a declaration exactly as it does for a
  definition. `extern` says *where the definition is*, which is a fact about the
  body; `static` — when it lands — is the word that says *who may see it*. The
  two are orthogonal and the roadmap's "storage classes and linkage" item stays
  open for the second one.
* **Not an ABI statement.** There is one ABI in the language today: the target's
  C calling convention, which is what LLVM emits from the triple. An `abi` field
  or a `callconv` annotation would be a grammar production and a checked
  enumeration for one value; it arrives with the first target that needs a second
  one, and the form has room for it next to `fn`.
* **Not a renamed symbol.** `minc+` mangles nothing, so the declared name is the
  symbol name. `__assert_fail` is declared by writing `__assert_fail`. A `@symbol`
  attribute is the mechanism for the day a name needs two spellings, and it needs
  attributes rather than a second declarator.
* **Not a `src/cinterop` type.** No aggregate crosses this boundary yet; when one
  does, the *type* mapping is cinterop's and this form carries the name and the
  signature, unchanged.
* **Not globals.** `extern let x: i32;` is a decision this record does not make:
  a global has storage, an initializer, and an address that the runtime
  initializes differently (`.data`/`.bss`), and it is one line in the roadmap
  under the same heading.

## Where it lands in the compiler

```
lexer      `extern` is KwExtern, so the form is decided by the first token
parser     two productions; both build one FnDecl node; two diagnostics
syntax     FnDecl::isExtern(), bodyOf() == nullopt
lower      Item.hasBody == false; the item's span ends at the `;`
resolve    the canonical chain: one DefId for every declaration of one name
sema       the signature comes from the declaration; the two agreement checks
ir         `Function::Create` once per def, ExternalLinkage, no body -> `declare`
backend    the linker resolves the symbol; nothing is emitted for a declaration
           nothing references
```

The IR is one line of consequence:

```llvm
declare i32 @puts(ptr)          ; a promise the linker keeps

define i32 @twice(i32 %0) {     ; the pair above: declaration + definition
entry:
  %mul = mul i32 %0, 2
  ret i32 %mul
}
```

An `extern` declaration a program never calls emits **no symbol at all**: LLVM
drops a declaration nothing references when the object is written, so declaring a
library function costs nothing until it is used. That is worth stating because it
is the reason there is no "unused declaration" warning and no bookkeeping here.

## How the claims above are checked

The suite pins each one, in the stage that owns it:

* **grammar** — `parser_test.cc`: the tree of a declaration (one `FnDecl`, no
  body, `isExtern()`), a definition (body, not extern), one diagnostic for each
  wrong form with every byte reconstructed, and that item recovery stops at
  `extern` the way it stops at `fn`.
* **codes** — `error_codes_test.cc`: one input per new `parse-` code, and the
  table/enum agreement test the file already had.
* **names** — `resolve/errors_test.cc`: both declarations of `f` produce one
  `itemDefs` entry and one identity, the identity is reachable from the second
  site, the call counts against the canonical declaration, and a declaration with
  no definition resolves with `Linkage::External`.
* **types** — `sema/check_test.cc`: an `extern fn` is typed and callable, a
  bodyless declaration is not a `missing-return`, a call is checked against the
  *declaration's* signature, the two disagreement codes fire with both spellings
  in the message, and repeating an identical declaration is not an error. Plus
  one input per new `sema-` code in the reachability list.
* **IR** — `ir/lower_test.cc`: declaration + definition is **one** symbol with no
  `f.1` and the call referring to it; a call to a declaration is a `declare` plus
  a `call`; a declaration nothing calls is still a `declare` and never a
  `define`.
* **end to end** — `examples/010_extern.mx` declares two libc functions, defines
  a declared one, prints through `puts` and returns `42`. It runs under
  `make examples`, the sema and IR example suites, and by hand:
  `mincc run examples/010_extern.mx`.

## References

* ISO/IEC 9899:2024 (C23) §6.2.2 (linkage) and §6.9 (external definitions) — the
  storage-class model this form deliberately does not copy.
* Zig language reference, *Functions* — `extern fn`, and the C calling
  convention as the default; `export` as the opposite direction.
* The Rust Reference, *External blocks* — `extern "C" { fn ...; }`, `extern
  "system"` on Win32, and `#[link_name]` replacing a symbol; Rust RFC 8 for why
  an intrinsic with a different ABI could not be used like a function (the same
  reasoning applies to a renamed symbol: the form carries the name it calls).
* LLVM Language Reference, `declare` — a declaration contributes a symbol to the
  module and nothing to the object unless it is referenced.
* `docs/architectures/memory.md` § *The one idea* and § *Stage 3* — where the
  boundary's ABI facts live, and why a reference is not a declaration.
* `docs/architectures/builtins.md` — why `exit`, `abort` and `assert` are this
  form plus a runtime rather than a table of builtins.
