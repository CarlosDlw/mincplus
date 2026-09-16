# Literals: numbers, strings, and the characters in them

A literal is the one place where the *text* of a program is the *value* of it, so
it is the one place where three stages have to agree about the same bytes:

| stage | what it owns |
| --- | --- |
| `src/lex` (the scanner) | **where the token ends** — `10u8` is one token, `10z` is two, and `"\x{41}"` is one escape and not five bytes of punctuation |
| `support/consteval` (the readers) | **what the number is** — the value, the digits, and the suffix, because the preprocessor's `#if` and the checker's range test must not disagree |
| `sema` (the checker) | **whether it fits the type the context gave it**, and the language's rules about what a literal may be |
| `src/ir` | the **correctly rounded value** of a float (`APFloat`) and the **bytes** of a string, because those are the two things no earlier stage may guess at |

This record is about the *surface* of that agreement: which spellings exist, which
are refused and with what sentence, and where a refusal is allowed to live. The
suffix taxonomy is in [`casts.md`](casts.md) and is not repeated here; the tree
that carries a literal's token is in [`arrays.md`](arrays.md) and
[`lexer.md`](lexer.md).

## The failure model this record repairs

A literal can go wrong in three ways, and until now they were not separated:

1. **The bytes are not a literal at all** (`1abc`, `"` with no closing quote).
   The scanner flags it; the flag is a diagnostic; the pipeline stops.
2. **The bytes are a literal, and the language refuses what it says**
   (`"\q"`, `'\400'`, a number that does not fit). This is a *feature refusal*
   with a sentence — never an internal error, because nothing here is a bug in
   the compiler.
3. **The bytes are a literal the language accepts, and the value cannot be
   built** — today, a character literal whose value is not a byte.

The third case was where this compiler was wrong in a way that matters, and it
was measured rather than reasoned about. All three of these **passed `mincc
check`** and then aborted the compiler in the lowering, inside LLVM's own
`APInt` assertion:

```minc
let c: char = '\U0001F600';   // a code point, packed into a byte slot
let x: i32 = 'ab';            // GCC's multi-character packing, typed `char`
let c: char = 'é';            // two UTF-8 source bytes, typed `char`
```

and two more reached the lowering as **`ir-internal`** — this compiler telling a
reader it has a bug when the program is what is wrong:

```minc
let s: str = "\x1FF";         // a hex escape above 0xFF
let s: str = "\400";          // an octal escape above 0xFF
```

`ir-internal` is the code for "a bug in this compiler" (`ir.md`, decision 17);
the opposite fix belongs to a *feature refusal*. The rule this record restores is
the project's own:

> If the checker lets it pass, it must run — and a literal the language refuses
> is refused by the stage that owns the rule, in the checker's words, and never
> discovered later as an internal error.

So a literal's problem is refused by **the stage that owns the rule** — and the
two escape rules are not one rule:

- the scanner flags the escapes **nothing later reads**. A `str` is the case: no
  stage folds a string, so if the scanner does not refuse `"\x1FF"`, the next
  stage that can is the lowering, and it would call the reader's mistake a
  compiler bug;
- the checker owns **a character literal's width**, because a `char` is one byte
  and only the checker holds the type and the value at once — and only the
  checker can name the fix (the string that means the character, or a wider
  integer).

One mistake gets one sentence either way, and neither is an internal error. The
split was *measured* rather than assumed, and the first version of this change got
it wrong in a way worth recording:

```minc
let c: char = '\u{1F600}';
```

with the width rule in the scanner *as well*, that line produced **two**
diagnostics at the same span — the scanner's, whose fix ("write the code point as
`\u{...}`") is exactly what the program had already written, and the checker's,
which is the one that helps. And `''` produced a cascade on top of
`lex-empty-char`, because a rule that counts code units has to leave a literal the
scanner already refused alone. Hence: **the decoder refuses no width at all** (it
hands the value over), the string reader refuses one, the checker refuses the
other, and the scanner flags only the string's.

## Numbers

### Where the market is, and what each answer costs

