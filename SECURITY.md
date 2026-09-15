# Security policy

## Supported versions

minc+ is pre-release. Only the tip of the default branch is supported: there are
no maintenance branches, and a fix is a new commit rather than a backport.

| Version | Supported |
| --- | --- |
| `main` (0.x) | :white_check_mark: |
| tagged releases | none yet — the first tag will be `0.1.0` |

## Reporting a vulnerability

**Please do not open a public issue for a security bug.**

Use GitHub's private reporting — *Security* → *Report a vulnerability* on this
repository. If you would rather not use it, open an issue that says only that you
have a report and how to reach you, and nothing about the bug itself.

A good report contains:

- the smallest `.mx` file that shows the problem, if the problem is about a
  program;
- `mincc -vV` — the version, the binary, the host, the default target and the
  LLVM version are all in that block;
- the exact command, and what happened against what you expected;
- for a wrong-code report, what the program printed and what it should have
  printed (and, if you have it, the IR from `mincc ir`).

We aim to acknowledge a report within a few days, to say whether we agree it is a
security bug, and to keep you informed while it is being fixed. Please give us
**90 days** before publishing; a fix that ships earlier is a reason to publish
earlier, and we will say so.

## What counts as a security bug in a compiler

A compiler is a trusted tool: it reads input a user may not control and produces
a program the user runs. So the interesting bugs are not the usual
memory-corruption shapes at the input boundary (though those count — see the
last item).

**1. Wrong code.** The compiler accepts a program and emits a module whose
behavior differs from what the language says. This is the highest-severity class,
because the user's program is the thing that is wrong, and nothing downstream can
detect it. The language's promise — *if the checker lets it pass, it must run* —
makes this the one failure the design of the pipeline exists to prevent. An
assumption handed to LLVM that the program did not state (an `nsw`, an `inbounds`,
a type-based alias result) belongs here.

**2. Silent acceptance of an ill-formed program** in a way that produces a
broken module — a module LLVM's verifier rejects, or one that links and crashes.

**3. Undefined behavior in the compiler itself** on any input: a malformed
source file, a deeply nested expression, a hostile macro, a truncated file, a
file that is not UTF-8. The lexer indexes bytes directly, the parser recurses,
and the preprocessor expands macro arguments, so each of those is a boundary
worth attacking. The `sanitize` preset (AddressSanitizer and
UndefinedBehaviorSanitizer) exists for exactly this, and an input that trips it
is a bug worth reporting.

**4. Resource exhaustion** from a bounded input. Every limit in the compiler
lives in `support/limits.h` and is enforced, and a translation unit is not
allowed to make `mincc` allocate without bound, hang, or take time that is
super-linear in its size. A quadratic blow-up or a stack overflow on a *legal*
input is a security bug; so is a preprocessor construct that escapes its budget.

**5. The build and the supply chain.** Anything that lets a build produce a
binary that does not match the sources.

## What is not in scope

- **A program that misuses raw pointers.** This language states its memory model
  in writing, and it says what happens when an obligation is not met: the access
  is outside the model. `*p` through a pointer that has left its object is a bug
  in the *program*, is documented as such, and does not become the compiler's
  vulnerability. See `docs/architectures/memory.md`.
- **A feature that does not exist yet.** Arrays, aggregates, casts and the
  standard library are marked as unimplemented on the documentation site; a
  program that needs one failing to compile is expected.
- **A diagnostic that is unhelpful, or a missing warning.** Those are bugs, and
  ordinary issues are the right place for them.
- **Anything that requires the attacker to already run code as the user.**
