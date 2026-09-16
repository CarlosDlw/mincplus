---
sidebar_position: 1
---

# Grammar

The grammar as the parser implements it. It is written in the shape of the
language's own node kinds, so each production corresponds to one node in the tree
you can see with `mincc parse`.

```
file            := item*item            := [ "static" ] ( extern-fn | fn | let-stmt | const-stmt )
extern-fn       := "extern" "fn" type name "(" params ")" ";"
fn              := "fn" type name "(" params ")" block

params          := [ param ("," param)* [ "," "..." ] ]
param           := name ":" type

type            := type-ctor* type-word+
type-ctor       := "*" | "!" | "[" [ count ] "]"
type-word       := identifier
count           := integer-literal | "_"

block           := "{" statement* "}"

statement       := block
                 | let-stmt | const-stmt
                 | if-stmt | while-stmt | for-stmt
                 | "break" ";"
                 | "continue" ";"
                 | "return" [ expr ] ";"
                 | expr ";"
                 | ";"

let-stmt        := "let" binding
const-stmt      := "const" binding

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
                          | "[" expr "]"
                          | "[" [ expr ] ".." [ expr ] "]"
                          | "++" | "--" )*
primary         := literal | name | "(" expr ")"
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
constructors (`*`, `!`, `[N]`, `[]`). There is no separate type grammar because
`unsigned long long int` is three words and one type: the *type reader*, in
`sema`, decides which runs of words are a type, and that is why `i32`, `long`,
and `unsigned long long int` need no production of their own. The **same** reader
answers for a binding's annotation, a parameter, a cast's target and a typed
initializer's count, so `[2][3]unsigned long long int` cannot mean one thing in
one place and something else in another.

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
| keywords | `as` `break` `const` `continue` `else` `extern` `fn` `for` `if` `let` `return` `static` `while` |
| literals | integer, float, character, string — a literal is one token, suffix and digit separators included (`10u8`, `1.5f32`, `1_000`, `0xFE'DC`, `1.5_f32`) |
| punctuation | `( ) { } [ ] ; , : ? . ...` |
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
