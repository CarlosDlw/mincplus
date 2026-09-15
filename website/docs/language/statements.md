---
sidebar_position: 5
---

# Statements

A function body is a block, and a block is a sequence of statements.

## Blocks and empty statements

```minc
{
  let a: i32 = 1;

  {
    let b: i32 = 2;   // a nested block is a scope
  }

  ;                   // an empty statement: legal, and does nothing
}
```

## `return`

```minc
fn i32 value()
{
  return 42;          // a function returning a type must return a value
}

fn void nothing()
{
  return;             // a `void` function may leave early with a bare `return`
}                     // … or fall off the end
```

A function that returns a type must not be able to reach the end of its body, and
the compiler checks that exactly rather than conservatively: a body ending in
`while true` with no `break` that could leave it is fine, an `if` whose both arms
return is fine, and the code after a call to a function returning `!` is
unreachable and does not count.

```console
$ printf 'fn i32 f(c: bool) { if c { return 1; } }\n' | mincc check -
<stdin>:1:8: error[sema-missing-return]: `f` returns `i32`, so it cannot reach the end without returning a value
  fn i32 f(c: bool) { if c { return 1; } }
         ^
```

## `if` / `else`

```minc
if value < 0
{
  return 0 - 1;
}
else if value == 0
{
  return 0;
}
else
{
  return 1;
}
```

- The condition is a `bool`. An arithmetic value is not one, and the refusal
  suggests the comparison to write.
- **Parentheses around the condition are optional**, and always were: a condition
  is an expression and a `{` can never continue one, so the parser knows where it
  ends. `if (value < 0)` is the same statement.
- The arms are blocks. There is no single-statement body, so there is no
  dangling-`else` rule to remember.
- `else if` is not special syntax: it is an `else` whose one statement is another
  `if`.

## `while`

```minc
while i <= n
{
  result *= i;
  i += 1;
}
```

The condition is evaluated before each iteration, and again it needs no
parentheses. `while true { … }` is the language's infinite loop, and it is what
makes an assignment count as reaching every path (see
[definite assignment](/language/variables#without-an-initializer)).

## `for`

```minc
for let i: i32 = 0; i < limit; i += 1
{
  total += i;
}
```

Three clauses, in C's order, any of which may be empty:

- **init** is a statement: a `let` binding (whose scope is the loop and its
  clauses), an expression, or nothing at all.
- **condition** is a `bool` expression; empty means "always true".
- **step** is an expression, evaluated after each iteration.

```minc
let i: i32 = 0;
for ; i < n; i += 1     // no initializer: the binding lives outside
{
  total += i;
}

for ; ;                 // an infinite loop, and it needs a `break` to end
{
  break;
}
```

## `break` and `continue`

```minc
for let i: i32 = 0; i < limit; i += 1
{
  if i % 3 == 0
  {
    continue;          // skip to the step
  }

  if i > 100
  {
    break;             // leave the loop
  }
}
```

They apply to the innermost enclosing loop, and are an error outside one.

:::note[Not implemented yet]
`goto` and labels, `do`/`while`, `switch`, and labels on `break`/`continue` are
not implemented. Neither is `defer`.
:::

## Declarations are statements

A `let` or `const` may appear anywhere a statement may, so a binding's scope is
the block it appears in:

```minc
fn i32 compute()
{
  let a: i32 = 1;

  if a > 0
  {
    let b: i32 = a + 1;   // `b` exists only in this block
    return b;
  }

  return a;
}
```

:::warning[One name, one declaration per scope]
Two `let`s of the same name in one scope is a redeclaration and an error. The
same name in a *nested* scope shadows the outer one, which `-Wshadow` reports.
:::
