---
sidebar_position: 1
---

# Source files

A `.mx` file is one translation unit. Text is read into a unit either by
compiling it directly or by `#include`, and the unit's own text is what later
stages point at.

## Encoding

A source file is **UTF-8**, and the decoder is strict: an ill-formed byte
sequence is reported rather than replaced with a substitute character. A UTF-8
byte-order mark at the start of a file is accepted and skipped, so a file saved
by an editor that adds one compiles.

Non-ASCII text is allowed in comments and in character and string literals. In
code, an identifier is ASCII:

```minc
// Comment in any language: 日本語もポルトガル語も書ける
fn i32 main()
{
  let text: str = "olá, mundo";   // fine — this is string content
  return 0;
}
```

## Line endings

LF, CRLF and a lone CR all end a line, and each counts as exactly one line
break, so a file saved on Windows and the same file saved on Unix produce the
same line and column numbers and the same diagnostics.

The repository's own sources are stored with LF (see `.gitattributes`) so a
checkout cannot introduce CRLF-only differences.

## Comments

Two forms, and they do not nest:

```minc
// A line comment runs to the end of the line.

/* A block comment runs to its terminator. It can hold anything, including
   "double quotes", 'single quotes', and code that looks like real code:
   let notCode: i32 = 0x1F; */
```

A `/* … */` comment does not nest, so `/* /* */` ends at the first `*/`. An
unterminated block comment is reported with a caret at the opening `/*`, and the
lexer keeps going from there rather than stopping.

Comments are **trivia**: the lexer produces them as tokens, the parser drops
them from the syntax tree, and the tree remembers the bytes they occupied so it
stays lossless. They never affect a diagnostic's position.

## Tokens

Every byte of a file belongs to exactly one token — whitespace and comments
included. That is what makes the lexer's output lossless and what lets a
formatter be written without a second scanner.

## Identifiers

An identifier starts with an ASCII letter or an underscore and continues with
letters, digits and underscores. There is no Unicode identifier: a non-ASCII
character outside a comment or a literal is reported as not part of the
language, with the caret under the whole character.

By convention, names beginning with `_` are the implementation's; a binding
whose name begins with `_` is never reported as unused.

```minc
let value = 1;
let _temporary = 2;
let snake_case_name = 3;
```

## Keywords

The set is fourteen words, and small on purpose. These cannot be used as names:

`as` · `break` · `const` · `continue` · `else` · `extern` · `fn` · `for` · `if` ·
`let` · `return` · `static` · `type` · `while`

A word earns a place in it by changing what the tokens after it mean: `as`
introduces a type where an expression could have continued, `static` says what a
declaration's linkage is, `type` starts a declaration that binds a *name for a
type*, and `let` is not a name that could stand where a statement starts.

Everything else a reader might expect to be a keyword is not one:

- **Primitive type names are not keywords — and they are still reserved.**
  `i32`, `u8`, `bool`, `str`, `int`, `long` and the rest are ordinary names that
  the type reader understands, so the lexer has no table for them. What the
  language does not allow is a *declaration* of one, and the sentence says why:

  ```console
  $ printf 'fn i32 main() { let int = 2; return 0; }\n' | mincc check -
  <stdin>:1:21: error[resolve-reserved-identifier]: 'int' names a type, and a type name is reserved: it is what makes `(T)x` a cast rather than a call -- choose another name
    fn i32 main() { let int = 2; return 0; }
                        ^^^
  ```

  That reservation is not tidiness — it is what makes the C-style cast
  unambiguous, because `(i32) + 1` can then never be a reference to a variable.
  See [Casts](/language/expressions#casts).
- **`true`, `false` and `null` are not keywords either.** They are the names the
  language binds before any source is read, so they behave like any other name:
  `let true = 0;` shadows one, and `-Wshadow` says so. That is why `#if`
  conditionals and expressions can mention them without the preprocessor having
  to know anything about the language's vocabulary.
- **`sizeof`, `alignof` and friends are not keywords yet.** When they land they
  will be *grammar* rather than names, because their operand is not a value:
  `sizeof(x++)` cannot increment `x`.

## The unit, and what a position means

A unit is one file plus everything it included. A token therefore has two ranges:

- where it was **written** — the file, the macro argument, the header the bytes
  came from; and
- where it sits in the **unit's text**, which is one buffer however many files
  went into it.

A diagnostic points at the written range, so a mistake inside a macro points at
the `#define` that wrote it and not at the line that used it. The unit range is
what identifies a node, because the written range is not unique: a macro that
expands one argument into two names gives both declarations the same written
location.

## Standard input

`-` names standard input, and it is read in binary:

```console
$ printf 'fn i32 main() { return 0; }\n' | mincc check -
```
