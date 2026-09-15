---
sidebar_position: 1
---

# Grammar

The grammar as the parser implements it. It is written in the shape of the
language's own node kinds, so each production corresponds to one node in the tree
you can see with `mincc parse`.

```
file            := item*

item            := extern-fn | fn

extern-fn       := "extern" "fn" type name "(" params ")" ";"
fn              := "fn" type name "(" params ")" block

params          := [ param ("," param)* [ "," "..." ] ]
param           := name ":" type

type            := type-word+ | "*" type
type-word       := identifier

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

unary           := ( "++" | "--" | "+" | "-" | "!" | "~" | "*" | "&" ) unary
                 | postfix
postfix         := primary ( "(" args ")" | "[" expr "]" | "++" | "--" )*
primary         := literal | name | "(" expr ")"

args            := [ expr ("," expr)* ]
```

## The productions that carry a rule

**`fn` vs `extern fn`.** The two forms differ by exactly the presence of `extern`
and the presence of a body, and getting one wrong is reported rather than
accepted: `extern fn f();` with a body, and `fn f();` without one, are both
diagnosed.

**`params`.** A parameter is `name ":" type` — the same shape as a binding — so
`fn i32 f(i32 a)` cannot be read two ways. `...` may appear only after at least
one parameter and only as the last thing in the list, and only on an `extern`
declaration.

**`type`.** A type is a run of identifiers, optionally preceded by `*`. There is
no separate type grammar because `unsigned long long int` is three words and one
type: the *type reader*, in `sema`, decides which runs of words are a type, and
that is why `i32`, `long`, and `unsigned long long int` need no production of
their own.

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
| keywords | `break` `const` `continue` `else` `extern` `fn` `for` `if` `let` `return` `while` |
| literals | integer, float, character, string |
| punctuation | `( ) { } [ ] ; , : ? ...` |
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