| language | grouping | octal | binary | hex float | notes |
| --- | --- | --- | --- | --- | --- |
| C23 | `'` (`299'792'458`) | `0` prefix **and** `0o`? — no: `0` only | `0b` (new in C23) | `0x1.8p3` | a leading zero silently means octal: `010` is eight, and no compiler will tell you |
| C++14/23 | `'` | `0` only | `0b` (C++14) | `0x1.8p3` | the same footgun, kept for compatibility |
| Rust | `_` (`98_222`) | `0o77` | `0b1111_0000` | none | refuses `1.` (needs digits both sides) |
| Zig | `_` | `0o` | `0b` | `0x1.8p3` | refuses `1.` |
| Go | `_` (1.13) | `0o` / `0O` | `0b` | `0x1.8p3` | `0` prefix is octal too, legacy |
| Python | `_` (3.6) | `0o` | `0b` | none | `0` prefix is a syntax error, deliberately |
| Java | `_` (7) | `0` only | `0b` | `0x1.8p3` | digit separators may not be adjacent to the prefix or the suffix |
| C#, Swift, Julia, Odin, JS | `_` | `0o`/`0` | `0b` | some | `_` is the consensus |

Two facts decide this section. **`_` is the modern spelling** — every language
that added grouping after 2013 chose it (Rust, Zig, Go, Python, Java, C#, Swift,
Julia, JavaScript) — and **`'` is the C one**, added by C23 and C++14 because in
C an identifier may begin with `_`. A language whose own source is `.mx` and whose
readers arrive from C wants both, and there is nothing for the two to disagree
about: a separator is *spelling*, the value is the digits, and neither can change
what the other means.

### Decisions

| # | Decision | Why |
| --- | --- | --- |
| **1** | **A digit separator is `_`, and `'` is accepted as C's spelling of it.** | `_` is the modern consensus and the one a new reader tries first; `'` is what a reader porting C23 or C++14 code already has in their fingers. Accepting both costs one predicate and cannot introduce ambiguity, because both are *removed* before the value is read |
| **2** | **A separator is legal only between two digits** — `1_000`, `0xFE'DC'BA'98`, `0b1111_0000`, `1.414'213'562`, `1e1_0`, `0xF_Fp1_0`. Refused: `_1000`, `1000_`, `1__0`, `0x_FF`, `10_u8`, `1_e3` | The rule is one sentence, and it is what makes `'` safe next to the character-literal delimiter: `1'a'` is still `1` and `'a'`, because a separator needs a digit on **both** sides. Rust's rule (`_` may not lead or trail) and Java's (it may not touch a prefix or a suffix) are the same rule said twice |
| **3** | **A misplaced separator is a `lex-` flag and a reader sentence, not two tokens.** `1000_` is one number token with the flag, and the sentence says where a separator may sit | Splitting it would hand the parser `1000` and the name `_`, and its "a literal and a name may not be written together" message would blame the wrong thing — measured, because that is exactly what happens today |
| **4** | **No implicit octal, ever.** `0755` is decimal; octal is `0o755` | C's leading-zero rule is the canonical silent wrong answer: `010` is eight and nothing warns. Every successor language refused it (Python makes it a syntax error; Rust, Zig, Go and Swift never had it). Keeping it would also make one spelling mean different things in `.mx` and in a `#if`, which is the drift `literal.h` exists to prevent |
| **5** | **`0b`/`0B`, `0o`/`0O`, `0x`/`0X`.** No `0d` | The three bases a machine has; a `0d` prefix would be a fourth spelling of "decimal", which is the one spelling that needs no prefix |
| **6** | **A hex float may be written without an exponent**: `0x1.8` is `1.5`. C requires the `p` part; here the `.` and a hex digit after it are enough | C's rule exists because `0x1.8` was not a valid float before C99, and the grammar had to stay unambiguous. A `.` followed by a hex digit cannot be anything else in this language — there is no member whose name is a number — so the extra rule would refuse a spelling that has exactly one reading. The lowering hands `APFloat`'s reader the same number with `p0` appended (measured: that reader refuses a hex float with no exponent, and accepts `0x.8p3`), and `p0` is `×2^0`: the significand and the exponent are untouched, so the rounding is the number's own and not a second reading of it |
| **7** | **A hex float may be written without an integer part**: `0x.8p3` | C parity, and the case that made the rule above necessary: with no digits before the point the scanner has to accept `.` as the start of the fraction. `0x.8` is the same shape as `.5`, one radix over |
| **8** | **`5.` is not a float.** `5.` lexes as `5` and `.` | A trailing `.` is the one spelling whose meaning would change when the language grows member access: `5.foo` and `5.` are one character apart, and a lexer that guessed would decide, once, for every future `struct`. Rust refuses it for the same reason; `5.0` costs one character and says what it means |
| **9** | **The suffix table is unchanged**, and a separator may not touch it (`10_u8`, `1.5_f32` are refusals) | The suffix is the *type* of the literal and the digits are its *value*; a separator that reaches into the suffix is grouping the wrong thing. `casts.md` owns the table |
| **10** | **A 128-bit literal is read in the lowering, and the reader there strips the suffix and the separators explicitly** | `sema` deliberately keeps no value for a literal wider than its 64-bit core, so `src/ir` reads the digits. It was passing the whole spelling to `llvm::APInt` and relying on that reader to stop at the first non-digit: `…455u128` worked *by accident*, and separators would have made it stop at the first `_` — one bug and one coincidence, both removed by passing digits |

