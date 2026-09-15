---
sidebar_position: 2
---

# Operator reference

Precedence is C's. Larger binds tighter; every binary operator is
left-associative except assignment, which is right-associative.

| Precedence | Operators | Associativity | Operands | Result |
| --- | --- | --- | --- | --- |
| **1** (tightest) | `f(x)` `p[i]` `x++` `x--` | left | — | — |
| **2** | `++x` `--x` `+x` `-x` `!x` `~x` `*p` `&x` | right | — | — |
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
converted to the pointer's index width. With two pointers, `-` is not defined yet.

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

**There is no cast.** `(T)value` is a parenthesised expression followed by
nothing; converting is what [`Types`](/language/types#conversions) describes,
and every conversion in the language today is implicit.
