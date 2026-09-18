---
sidebar_position: 2
---

# Diagnostics

Every stage of the compiler reports the same way, because there is one renderer
and one format.

## The format

```
file:line:column: severity[code]: message
    the source line
    ^^^^^
```

```console
$ mincc check bad.mx
bad.mx:3:3: error[parse-expected-token]: expected ';'
    return x;
    ^^^^^^
```

- **`file:line:column`** is where the mistake was **written** — so a mistake
  inside a macro points at the `#define`, and a mistake inside an included file
  points at that file. Lines and columns are 1-based, and a column counts bytes
  from the start of the line (tabs are expanded for the caret, so the underline
  lines up whatever the indentation is).
- **`severity`** is `error` or `warning`. An error means no output is produced.
- **`[code]`** is the stable name of the rule, always `stage-thing`: the stage
  that decided, and what it decided. A code never changes meaning and is never
  reused, which is what makes it safe to grep and to suppress by.
- **`message`** is a sentence about *this* mistake. It names the declaration, the
  type or the value involved, and where a fix has a shape it gives the shape.

The caret line underlines the token the diagnostic is *about*. For a
zero-width position — a missing `;` at the end of a line — the underline covers
the token where the parser noticed.

## Where a diagnostic comes from

The code's prefix is the stage:

| Prefix | Stage | Example |
| --- | --- | --- |
| `lex-` | the lexer | `lex-unterminated-string` |
| `pp-` | the preprocessor | `pp-macro-redefined` |
| `parse-` | the parser | `parse-expected-token` |
| `ast-` | lowering and structural validation | `ast-missing-type` |
| `resolve-` | name resolution | `resolve-unknown-name` |
| `sema-` | type checking | `sema-invalid-assignment` |
| `ir-` | LLVM lowering | `ir-unsupported-type` |

A stage reports what it sees and lets the stages after it stay quiet about the
same mistake — one mistake, one diagnostic. A declaration inside a region the
parser could not understand is not reported again by the resolver, and a name
that did not resolve does not produce a second complaint about its type.

## Order and limits

Diagnostics are produced in source order, and all of them are reported rather
than stopping at the first:

```console
$ mincc check bad.mx
bad.mx:2:3: error[...]: ...
bad.mx:5:11: error[...]: ...
bad.mx:9:1: error[...]: ...
```

`-ferror-limit=N` caps how many errors are shown, and the renderer says when it
stopped.

## Warnings

A warning is a diagnostic the language decided not to refuse, and each one is
asked for by a flag:

| Flag | What it reports |
| --- | --- |
| `-Wunused` | a binding or declaration that nothing reads (a name starting with `_` is exempt) |
| `-Wshadow` | a declaration that shadows an outer one; the message names both sites |
| `-Wconversion` | an implicit conversion that may lose information |
| `-Wcast` | a cast the program wrote that may lose information (`sema-cast-loses`) |
| `-Wprovenance` | a cast between a pointer and an integer — the model's `expose` and `with_exposed_provenance` |

Some warnings are on by default because they are about a mistake and not about a
style: `sema-unreachable-code`, for example, reports a statement the checker can
prove cannot run.

There is no `-Werror` yet: a warning is a diagnostic the language decided not to
refuse, and turning every one into a refusal is a decision the flag would have to
make per warning rather than in bulk. What CI does instead is build with `-Werror`
for the *compiler's own* C++, which is a build flag and not this one.

## Color and layout

Color is detected from the terminal and can be overridden with `--color`
(`auto`, `always`, `never`), so a diagnostic in a log file is plain text and a
diagnostic on a terminal is not. Tab width and the error limit are options for
the same reason: the renderer's job is to be readable wherever it lands.

## The full list

Every code each stage can print, with the `-` removed for reading. The table the
renderer reads is the same table this list is taken from, so a code that exists
here and nowhere else is a code that cannot be printed.

**`lex-`** — `invalid-character`, `unterminated-string`, `unterminated-char`,
`unterminated-comment`, `unknown-escape`, `named-escape`, `escape-out-of-range`,
`escape-digits`, `escape-too-wide`, `empty-char`, `missing-digits`,
`misplaced-separator`.