### What a number may be, in one table

| spelling | is | note |
| --- | --- | --- |
| `1234567` | `i32` | the default, and the context may ask for another type |
| `1_234'567` | the same number | separators are removed; both spellings are one value |
| `0xFE'DC'BA'98` | hexadecimal | `d`/`D` is not a hexadecimal digit here, so `0d10` is `0` and the name `d10` |
| `0b1111_0000`, `0o755` | binary, octal | `0o` is the only octal spelling |
| `0755` | **decimal** 755 | not octal, by decision 4 |
| `1.5`, `.5`, `1e9`, `1e+9`, `2.5e-3`, `1.5E3` | decimal float, `f64` | `5.` is not one (8) |
| `0x1.8p3`, `0x.8p3`, `0x1p-3` | hex float | the `p` exponent may be absent (6, 7) |
| `10u8`, `12f`, `1.5f32`, `1.5L`, `10ull` | typed by its suffix | `casts.md` |
| `300u8`, `1.5u`, `10wb` | refused, by name | the sentence says what to write instead |

## Strings

`str` here is a **byte string**: NUL-terminated, C-compatible, and not required
to be valid UTF-8. That single fact decides most of the escape table, because a
byte escape (`\x`, `\o`) names a *byte* and a code-point escape (`\u`) names a
*character* whose bytes are its UTF-8 encoding.

### The alphabet

