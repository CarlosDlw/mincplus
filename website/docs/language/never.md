---
sidebar_position: 8
---

# The bottom type, `!`

`!` is the type of an expression that **never produces a value**. A function
declared `fn ! name(...)` never returns to its caller, and every call to it is an
expression of type `!`.

```minc
extern fn i32 puts(s: str);
extern fn ! exit(code: i32);

fn ! die(msg: str)
{
  puts(msg);
  exit(1);
}

fn i32 pick(c: bool)
{
  let chosen: i32 = c ? 1 : die("the impossible branch\n");
  return chosen;
}
```

Two rules, and everything else follows from them.

## Rule 1 — `!` converts into every type, and nothing converts into `!`

A value of type `!` is never produced, so "it becomes a `T`" is not false — it is
*vacuous*. That is what makes an arm of a `?:` work:

```minc
let chosen: i32 = c ? 1 : die("unreachable");   // the conditional is an `i32`
```

and an initializer, and an argument, and a `return`:

```minc
let x: i32 = exit(1);        // legal: the initializer never produces a value
return die("bad");           // legal wherever a value is expected
```

C could not express this with an attribute: `_Noreturn` is a fact *beside* the
signature, so `c ? 1 : die()` has a `void` arm, and `void` is not a value. A type
carries the fact with the value, so the type system answers the question with the
rule it already had.

Nothing converts *into* `!`: a type that admits no values cannot be arrived at.

## Rule 2 — `!` is written only as a return type

`let x: !`, `p: *!`, `f(x: !)` are all refused, because no object can have that
type. The refusal names the word the reader wrote, so `void` and `!` are never
confused for one another:

```console
$ printf 'extern fn ! exit(c: i32);\nfn i32 h() { let x: ! = exit(1); return 0; }\n' | mincc check -
<stdin>:2:21: error[sema-type-not-value]: `!` is not a type an object can have
  fn i32 h() { let x: ! = exit(1); return 0; }
                      ^
```

`*!` is refused at the type itself, with the other word in the message:

```console
$ printf 'fn i32 h(p: *!) { return 0; }\n' | mincc check -
<stdin>:1:13: error[sema-malformed-type]: `!` is a type on its own: it cannot be combined with a type name or a `*`
  fn i32 h(p: *!) { return 0; }
              ^^
```

## The body is proved

`fn ! f()` is a claim about the *implementation* — it says control never comes
back — so the compiler proves it rather than taking it on trust. Two ways to break
the promise, and each has its own message:

- **A `return` that can run** → `sema-never-returns`.
- **A body that can reach its end** → `sema-never-body-completes`.

```console
$ printf 'fn ! f() { return; }\n' | mincc check -
<stdin>:1:15: error[sema-never-returns]: `f` returns `!`, so control never comes back to its caller, and a `return` is control coming back
```

*Reachable* is the load-bearing word: `while true {} return;` passes, because the
`return` is code no caller ever sees, and `return die();` does not count as a
`return` that completes. The second of the two messages:

```console
$ printf 'fn ! g() { }\n' | mincc check -
<stdin>:1:6: error[sema-never-body-completes]: `g` returns `!`, so its body cannot reach its end -- it has to loop forever, or call another function that never returns
  fn ! g() { }
       ^
```

A promise is never **inferred**. A function whose body happens to loop forever is
still `void`, so "this never returns" is always something the source says.

## What it means for everything else

Because `!` is a type, the flow rule is not a second table:

- **A call to a function returning `!` never completes**, so the statement after
  it is unreachable, and a function whose body ends in one does not need a
  `return`.
- **`terminates` is the ordinary question** — "can control reach the end of this
  statement" — and a `!`-typed expression is one more way the answer is no.
- **Unreachable code is reported**: a statement the checker can prove cannot be
  reached is `sema-unreachable-code`, a warning, naming the statement that never
  completes before it.

```console
$ mincc check one.mx
one.mx:2:34: warning[sema-unreachable-code]: this statement can never be reached, because the statement before it never completes
```

And in the emitted code nothing changes: `!` lowers to `void` on the ABI side, and
the promise that the call never returns is handed to LLVM as the `noreturn`
attribute — *derived* from the return type rather than declared beside it.

:::note[A second spelling is not planned]
There is no `never` keyword and there will not be one. `!` is already a token, so
it costs no word from the identifier namespace, and `fn never f()` would be
ambiguous with a user-defined type name the moment the language has type aliases.
:::
