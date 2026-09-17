---
sidebar_position: 1
---

# Grammar

The grammar as the parser implements it. It is written in the shape of the
language's own node kinds, so each production corresponds to one node in the tree
you can see with `mincc parse`.

```
file            := item*
item            := [ "static" ] ( extern-fn | fn | type-alias | let-stmt | const-stmt )
extern-fn       := "extern" "fn" type name [ "<" binders ">" ] "(" params ")" ";"
fn              := "fn" type name [ "<" binders ">" ] "(" params ")" block
type-alias      := "type" name [ "<" binders ">" ] "=" type ";"

binders         := binder ( "," binder )*
binder          := name [ ":" name ]

params          := [ param ("," param)* [ "," "..." ] ]
param           := name ":" type

type            := type-ctor* type-word+ [ "<" type-args ">" ]
                 | "(" type "," type ( "," type )* ")"
type-ctor       := "*" | "!" | "[" [ count ] "]"
type-word       := identifier
type-args       := type ( "," type )*
count           := integer-literal | "_"

block           := "{" statement* "}"

statement       := block
                 | let-stmt | const-stmt | type-alias
                 | if-stmt | while-stmt | for-stmt
                 | "break" ";"
                 | "continue" ";"
                 | "return" [ expr ] ";"
                 | expr ";"
                 | ";"

let-stmt        := "let" ( pattern | binding )
const-stmt      := "const" ( pattern | binding )
pattern         := "(" pattern-member ( "," pattern-member )+ ")"
pattern-member  := name | "_"

binding         := name [ ":" type ] [ "=" expr ]

if-stmt         := "if" expr block [ "else" ( block | if-stmt ) ]
while-stmt      := "while" expr block
for-stmt        := "for" [ for-init ] ";" [ expr ] ";" [ expr ] block
for-init        := binding | expr

expr            := assignment
assignment      := conditional [ assign-op assignment ]
conditional     := logical-or [ "?" expr ":" conditional ]
```

Everything below `conditional` is precedence climbing over the table in
[the operator reference](/reference/operators), which is the same statement as this
grammar's expression levels:

```
logical-or      := logical-and ( "||" logical-and )*
logical-and     := bit-or ( "&&" bit-or )*
bit-or          := bit-xor ( "|" bit-xor )*
bit-xor         := bit-and ( "^" bit-and )*
bit-and         := equality ( "&" equality )*
equality        := relational ( ( "==" | "!=" ) relational )*
relational      := shift ( ( "<" | "<=" | ">" | ">=" ) shift )*
shift           := additive ( ( "<<" | ">>" ) additive )*
additive        := multiplicative ( ( "+" | "-" ) multiplicative )*
multiplicative  := unary ( ( "*" | "/" | "%" ) unary )*

unary           := prefix ( "as" type )*
prefix          := ( "++" | "--" | "+" | "-" | "!" | "~" | "*" | "&" ) prefix
                 | "(" type ")" prefix
                 | postfix
postfix         := primary ( "(" args ")"
                          | "::" "<" type-args ">" "(" args ")"
                          | "[" expr "]"
                          | "[" [ expr ] ".." [ expr ] "]"
                          | "." integer-literal
                          | "++" | "--" )*
primary         := literal | name | "(" expr ")"
                 | "(" expr "," expr ( "," expr )* ")"
                 | "[" [ elements ] "]"
                 | "[" ( count | "]" ) type "{" [ elements ] "}"

elements        := element ( "," element )* [ "," ]
                 | element ";" expr
element         := expr

args            := [ expr ("," expr)* ]
```

## The productions that carry a rule

**A binding is an item.** `let` and `const` at the top of a file are the same
production a block-scope binding uses, and `static` is a prefix on either — and
on `fn` too, since linkage is one question and not four. Two shapes are *read*
and then refused rather than misread: `static` in front of anything that is not
a declaration gets the position rule, and `static extern fn` gets
`parse-conflicting-linkage` — the two words say opposite things, and a parser
that quietly picked one would be inventing a linkage.