| escape | means | source |
| --- | --- | --- |
| `\n` `\r` `\t` `\v` `\f` `\b` `\a` | the control characters | C |
| `\?` `\"` `\'` `\\` | themselves | C (`\?` is kept even though trigraphs are gone: C23 still has the escape, and refusing it would be a new refusal for old code) |
| `\e` | `0x1B`, ESC | GCC/Clang's own escape, in both C and C++ since forever, and what every terminal protocol is written with. It is *new* here, so no existing program changes meaning |
| `\nnn` | octal, **one to three** digits | C: `\377` is a byte, `\3777` is `\377` then `7` |
| `\o{n...}` | octal, **delimitada**, any number of digits | C++23's delimited escape sequences (P2290) |
| `\xn...` | hex, **as many digits as follow**, refused above `0xFF` | C's rule for where the run ends, with C's unspecified overflow replaced by a refusal |
| `\x{n...}` | hex, delimitada | C++23 |
| `\unnnn`, `\Unnnnnnnn` | a code point, four or eight hex digits | C99 universal character names |
| `\u{n...}`, `\U{n...}` | the same, delimitada | C++23 defines `\u{...}`; `\U{...}` is accepted as its symmetric spelling, because the unbraced pair already has two widths and a braced form that refused one of them would be a question with no answer |
| `\` + newline | **nothing**: the line continues | C's phase-2 splicing, and Rust's *string continuation escape* |
| anything else | refused, `lex-unknown-escape` | C requires a diagnostic for it; so does this language |

### Decisions

| # | Decision | Why |
| --- | --- | --- |
| **11** | **`\x` keeps C's "as many hex digits as follow", and a value above `0xFF` is refused rather than truncated** | The two alternatives are C's (unspecified) and Rust's (exactly two digits, up to `0x7F`, because "it is ambiguous whether they mean Unicode code points or byte values"). This language's `str` is bytes, so `\xFF` is legitimate and must stay one byte; the ambiguity Rust names is answered here by the *delimited* form: `\x{41}B` says where the run ends. Keeping C's run length also keeps `"\x041"` meaning `A` — a change to Rust's rule would silently re-read a valid literal |
| **12** | **The delimited forms are the way to say "the digits end here"** (`\x{41}B`, `\o{101}2`, `\u{e9}0`) | This is exactly what P2290 added them for; it is the one thing the braced family is *for*, and it is why the family is worth the four rows |
| **13** | **`\u{...}` names a code point and a `str` encodes it as UTF-8**, on every target | The language already made this call for `\uXXXX`; a `str` is bytes and the encoding is spelled here rather than inherited from `wchar_t`. No locale, no target, no ABI is consulted — `"\u{e9}"` is `C3 A9` on a mainframe if this compiler ever targets one |
| **14** | **An escape above one byte is refused — by the *string* reader, and by the checker in a character literal** — with the code point form named as the fix | `"\x{1F600}"` is a *byte* escape naming 128512; a `str` has no byte for it, and silently truncating is the one thing a reader must never do, so `parseStringLiteral` refuses it and the scanner's flag makes that refusal visible in `check`. The **decoder** refuses neither: it returns the value, because "does it fit" depends on a type it does not have, and a refusal there would have to guess which fix to name |
| **15** | **A code point above `0xFF` is refused in a character literal** (`'\u{1F600}'`) and accepted in a string | A `char` is one byte (README, *Types*), and the refusal is the checker's — one sentence that names both fixes: the string, which encodes it as UTF-8, and a wider integer, which holds it. The scanner says nothing about a character literal's width, which is what keeps it to one diagnostic |
| **16** | **`\N{NAME}` is refused by name** | C++23's named universal character escapes need the Unicode character-name table: a generated data file of ~150k entries, with its own update policy, in a compiler that today carries no Unicode data at all. That is its own decision, and until it is made the refusal names the code point form |
| **17** | **A separator is not a digit**, inside an escape as anywhere else: `\x{4_1}` is refused | The digits of an escape are already delimited by the braces; a separator there would be grouping something that cannot be misread. Rust allows it and C++23 does not; one answer is enough |
| **18** | **`\` + a newline continues the string and contributes nothing**, for `LF` and `CRLF` | C splices it in phase 2 (before tokens exist) and Rust spells it as an escape; both mean the same thing, and the alternative — refusing it — makes every long literal an exercise in `strcat` or in counting columns. The `CRLF` half is not optional: the source may be either, and a rule that only knew `LF` would treat Windows source differently from Unix source, which is the one thing this project does not do |
| **19** | **A literal newline still ends the string** (the literal is unterminated), and `\r` may not appear in a string body | Kept: a string that crosses a line without saying so is a mistake whose diagnostic is a *column*, and the language has an explicit way to say it (18). Multi-line and raw strings are their own item (see *Not in scope*) |

## Characters

`char` is one byte, unsigned, and distinct from `u8` (README, *Types*); it is the
code unit a `str` is made of. A character literal is therefore **one byte**:

| written | answer |
| --- | --- |
| `'a'`, `'\n'`, `'\0'`, `'\x41'`, `'\101'`, `'\u{e9}'`, `'é'`? | one byte — every one of these is |
| `'é'` | **refused**: two source bytes. `'\u{e9}'` is the code point, `"é"` is the string |
| `'ab'`, `'\xC3\xA9'` | **refused**: two bytes. `"ab"` is the string, `0x6162` is the integer |
| `'\u{1F600}'`, `'\U0001F600'` | **refused**: a code point above a byte. `"\u{1F600}"` is the four UTF-8 bytes |
| `''` | **refused** by the lexer (`lex-empty-char`) |

| # | Decision | Why |
| --- | --- | --- |
| **20** | **The character rules live in the checker**, and the *reader* stays able to pack a multi-character body | The preprocessor evaluates `#if 'ab' == 0x6162` on **C** input, where GCC's packing is the only answer available; the language is the stage that may say no. Putting the refusal in the reader would make a C header's `#if` unreadable to fix a `let` |
| **21** | **`'ab'` is refused, not packed.** C says a multi-character constant is implementation-defined; GCC packs it; Microsoft does not, and `-Wmultichar` exists because the answer surprises people | "Implementation-defined" is where C keeps its worst footguns, and this language refuses C's footguns on purpose (the `long` warning, the `010` rule, the integer/float split). The value is still writable — `0x6162` — and now it is written *as what it is* |
| **22** | **`'é'` is refused, and the sentence says why in the language's own terms** — the source is UTF-8, a `char` is one byte, and `'\u{e9}'` is the code point | The alternative readings are what C does (two bytes packed into an `int`) and what Rust and Go do (a character type wide enough for any scalar). This language's `char` is a byte and its `str` is bytes; decoding a source character to `0xE9` would make `'é'` and `"\u{e9}"` agree and disagree with `"é"` at the same time, which is exactly the ambiguity Rust's `\x` rule cites as its reason |
| **23** | **A value that does not fit a byte can never be typed `char`, and the checker is the one that says so.** `units != 1` or `value > 0xFF` is an error (`sema-literal-out-of-range`), and no value leaves the stage otherwise: an empty body or an escape the scanner already condemned returns the poison type without a second sentence | The three crashes above were all this one missing refusal, and the invariant is what makes them unreachable — the shape `invariants.cc` uses for the module, applied to a literal. The *sentence* is here rather than in the scanner because only this stage knows the literal is a `char` and can therefore name the fix; the lexer's `'ab'` is right to be a spelling (C's `#if` on a multi-character constant needs it) and wrong to be a language rule |