**`pp-`** — `invalid-directive`, `unknown-pragma`, `not-supported`,
`error-directive`, `warning-directive`, `invalid-line`, `unterminated-conditional`,
`unexpected-conditional`, `else-after-else`, `conditional-nesting`,
`macro-redefined`, `macro-parameter-limit`, `missing-macro-arguments`,
`too-many-macro-arguments`, `unterminated-macro-arguments`, `invalid-paste`,
`invalid-hash-operand`, `missing-macro-name`, `reserved-identifier`,
`expansion-depth`, `expansion-budget`, `expression-syntax`, `undefined-identifier`,
`stray-hash`, `invalid-pragma-operand`, `include-not-found`,
`include-unreadable`, `include-self-reference`, `include-depth`,
`include-budget`, `missing-include-guard`, `output-budget`, `token-too-long`,
`date-without-epoch`.

**`parse-`** — `expected-token`, `expected-item`, `expected-name`,
`expected-type`, `expected-expression`, `expected-statement`,
`expected-array-count`, `expected-array-count-close`, `expected-type-arg-close`,
`expected-type-group-close`, `stray-type-arg-close`, `leading-point-number`,
`invalid-literal-suffix`, `brace-without-type`, `cast-to-product`,
`missing-extern`, `extern-with-body`, `extern-binding`, `static-position`,
`conflicting-linkage`, `type-alias-linkage`, `variadic-definition`,
`variadic-position`, `aborted`.

**`ast-`** — `missing-type`, `const-without-value`, `pattern-at-file-scope`,
`node-limit`.

**`resolve-`** — `unknown-name`, `redeclaration`, `reserved-identifier`,
`unused-entity`, `shadowed-name`, `limit-defs`, `limit-scopes`, `limit-refs`.

**`sema-`** — `unknown-type`, `malformed-type`, `type-not-value`, `unknown-member`,
`literal-out-of-range`, `literal-type-unknown`, `condition-not-bool`,
`invalid-operands`, `comparison-chain`, `invalid-assignment`, `assign-to-const`,
`incdec-not-lvalue`, `initializer-shape`, `index-out-of-range`,
`index-not-integer`, `slice-bounds-reversed`, `slice-not-viewable`,
`slice-pointer-needs-both-bounds`, `variadic-aggregate`, `extern-aggregate`,
`never-returns`, `never-body-completes`, `not-a-function`, `argument-count`,
`return-mismatch`, `return-missing-value`, `return-void-value`, `missing-return`,
`main-signature`, `function-redefinition`, `signature-mismatch`,
`break-outside-loop`, `continue-outside-loop`, `division-by-zero`,
`constant-out-of-range`, `shift-count-out-of-range`, `use-before-assignment`,
`deref-not-pointer`, `pointer-void-access`, `pointer-void-arithmetic`,
`address-of-non-lvalue`, `address-of-const`, `pointer-mismatch`,
`pointer-integer`, `address-from-constant`, `provenance-cast`,
`cast-invalid`, `cast-loses`, `implicit-conversion`, `builtin-width`,
`builtin-not-a-value`, `type-alias-cycle`, `type-constant-unknown`,
`type-constant-not-granted`, `destructuring-arity`,
`destructuring-not-product`, `global-cycle`, `global-not-constant`,
`constraint-not-a-class`, `constraint-unsatisfied`, `generic-extern`,
`generic-main`, `generic-name-not-value`, `generic-not-inferable`,
`generic-operation`, `generic-type-args`, `generic-instance-limit`,
`limit-types`, `unreachable-code`.

**`ir-`** — `unsupported-type`, `unsupported-node`, `unsupported-target`,
`missing-obligation`, `assumption`, `alignment`, `unguarded-op`, `internal`,
`object-too-large`, `initializer-too-large`, `cast-out-of-range`.

**`memory-`** — the checked build's traps, printed by a running program rather
than by the compiler: `memory-null`, `memory-misaligned`,
`memory-out-of-bounds`. See
[the memory model](/language/memory-model#what-a-checked-build-reports).

Every stage also has a `-unknown` code, the fallback for a value that reached the
renderer from outside its own table, so an unrecognized code prints as *a code*
rather than as a blank.