**`fn` vs `extern fn`.** The two forms differ by exactly the presence of `extern`
and the presence of a body, and getting one wrong is reported rather than
accepted: `extern fn f();` with a body, and `fn f();` without one, are both
diagnosed. `extern` in front of a binding is refused by name
(`parse-extern-binding`): a file-scope binding is *defined* in this unit, and
`extern fn` is the declaration form that exists.

**`params`.** A parameter is `name ":" type` — the same shape as a binding — so
`fn i32 f(i32 a)` cannot be read two ways. `...` may appear only after at least
one parameter and only as the last thing in the list, and only on an `extern`
declaration.

**`type`.** A type is a run of identifiers, optionally preceded by its
constructors (`*`, `!`, `[N]`, `[]`), optionally followed by a list of type
arguments (`Vec<i32>`), or a parenthesised **product** of two or more types
(`(i32, bool)`). There is no separate type grammar because `unsigned long long
int` is three words and one type: the *type reader*, in `sema`, decides which runs
of words are a type, and that is why `i32`, `long`, and `unsigned long long int`
need no production of their own. The **same** reader answers for a binding's
annotation, a parameter, a cast's target an array's element and a typed
initializer's count, so `[2][3]unsigned long long int` cannot mean one thing in
one place and something else in another.

**`type` also declares.** The same word starts `type Name = T;` — a *name for a
type* rather than a type — and the production is an item and a statement alike,
so an alias is visible at file scope anywhere and in a block from its line down.
It is the one declaration whose left side the grammar has to tell from a use: a
`type` at the start of a statement is a declaration, and `Name` anywhere else is a
type word or a value, never a declaration.

**The binder list is after the name being declared.** `fn T identity<T>(v: T)`
and `type Pair<T, K> = (T, K);`: a `<` between the name and the `(` or the `=`. A
binder is a name and an optional **class** (`T: Number`), which the parser reads
as a `Name` and does not judge — whether the word is one of the classes is
`sema`'s question, because the grammar has no table of names. At a **use** the
list is written where a type is (`Pair<i32, bool>`) or, in an expression, after
`::` (`twice::<i32>(3)`): the two-character token is what tells the argument list
of a call from a comparison.

**A product is a type, a value and a pattern.** `(T, U)` in a type position is a
type; `(1, 2)` in an expression is an initializer for one; and `let (q, r) = …` is
that value taken apart into bindings, with `_` for a member nobody wants. A
member is read at compile time with `.` and an integer (`t.0`), which is why a
chain of two reads is written `t.0 .1`: `0.1` is one number to the scanner, and a
scanner that guessed would be a scanner with a rule about member access in it.

**`as` is one level, and it is the whole of the cast operator's grammar.**
`unary` is `prefix` followed by any number of `as type` — looser than every
prefix operator, tighter than every binary one, and left-associative:

```
a as i64 * 2      is  (a as i64) * 2
-a as i64         is  (-a) as i64
a as i32 as i64   is  (a as i32) as i64
```

What follows `as` is a `type` and nothing more, which is what makes `a as i32 * 2`
a multiplication rather than a type named `i32 *`.

**Right here is the one place a type has to be told from an expression.** A `<`
after a word starts an argument list (`Vec<i32>`) and is the comparison operator
(`x as i32 < 3`), and four lexical facts settle which — a list hangs off a *word*;
a **reserved** type name takes no arguments; the contents have to be a list of
types; and what follows the closer has to be able to follow a complete cast.

```
x as Pair<T, K>      a use, read as a type
x as (i32, i32)      a product, read as a type
x as i32 < 3         a comparison (a primitive takes no arguments)
x as Foo < 3 > 2     a comparison (a number is not a type)
x as Foo < y >> 2    a comparison, and `>>` is the shift it binds tighter as
```

A **chain** — `x as Foo < y > 2` — is refused by name
(`sema-comparison-chain`): of the two readings of those characters the comparison
one is a chain, and a chain is illegal whatever the operands are.