## Cross-platform

- **The encoding is spelled by us.** `\u{...}` is UTF-8 on every target, and
  `\x`/`\o` are bytes; `wchar_t`, `char16_t` and the machine's locale are never
  consulted. Source is already validated as UTF-8 at the boundary
  (`SourceManager`), so a byte above `0x7F` outside a string or a comment is an
  `Invalid` token on every target, not "implementation-defined".
- **The rules are target-independent.** A byte is a byte on every triple named.
  The one place a literal meets the target is the suffix whose *type* the ABI
  decides (`10L`, `1.5L`), and that resolution is `sema/target.h`'s—unchanged by
  this record.
- **`CRLF` is handled in the continuation rule** (18) because the source may come
  from either kind of machine: a Windows file and a Unix file with the same
  characters must produce the same value.
- **The separator is spelling.** Removing it cannot change a value, an overflow
  decision, or a suffix boundary on any target.

## Tests

- **The reader** (`tests/unit/support/consteval/literal_test.cc`): a value table
  per spelling — every base, every separator placement that is legal, every one
  that is not, the delimited escapes, `\e`, the continuation, and the character
  rules — plus the round trip that a spelling is claimed by the scanner, split by
  the reader and classified by the table as the same one thing.
- **The scanner** (`tests/unit/lex/lexer_test.cc`): one token per spelling
  (`1_000`, `0xFE'DC'BA'98`, `0x.8p3`, `"\x{41}B"`, a continued string), the
  flags for each refusal, and the existing exhaustive 1- and 2-byte coverage —
  which now includes `_` and `'` in the byte set, so a separator rule that broke
  a two-byte input would fail there.
- **The checker** (`tests/unit/sema/`): each character refusal, by code and by the
  sentence's fix (`'ab'` names both the string and the packed value; `'\xC3\xA9'`
  is refused for being two units, not for being wide; every byte spelling is
  accepted), and the proof of decision 23 — a `char` literal always has a byte
  value, checked by walking the accepted set.
