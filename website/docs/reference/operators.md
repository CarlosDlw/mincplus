---
sidebar_position: 2
---

# Operator reference

Precedence is C's. Larger binds tighter; every binary operator is
left-associative except assignment, which is right-associative.

| Precedence | Operators | Associativity | Operands | Result |
| --- | --- | --- | --- | --- |
| **1** (tightest) | `f(x)` `p[i]` `x++` `x--` | left | — | — |
| **2** | `++x` `--x` `+x` `-x` `!x` `~x` `*p` `&x` `(T)x` `x as T` | see the note | — | the type named, for the cast forms |
| **3** | `?:` | right | `bool ? T : T` | `T`, unified from both arms |
| **4** | `=` `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` | **right** | place, value | the place's type |
| **5** | `\|\|` | left | `bool`, `bool` | `bool` |
| **6** | `&&` | left | `bool`, `bool` | `bool` |
| **7** | `\|` | left | integer | integer |
| **8** | `^` | left | integer | integer |
| **9** | `&` | left | integer | integer |
| **10** | `==` `!=` | left | arithmetic, or two `bool`/two `str` | `bool` |
| **11** | `<` `<=` `>` `>=` | left | arithmetic | `bool` |
| **12** | `<<` `>>` | left | integer | the left operand's promoted type |
| **13** | `+` `-` | left | arithmetic, or a pointer and an integer | the wider type, or the pointer |
| **14** (loosest) | `*` `/` `%` | left | arithmetic | the wider type |

The table above is the same order the parser climbs — one precedence table in
`src/parse/token_class.h`, not a call stack, because precedence expressed as the
order of function calls is a grammar whose meaning moves when someone reorders
two calls.

## Notes that are not in the table

**`+` and `-` do double duty.** With two arithmetic operands they are arithmetic;
with a pointer and an integer they are pointer stepping, and the offset is
converted to the pointer's index width. With two pointers, `-` is defined and
gives an `isize` — the element count between them, not a byte count.

**`? :` is not a `bool`.** The condition is a `bool`; both arms unify to one type,
and an arm of type `!` does not take part in that unification.

**The shift's result is the left operand's promoted type**, and the count is *not*
promoted to it: `x << 1` is an `i32` shift even when `x` is `i64`. The count must
be less than the width of the value moved.

**`&` and `*` are two different operators depending on position.** In prefix
position they are address-of and dereference; in binary position, bitwise-and and
multiplication.

**Assignment is an expression** whose value is the value stored, so `a = b = 0`
works and `(x += 1)` is a value. Its left side must be a modifiable place.

**There is no comma operator.** Argument lists and declarators are separated by
commas, and a comma is not an operator in an expression. (This matters for the
argument-is-the-offset rule in a call: a comma separates.)

**There are two cast forms, and both sit in the unary row.** `(T)value` and
`value as T` are in precedence 2 because that is the level they are parsed at —
but within that level the postfix and prefix forms bind tighter: `f(a) as i64` is
`(f(a)) as i64`, `-a as i64` is `(-a) as i64`, and chains are left to right
(`a as u8 as i64`). A cast is not a conversion: converting is what
[`Types`](/language/types#conversions) describes, and it is what the language
does on its own. The two spellings, the suffixes, what each pair means and what
each refusal says are on [the expressions page](/language/expressions#casts).