**`(T)x` and `(x) + 1` differ by two lexical questions.** A `(` opens a cast
prefix only when the run inside is a **complete type** *and* the token after `)`
**starts an expression**. Otherwise it is the parenthesised expression it always
was. That is decidable without a symbol table only because **type names are
reserved**: `let i32 = 5;` is refused where it is declared, so `(i32) + 1` cannot
be a reference to a variable named `i32`. C needs a typedef table in its parser
and inherits the "most vexing parse" for it; this grammar has neither.

**`[` opens two things, and the count tells them apart.** `[1, 2, 3]` is an
array literal — a *value*, typed by its context — while `[3]i32{1, 2, 3}` is a
typed initializer, which carries its type. The reader decides with a scan to the
`{`: a `[...]` group followed by a type run and then `{` is the initializer. No
other production puts `{` after type tokens, so the two readings can never both
be valid. An empty group is read and left empty for the reader to refuse:
the parser answers *what shape is written*, and "a zero-element array has no
spelling" is a rule with a sentence. `[;]` — the fill — is
`element ";" count`, and the `;` is the only thing that tells a fill from a
one-element list.

**`a[i]` and `a[l..r]` are one bracket and two nodes.** The `..` decides which:
without it the operand is an index, with it the operands are bounds and either
may be absent (`a[..]`, `a[l..]`, `a[..r]` — the absence is *written*, because
`a[l]` and `a[l..]` are different programs). Nothing is left half-consumed, so
one `]` closes whichever of the two was opened.

**`if`/`while`/`for` conditions.** Written as `expr`, never as `"(" expr ")"` —
and a leading `(` is just a parenthesised expression, so `if (a) { }` parses as
`if ((a)) { }` with the extra parentheses doing nothing. Both spellings are the
same tree modulo the `ParenExpr`, and the language keeps only the one that is
always unambiguous: a condition is an expression, and a `{` can never continue
one, so the parser always knows where the condition ends.

**`for-init`.** Any clause may be empty. The init may be a `let` binding, whose
scope is the loop.

**Statement bodies.** `if`, `while` and `for` take a `block` and not a
`statement`, so there is no single-statement body and no dangling-`else`
question.

**`;`** between the top-level items is not required: `fn i32 a() { } fn i32 b() { }`
on one line parses. It is not accepted either — a stray `;` at file scope is
"expected a declaration".

## Tokens

Full list, with the trivia the lexer keeps and the parser drops:

| Class | Tokens |
| --- | --- |
| keywords | `as` `break` `const` `continue` `else` `extern` `fn` `for` `if` `let` `return` `static` `type` `while` |
| literals | integer, float, character, string — a literal is one token, suffix and digit separators included (`10u8`, `1.5f32`, `1_000`, `0xFE'DC`, `1.5_f32`) |
| punctuation | `( ) { } [ ] ; , : ? . :: ...` |
| operators | `+ - * / % ! ~ & \| ^ < > =` and their compound forms: `++ -- += -= *= /= %= &= \|= ^= <<= >>= == != <= >= && \|\| << >> ->` |
| trivia | whitespace, newline, line comment, block comment |
| other | identifier, `#` (the preprocessor's), end of file, invalid |

`->`, `...` and `#` are recognized as tokens even though the language does not use
`->` yet: they are what a C reader expects to see, and reserving them costs one
line each.

The five-token table above and this grammar are the *decided* syntax. Kinds for
syntax that is not fixed yet — macros, token trees, attributes — are reserved in
the tree but have no production.

## Errors and recovery

The parser does not stop at the first mistake. It records the error, **wraps the
tokens it could not understand in an `Error` node**, and continues, so a
malformed file still produces a tree that covers every byte and every later stage
still has something to work on.

Two consequences worth knowing:

- **One mistake, one diagnostic.** A declaration inside a region the parser
  reported is not reported again by validation or resolution: the tree remembers,
  so the later stages stay quiet.
- **`parse-expected-*` codes name what was expected**, at the token where the
  parser noticed — `parse-expected-token`, `parse-expected-item`,
  `parse-expected-name`, `parse-expected-type`, `parse-expected-expression`,
  `parse-expected-statement`.

A file whose recovery would exceed the node budget reports `parse-aborted` and
stops, rather than producing a tree nobody can use.