- **The ownership rule itself**, by running the front end: one mistake, one
  diagnostic — `''`, `'ab'`, `'\400'`, `'\u{1F600}'`, `'\uD800'` and `'é'` each
  produce exactly one, from the stage this record says owns it.
- **The corpus** (`examples/005_literals.mx` and the `ir` tests): every accepted
  spelling appears in an example that runs, because the record's claim is that a
  literal the checker accepts can be built.

## Not in scope, with the reason

- **Raw and multi-line strings** (`r"..."`, `"""..."""`, backticks): a spelling
  question about the *delimiters*, with a decision each about escapes (raw means
  none), about trailing newlines, and about indentation stripping. Its own item;
  the continuation escape (18) is what a long literal uses until then.
- **Adjacent string literal concatenation** (`"a" "b"`): it belongs to the
  translation-phase model (C merges them after preprocessing), and this compiler
  has no phase 6. A *parser* that merged them would have to invent which token's
  spelling the value comes from; its own item.
- **String prefixes** (`L"..."`, `u8"..."`, `u"..."`, `U"..."`): the language has
  exactly one string type and one character type, so a prefix would have to
  change the element width — that is a type-system decision, not a literal one.
- **`\N{NAME}`**: a Unicode name table is a data dependency (16).
- **Imaginary, decimal (`_Decimal32`) and `_BitInt` literals**: the types do not
  exist. `wb`/`uwb` are already refused *by name* so the sentence can say `i64`.
- **Trigraphs**: removed from C23 and never present here.
- **`0` as an octal prefix**: refused on purpose (4), and the refusal is the
  reason `010` is decidable at all.

## The order it lands

1. **The flags** (`include/lex/token.h`, `src/lex/token.cc`): `MisplacedSeparator`,
   `EscapeDigits`, `EscapeTooWide`, `NamedEscape` — because every refusal in this
   record has to be visible in `check`, and a flag is how a scanner says so.
2. **The scanner** (`src/lex/literal_scanner.cc`): separators claimed into the
   token, the no-integer-part hex float, the delimited escapes, `\e`, the
   continuation, and the decoding of an escape's value (which is what decides
   "too wide").
3. **The readers** (`support/consteval`): separators skipped and validated, the
   delimited forms decoded, the continuation dropped, `decodeCharOrEscape`
   answering "no unit" for a continuation, and a helper that strips separators
   for the one consumer that hands a number to `llvm::APFloat`.
4. **The checker**: the character rules (20–23), and `char` literals joined to the
   existing `LiteralOutOfRange` family.
5. **The lowering** (`src/ir/types.cc`): the wide-integer path takes digits and
   not a spelling, and the float path takes a number with no separators and with
   the exponent the language lets a hex float omit (`p0`), because `llvm::APInt`
   and `llvm::APFloat` both stop at the first byte they do not know — a separator
   there would be a *wrong value* rather than a refusal.
6. **The tests, then the site**: `examples/005_literals.mx` is the page a reader
   can run, and `website/docs/language/expressions.md` is where the tables above
   are published for readers rather than for maintainers.

## References

- ISO/IEC 9899:2024 (C23) 6.4.4, 6.4.4.4, 6.4.5: integer and floating constants,
  character constants, and the `'` digit separator.
- ISO/IEC 14882:2023 (C++23) `[lex.icon]`, `[lex.fcon]`, `[lex.ccon]`, and
  P2290R3 *Delimited escape sequences* (`\x{}`, `\o{}`, `\u{}`, `\N{}`).
- The Rust Reference, *Tokens*: `\x` as exactly two hex digits and why ("ambiguous
  whether they mean Unicode code points or byte values"), `\u{...}` with up to six
  digits, `1.` as an error, and string continuation escapes.
- The Zig Language Reference, *Grammar*: `_` separators, `0b`/`0o`/`0x`, hex
  floats with a mandatory `p` exponent.
- The Go Language Specification, *Integer literals* and *Floating-point literals*:
  `_` separators (Go 1.13), `0o`, and `0` as octal for compatibility.
- The Python Language Reference, *Numeric literals*: `_` grouping and the refusal
  of a leading zero.
