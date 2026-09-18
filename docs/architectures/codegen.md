# Codegen — `src/backend/llvm`, and the driver's `build` and `run`

The design record for the last stage of the pipeline that produces something
other than a diagnostic, and for the two commands that reach a user: `build`
and `run`. [`ir.md`](ir.md) ends with a module that has a triple, a data layout
and no target machine, and says why: the expensive, target-specific,
sometimes-unavailable half is this stage's. This document is that half.

It is also the record where the project's central promise stops being about the
compiler and starts being about the *program*: **if the checker lets it pass, it
must run.** That sentence decides almost everything below — which failures are
allowed to exist, which are refusals, and why `run` is not what the previous
document assumed it would be.

Research behind it: the LLVM 22 `LangRef` and the `TargetMachine`,
`PassBuilder`, `DIBuilder`, `Verifier` and `ORC` references, read against the
copy installed on this machine (**22.1.8**), whose build we can also *ask* what
it supports (`llvm-config --targets-built` — `AArch64 AMDGPU ARM AVR BPF Hexagon
Lanai LoongArch Mips MSP430 NVPTX PowerPC RISCV Sparc SPIRV SystemZ VE
WebAssembly X86 XCore`, so every target this project names is present here and
none of them is guaranteed on someone else's machine, which is a distinction the
stage has to encode). Externally: Clang's `BackendUtil`/`CodeGenAction` (the
shape of "run passes, then emit"), `lld`'s driver and `link.exe`'s command
surface (what a linker driver actually has to know), DWARF 4/5 and CodeView (what
a debugger reads), `dsymutil` and PDB (what macOS and Windows add *after* the
link), and the ordinary C toolchain behaviour of the three hosts — which is
where the most expensive mistake in this document came from (§ *Position
independence*).

**Status: shipped.** `src/backend/llvm` selects the target machine and writes
the object or the listing, `mincc build` drives the linker driver, and `mincc run`
executes the program it produced. Two things this document decided *revise*
`ir.md` rather than extend it — where `run` executes, and the assumption scan's
metadata rule — and both revisions are marked in place there. Every code of the
failure table below has a row in `diagnostics.cc` and a name a reader can grep
for; the per-code test inputs and the unreachability sweep over that table are
the one part of this stage that is written down and not yet built, and
[`../roadmap.md`](../roadmap.md) § 7 says so.

## The one question that decides the shape: what is `run`?

[`ir.md`](ir.md) (decision 15, § *`mincc run`: ORC*) says `run` is `LLJIT` on
the host triple: build the module, add it to an in-process JIT, look up `main`,
call it. The argument was speed and the absence of a linker. That argument is
real, and the design is wrong for `run`, for three reasons that only appear once
you write down what `run` is *for*.

1. **`run` is a user-facing command, and the program is untrusted.** JIT'd code
   executes *inside the compiler's process*. A program with a wild pointer takes
   the compiler down with it; the user sees `mincc` segfault and no diagnostic,
   and the compiler's own crash-handling (which is careful, and which the rest
   of this project spent its effort on) never runs. A program that calls
   `exit(0)`, or `abort()`, or that installs a signal handler, changes the
   compiler's process state. The isolation is not a nicety; it is the difference
   between a tool and a toy.
2. **`run` must be the same thing as `build`, or it is a second implementation
   of the program's semantics.** If `run` JITs and `build` links, then a bug in
   either path is a bug in only one of them, and the two can disagree — the
   exact failure mode `driver/frontend.*` was created to prevent one stage up.
   `run` is `build` plus `exec`, and that is a property worth having by
   construction rather than by care.
3. **A real process is what the user is comparing against.** `argv`, `stdin`,
   `stdout`, `stderr`, the exit code, a signal death — all of these are the
   operating system's, and a JIT reproduces them approximately. "It ran under
   `mincc run` but not as a binary" would be a bug report this project could not
   answer.

**Decision: `run` is `build` into a temporary executable, executed as a child
process.** `run` and `build` share one code path down to the linker; `run` then
`fork`s/`exec`s the result with the arguments after `--`, inherits the three
standard streams, and returns the child's exit status — or, for a signature
death, an abnormal-termination sentence and a non-zero status. (This paragraph
used to promise the signal's number and the shell's `128+n`; the [*`run`*](#run)
section below has what shipped and why the number is not named.)

The JIT does not disappear; it moves. [`ir.md`](ir.md) wanted it as a
**differential oracle** — compile a program, execute it, compare the answer
against the same program translated to C — and that is a *test's* job, not a
user's. There, in-process execution is a feature: no linker needed, no process
per case, and a crash is a failing test with a stack trace rather than a
user-facing failure. So the oracle keeps `LLJIT`, lives in `tests/`, and is
governed by the rules `ir.md` § *`mincc run`: ORC* already states (process
symbols visible, context ownership transferred, host triple only, and **never**
the engine behind `run`).

> Revision to `ir.md`: decision 15's "`run` is `LLJIT`" becomes "`run` is
> `build` plus `exec`; `LLJIT` is the test suite's differential oracle". The
> rest of that section — `libc` reachable through
> `DynamicLibrarySearchGenerator::GetForCurrentProcess`, the
> `ThreadSafeModule` ownership transfer, host-triple-only — stands unchanged and
> applies to the oracle.

### What this buys, concretely

- **A crash is a crash.** `mincc run crash.mx` reports that the program did not
  exit normally and exits non-zero; the compiler is still alive to have said so.
- **`run` cannot be more permissive than `build`.** If the link fails, `run`
  fails with the linker's message, exactly as `build` would.
- **The exit code is the program's.** `mincc run prog.mx` exits with whatever
  `prog` returned, so `run` composes in a shell.
- **`-g` works.** The binary under the debugger is a normal binary with normal
  DWARF; there is no "debug it under the JIT and hope" mode.

## What the stage is: a module in, an object out

`codegen` receives `ir::Module` — a handle that owns an `LLVMContext`, a
`DataLayout` and an `llvm::Module` — and produces bytes: a relocatable object
(`CGFT_ObjectFile`), an assembly listing (`CGFT_AssemblyFile`), or nothing
(`CGFT_Null`, which is what `--emit=none` and a syntax/semantic-only pass use).
The enumeration is LLVM's (`Support/CodeGen.h`: `CodeGenFileType {
AssemblyFile, ObjectFile, Null }`) and it is *total*, which is the same rule
`ir.md` applies to `TypeKind`: an emit kind added by LLVM is a compile error in
our switch rather than a silent gap.

The stage contains **no instruction selection, no register allocation, no
scheduling and no target knowledge**. It selects a `TargetMachine` from the
triple `sema` decided, runs LLVM's pass pipeline, hands the result to LLVM's
emitter, and reports what happened. Every claim in `ir.md` about *why* the IR is
LLVM's is a claim about this stage's size: this file is the *smallest* in the
pipeline for how much it decides.

### The unit of work, and its concurrency

One `TargetMachine` per module, created from the module's triple and destroyed
with the emission. A `TargetMachine` is **not** safe to share across threads
emitting different modules — it carries mutable per-emission state (the MC
layer, the subtarget caches) — so "one per unit" is the same rule `ir` already
applies to `LLVMContext`, for the same reason: two units compiled concurrently
in an editor must not be a race.

`ir` already established that one unit's lowering is independent of another's.
That carries through here unchanged: N modules emit on N threads, each with its
own context, layout, target machine and output stream, and the only shared
object is LLVM's *read-only* target registry, which is what the initialization
below exists to populate.

## The target machine

`TargetRegistry::lookupTarget(triple, error)` answers the registered `Target`,
and `Target::createTargetMachine(TT, CPU, Features, Options, RM, CM, OL)` builds
the machine. Three of those arguments are decisions rather than defaults:

- **`CPU` and `Features` are the empty string.** Not the host's. A target
  machine built with `"native"` would make the object depend on the machine that
  happened to compile it, which is the opposite of what a triple is for. The
  host's own feature set is for a JIT; this is not one.
- **`RM` (relocation model) is stated, never inherited** — see § *Position
  independence*.
- **`CM` (code model) is `Small`**, which is what every ordinary program is and
  what `codegen`'s future flag will let a large one change.

`OL` (the backend's own optimisation level) is *not* the `-O` the user typed:
the IR pipeline is where `-O0..-O3` happens, and the backend's level is set to
match so that instruction selection and scheduling do not undo it. Getting these
two "the same number" is the whole of it.

### Initialization, once, and why it is in two places

LLVM's targets live in static registries that a linker is free to discard, so a
program that uses LLVM must register them itself. `InitializeAllTargets` (and
its `TargetInfos`, `TargetMCs`, `AsmPrinters` and `AsmParsers` siblings, all in
`Support/TargetSelect.h`) are the call, and the codegen path needs **more of them
than `ir` does**: object emission goes through the target's `AsmPrinter`, which
`InitializeAllTargetMC…` does not register.

`ir` already has a `call_once` guard that registers `TargetInfos`, `Targets` and
`TargetMCs` (it needs them to build a `DataLayout` for the triple). `codegen`
gets its own `call_once` that registers the full set. Two guards and not one
shared one because **`ir` may not depend on `codegen`** — the pipeline's rule is
that no stage depends on a later one, and a shared "LLVM init" module below
`sema` would put `llvm/*` under the boundary that `ir.md` decision 13 draws
there. The cost of two guards is a redundancy that runs once per process; the
cost of one shared module is the isolation rule.

Registrations are idempotent (`RegisterTarget` guards itself), so the ordering
does not matter and neither guard has to know about the other.

### When the target is not there

`lookupTarget` returns `nullptr` for a triple this build of LLVM has no backend
for — `llvm-config --targets-built` is exactly the list that decides it, and a
distribution LLVM is routinely built with a subset. That is a **refusal with its
own code** (`codegen-target-unavailable`), never a crash and never a guess: it
says which triple, and it says that it is this LLVM build's limitation rather
than the program's. It is the same posture as `sema`'s refusal of a triple it
does not state, and for the same reason: an ABI we cannot compute is not an ABI
we may assume.

Note the asymmetry with `ir`, and that it is deliberate: `ir` needs only the
target *info* to build a data layout, and `sema` refuses a triple whose ABI it
does not know; `codegen` needs a *code generator*. So a triple can be perfectly
well understood by `mincc check` and `mincc ir` on a machine whose LLVM has no
backend for it, and `build` is where that finally matters. `--emit=ir` on such a
machine keeps working, which is the split `ir.md` decision 11 was written to
protect.

## Two pass managers, and why that is not a mistake

The middle-end optimisation pipeline is the **new** pass manager, driven by
`PassBuilder` (`Passes/PassBuilder.h`): `buildPerModuleDefaultPipeline(Level)`
for a normal build, `buildO0DefaultPipeline(Level)` for `-O0`, and
`registerPipelineParsingCallback`/`registerAnalysisRegistrationCallback` if a
future flag names a pass. The codegen pipeline is the **legacy** pass manager,
because `TargetMachine::addPassesToEmitFile` takes a
`legacy::PassManagerBase&`. This is LLVM's own split — the same one Clang uses —
and it is worth naming in the record precisely so that a reader does not "fix"
it. The two managers are sequenced, not mixed: optimise with the new PM, then
emit with the legacy one.

Three details of the emission call are traps rather than choices:

- **It returns `true` on failure.** `addPassesToEmitFile` returning true means
  "emission of this file type is not supported", which is the opposite of the
  convention every other boolean in this codebase follows. It is wrapped once,
  beside `verifyModule`'s polarity, so the two inversions are stated in one
  place instead of remembered in two.
- **`DisableVerify` defaults to `true`** — the *backend's* machine verifier is
  **off by default**. This project asks for it explicitly (pass `false`), which
  is one line and turns "the instruction selection produced something malformed"
  from a miscompile into a diagnostic.
- **`TargetMachine::registerPassBuilderCallbacks(PassBuilder&)` must be called**
  before the pipelines run. It is where a target contributes its own passes;
  skipping it compiles fine and quietly omits them.

### The optimisation levels

`-O0`, `-O1`, `-O2`, `-O3`, `-Os`, `-Oz` map onto `OptimizationLevel::O0…Oz`
(`Passes/OptimizationLevel.h`, which is a class and not an enum — the same
letter is a different object at a different level). The default is `-O0` for
`build`, and `ir.md` already fixes the reason it is safe to have any level at
all:

> The optimiser cannot invent the undefined behaviour we refused, because it is
> not in the IR we hand it.

That is the payoff of the runtime contract. It is also the reason `-g` and `-O3`
can coexist here (§ *Debug information*): optimisation is licensed by what the
module states, and the debug info is a mapping *on top of* the semantics rather
than a part of them.

## Position independence: the failure that only shows up on someone else's machine

This is the section that earned its place by being measured rather than
reasoned about. On this machine:

```
$ gcc -v 2>&1 | grep -o enable-default-pie
enable-default-pie
```

and then, with a position-**dependent** object:

```
$ clang -c -fno-pic t.c -o t_nopic.o
$ cc t_nopic.o -o t
/usr/bin/ld: t_nopic.o: warning: relocation against `g' in read-only section `.text'
/usr/bin/ld: warning: creating DT_TEXTREL in a PIE
```

A `TargetMachine` built with LLVM's default relocation model emits exactly that
object, and the host's `cc` on a distribution with `enable-default-pie` — which
is every modern Debian/Ubuntu/Fedora — links it into a PIE. Today that is a
warning and a `DT_TEXTREL`; it is a **hard error** with `-Wl,--fatal-warnings`,
on architectures whose `ld` will not do text relocations at all, and on the day
a linker changes its mind. It is the canonical "works here, broken on the user's
machine" bug, and the only defence that does not depend on the host is to not
produce the object that triggers it.

**Decision: emit position-independent code on every platform, and state the
relocation model per triple rather than inheriting LLVM's default.** PIC links
into a PIE, into a non-PIE, and into a shared library; a static object links into
exactly one of those and needs the driver to be told which. The cost on ELF is a
GOT indirection for the address of a global, and this language's globals are
string literals. One table, read in one place, so the answer cannot depend on
which `TargetMachine` overload was called.

### COFF: the same answer, for a sharper reason

The Windows row of the table said `Static` for a while, on the assumption that
COFF's default is what the platform expects. It is not, and the way it failed is
worth keeping, because it is the *second* time this section has earned its place
by measurement:

```
D:/a/_temp/.../unit0.o:(.text+0x25): relocation truncated to fit: IMAGE_REL_AMD64_ADDR32 against `.data'
collect2.exe: error: ld returned 1 exit status
```

`Static` is the small code model's assumption that an address fits in 32 bits,
and under it LLVM addresses a global absolutely. A PE image is based at
`0x140000000`, so a 32-bit absolute address of a `.data` object is not
representable and `ld` refuses the object. What made this a CI-only mystery is
that the *form* depends on the optimiser: the module's own guard message was
`movabsq $.Lcheck.site, %rcx` at `-O0` (a 64-bit relocation, which links
perfectly) and `movl $.Lcheck.site, %ecx` at `-O2` (a 32-bit one, which does
not). An object that links on the machine it was developed on and not on the
machine that has never seen it is the failure mode this whole section exists to
prevent — and it was invisible to every local gate, because no local gate emits a
PE object.

`PIC_` on COFF costs nothing that matters: it is RIP-relative addressing of a
local symbol (`leaq .Lcheck.site(%rip), %rcx`), not the GOT indirection this
section originally assumed. COFF has no `GOT`; a *local* symbol is reached
through the instruction pointer directly, and an imported data symbol gets the
`.refptr` thunk the platform itself uses for the job. It is also what `clang`
emits for the target under `-fPIC`, `-fno-PIC` and neither — three spellings, one
object — which is the simplest available statement of "this is the platform's
answer".

The corollary is a rule about the *linker*: because we always hand the driver PIC
objects, we never have to pass it `-no-pie`, which is a flag that macOS does not
have and that Windows spells differently. A flag we do not need is a portability
problem we do not have.

## The link: a driver, not a linker

`build` ends with an executable, and the naive way to get one is to `exec` the
linker. It is the wrong way, and the reason is the whole of this project's
philosophy applied to a new subject: a linker needs to be told where the C
runtime is, and that knowledge is *platform knowledge of a specific machine*,
not of a target.

To link an executable by hand you must know, per host: the startup objects
(`crt1.o`/`Scrt1.o`, `crti.o`, `crtn.o` on ELF; nothing on Mach-O), the dynamic
linker path, `libc` and `libm` and `libgcc`/`compiler-rt`, the C++ ABI library
if anything C++ is linked, the macOS SDK and `-lSystem`, and on Windows the
MSVC import libraries, `/subsystem:console`, `/machine:`, and the entry symbol
convention. Every one of those is a place to be subtly wrong, and every one of
them is already correct inside `clang` and `gcc`, which were written by people
whose job it is.

**Decision: `build` does not link. It drives a linker *driver*, which links.**
The program is `--linker PATH` if it was given, and otherwise the first of these
that exists:

| Order | Program | Why |
| --- | --- | --- |
| — | `--linker PATH` | Explicit, and wins over everything below: the answer when the search picks wrong, and the escape hatch when it picks nothing |
| 1 | `clang` | The same LLVM generation as this compiler, so it understands every target we emit; on Windows it finds and drives `link.exe`/`lld-link` correctly, and it translates the Unix `-l`/`-L` spellings |
| 2 | `cc` | The system's C compiler, present wherever a C toolchain is |
| 3 | `gcc` | `cc` missing but the GNU driver present |

Discovery is `llvm::sys::findProgramByName` (the platform's `PATH` rules,
including `PATHEXT` on Windows), and the failure to find one is a refusal with
its own code and a sentence naming `--linker`, which is the flag that fixes it.

Three properties of the invocation matter more than the search order:

- **An `argv` array, never a shell string.** `llvm::sys::ExecuteAndWait` takes a
  `StringRef[Program]`/`ArrayRef<StringRef>` vector, and the child is spawned
  with those exact words. There is no shell, so there is nothing to quote, no
  `"` to escape, no `$` to expand and no command injection — and, on Windows,
  none of the quoting rules that a naive string concatenation gets wrong for a
  path with a space in it. The user's paths are passed as data, because they are
  data.
- **The child's `stderr` is inherited, and its exit code is the answer.**
  `ExecuteAndWait` returns the status; a non-zero one becomes
  `codegen-link-failed`, and the linker's own message has already been printed
  in the linker's own format. We do not try to parse the linker's error text —
  that is a second implementation of the linker's diagnostics and a permanent
  maintenance cost — and we do not swallow it either.
- **The objects are real files in a real temporary directory**, created with
  `llvm::sys::fs::createUniqueFile` (which is `O_CREAT|O_EXCL` and a random
  suffix, so the create is atomic) and owned by an RAII object that removes the
  directory on every exit path, including the error ones. A predictable path in
  `/tmp` is a symlink race waiting to happen, and a compiler that writes to
  `/tmp/mincc-out.o` is a compiler that will one day write through someone
  else's symlink.

On Windows the command line has a hard length limit (32767 characters, and
`cmd`'s 8191 if anything goes through a shell — which nothing here does). The
mechanism for the day it matters is a **response file**: the driver writes the
arguments to a file and passes `@path`, which `link.exe`, `lld-link`, `clang`
and `gcc` all accept. It is listed in the roadmap and it is deliberately *not*
built now: it exists to solve a length problem this language's programs do not
yet have, and a mechanism with no input that reaches it is a mechanism nothing
tests.

### `main` and the entry point

Linking through a C driver is also what gives us a C runtime start-up: the
driver supplies the object that calls `main`, so `main` is exactly the symbol a
C program's `main` is, with exactly its calling convention, and the process
starts the way every process starts. `src/cinterop` will need to know much more
about that boundary; today, this is the entire of it, and it is free.

One thing the driver checks before it links, because the linker's answer is
unhelpful and platform-specific: **a unit that defines no `main` cannot be
linked into an executable.** The compiler already has that fact — it has the
module and the symbol table — so it says it in its own words
(`driver-no-entry-point`), instead of relaying `undefined reference to 'main'`
from GNU `ld`, `Undefined symbols: _main` from `ld64`, or
`LNK2019: unresolved external symbol main` from `link.exe`. `--emit=obj` and
`--emit=asm` do not need an entry point, and neither does a future
`--shared`.

### Cross-compiling, and where it stops

`--emit=obj` and `--emit=asm` work for **any** triple LLVM has a backend for:
the object is a self-contained artifact and needing nothing but the backend is
the point.

Linking an executable for a *foreign* triple is refused
(`codegen-linker-unavailable`) unless the user supplies the linker and the
sysroot explicitly (`--linker`, `--sysroot`). The reason is not conservatism: to
link for another target we would need that target's `libc` and startup objects,
which are a *sysroot* — a thing the user has or does not — and a compiler that
silently links a foreign object against the *host's* libc produces a binary that
does not work and does not say why. `--target … --emit=obj` is the honest way to
cross-compile today, and it is a complete one.

## Debug information

### Why it is built in `ir`, and emitted here

[`ir.md`](ir.md) § *Debug information* already decides this, and the argument
holds: a source location is a property of the *node*, so the only stage that has
both the node and the instruction it became is the lowering. Attaching locations
to a finished module would mean correlating instructions back to nodes — a
second walk, in lockstep, over structures that have already lost the
correspondence. So `DIBuilder` is driven from `src/ir`, under `-g`, and
`codegen` does not create a single metadata node.

What `codegen` owns is everything *after* the metadata: the triple decides
whether it becomes a `DWARF` section or a `CodeView` record (`DwarfDebug` vs
`CodeViewDebug`, chosen inside LLVM, not by us), the object carries it, and the
linker decides where it lands. The stage's whole involvement is that it must
**not lose it**: `-g` implies the object is written with its debug sections, and
the link must be told to keep them (§ *Windows (COFF)*).

This is also where a cross-platform claim that looks like a lie becomes true.
"One metadata, three debug formats" is exactly the payoff of the triple being
the single source of truth: `minc+` debug info is DWARF on Linux and macOS and
CodeView on Windows without `ir` or `codegen` containing the word for either.

### What `-g` has to produce, to be worth anything

The test for this is not "does the metadata exist" but "can `gdb` do the four
things a debugger is for". So the bar is stated in those terms, and each item
has a tool that answers it:

| What the user does | What makes it work |
| --- | --- |
| `gdb ./prog`, `run` | `DW_AT_low_pc`/`high_pc` per function: the `DISubprogram` is attached to the `llvm::Function`, and the linker's symbol table has the name |
| breakpoint on a line | a line table: a `DILocation` on every instruction that has a span, from the same `support/line` index the diagnostics use |
| `break add`, by function name | the parameter spill carries **no** line, so the `prologue_end` flag LLVM derives from the line table lands on the function's first *statement* and not on its declaration: the frame exists when the breakpoint fires, and `info args` shows values rather than registers (decision 22) |
| `bt` shows a source frame | the `DILocation` on the `call`, plus `DW_AT_name`/`DW_AT_linkage_name` on the `DISubprogram` |
| `print x`, `ptype x` | `#dbg_declare` per binding, with a `DILocalVariable` carrying a `DIBasicType`/`DIDerivedType` whose size and encoding match the type `sema` decided |
| `ptype` a slice, `print s.len` | the descriptor is a `DW_TAG_structure_type` with two `DW_TAG_member`s, `ptr` and `len` — and each member's *type* is read through `debugType` by `TypeId`, so the length is the checker's `usize` and not a basic type named in `ir`: one number, one type, and `ptype` does not answer `len len` |

Three details decide whether that table is true rather than aspirational:

- **Debug *records*, not intrinsics.** LLVM 19+ made `#dbg_declare`/`#dbg_value`
  the default representation and the `llvm.dbg.*` intrinsic calls the legacy
  one, and the reference is explicit that **the two must never be mixed within
  one module** (LLVM's `SourceLevelDebugging`, § *Debug information descriptors*
  through § *Debugger intrinsic functions*). We use `DIBuilder::insertDeclare`,
  which
  produces the record form, and there is a test that the module contains no
  `llvm.dbg.` call. Mixing them is the kind of mistake that produces a module
  that verifies and a debugger that lies.
- **The module flags, which nothing else tells you to set.**
  `!llvm.dbg.cu` (the compile units), `"Debug Info Version" = 3`
  (`DEBUG_METADATA_VERSION`, `IR/Metadata.h`) and `"Dwarf Version" = 4`
  (`DWARF_VERSION`, `BinaryFormat/Dwarf.h` — 4 is LLVM's default; 5 is opt-in
  and a separate decision for the day a feature needs it). Missing the version
  flag is a module that a *consumer* rejects with a message about metadata,
  which is a confusing way to learn this.
- **`DIBuilder::finalize()` is called, once, before emission.** It resolves the
  forward references the builder deferred (`createCompileUnit`'s
  `retainedTypes`/`subprograms`): a subprogram built before the function body
  exists is a temporary that `finalize` repairs. Skipping it produces metadata
  that looks right and has a null scope in the interesting places.

The *language* of the compile unit is `DW_LANG_C99` and not a vendor string,
deliberately: `gdb` parses expressions and picks type interpretations from that
field, and for a C-shaped language the C parser is the one that makes `print x`
work. Declaring a language `gdb` does not know would make the debugger fall back
to a minimal mode where the metadata we just emitted is unreadable. `minc+`'s
extensions (`let`, `const`, no implicit pointer/integer conversion) are a
*tighter* subset of C's behaviour, not a different one, so the mapping is honest
today; the day the language diverges far enough that a debugger misleads a
reader because of it, this is one constant and it is written down here so the
change is a decision rather than an archaeology.

### The conflict with the assumption scan, and its resolution

`ir.md` § *The assumption list* is a closed list of metadata and attributes the
module may not carry, and `invariants.cc` enforces it with a scan whose rule is,
verbatim, *"an instruction carries metadata; the language emits none"*. Under
`-g` **every instruction carries `!dbg`**, so the two halves of a decision
already made contradict each other. Nothing caught it because neither half is
implemented yet; writing it down is how it gets caught.

The resolution is to make the rule precise rather than to weaken it, because the
scan's *reason* still holds exactly:

> The scan exists because metadata is an assumption about the program that the
> compiler is telling the optimiser. `!tbaa` says two accesses do not alias;
> `!alias.scope` says two scopes do not; `!range` says a value is bounded. A
> `!dbg` attachment says *where in the source this came from*. It is not an
> assumption about the program's meaning and it licenses no transformation —
> the optimiser's handling of it is defined by LLVM's own contract and is
> "preserve or drop", never "exploit".

So the scan's rule becomes **"no metadata except debug info"**: `!dbg` and
`!llvm.dbg.*` are permitted, every other kind of attachment and every metadata
`inbounds`-style assumption stays forbidden, and the scan's diagnostic keeps
naming what it found so the exception cannot quietly widen. The forbidden list
is a *permit-list*, not a deny-list, which is what makes "except debug info" a
one-word change and a new metadata kind a failing test.

Two refinements go with it, and both are tests:

- **Debug info must not change the code.** The instruction stream with `-g` and
  without it is identical; only `!dbg` attachments and debug records differ.
  This is checkable exactly — strip the debug info and compare the dumps — so it
  is checked, rather than asserted. It is the property that makes `-g` free to
  leave on, and it is the one a later change to the lowering is most likely to
  break.
- **A broken debug-info scope does not fail the build.** `verifyModule` takes a
  `bool *BrokenDebugInfo` out-parameter precisely so a malformed line table can
  be reported apart from a malformed module, and `ir.md` decision 18 already
  says the answer is to strip the metadata and carry on. A build that fails
  because a scope chain is wrong trades a breakpoint for a compiler error, which
  is the wrong trade in both directions.

## The command surface

### `build`

```
mincc build [options] <files...>
  -o PATH          the output; default `a.out` (`a.exe` on Windows)
  -O LEVEL         O0..O3, s, z; default O0
  -g               emit debug information
  --emit KIND      exe (default), obj, asm
  --target TRIPLE  the ABI; the linker is only driven for the host
  -L DIR, -l NAME  forwarded to the linker driver
  --linker PATH    the linker driver to use
  --sysroot DIR    forwarded; required with -L/-l arguments
  -v               print the linker command before running it
  --               everything after is a file
```

- **`--emit exe` with one input** writes the default output, like every C
  compiler, because a compiler that writes `a.out` when you did not say where is
  what the muscle memory expects.
- **`--emit obj` with N inputs** writes N objects, one per unit, named after
  each input (`x.mx` → `x.o`) unless `-o` names one, in which case N > 1 is a
  usage error rather than a guess. One object per *unit* is the rule `ir`
  established: one module, one context, one target machine.
- **`-v` prints the exact `argv` it is about to run**, because the first thing
  anyone does with a link failure is look at the command, and a driver that will
  not show it is a driver that gets replaced by a Makefile.
- **`--emit ir`** is not here: it is `mincc ir`, one command up, and it does not
  need a target machine at all. The two stay separate for the reason `ir.md`
  decision 11 gives.

### `run`

```
mincc run [options] <files...> [-- <program arguments>]
```

`run` accepts every `build` option (it is `build`), then:

- **Arguments after `--` go to the program, unchanged and interpreted by
  nobody.** Not a single one is inspected: `mincc run p.mx -- -o --emit` passes
  two ordinary arguments to a program, because `run`'s job is not to be a second
  command line.
- **`argv[0]` is the program's path**, the temporary executable, exactly as if
  it had been invoked directly.
- **The exit status is the program's.** `run` exits with it, the way `sh -c`
  does, untranslated: a program that returns `2` makes `run` exit `2`, which is
  what lets `run` be the thing a test harness drives. A program that *did not
  exit* is not a status, and that is the next bullet.
- **"Did not exit" is a fact the platform is asked for, not a number that is
  interpreted.** The spawn belongs to `support/process`: `posix_spawn` +
  `waitpid` on one side, `CreateProcessW` + `GetExitCodeProcess` on the other, and
  the answer is three separate things — *started*, *exited*, *status*. The signal's
  number is deliberately not named, because naming it is `waitpid` on one platform
  and `WSTATUS` on the other; "did not exit normally" is the portable truth and all
  a user needs. This is the fourth file in the tree that contains a platform branch
  (`architecture.md`), beside the other three in `support`.
- **Why not `llvm::sys::ExecuteAndWait`, which does all of that already.**
  Because it *cannot* answer "started" and "status" separately: the Unix side of it
  gives a child whose `execve` failed `_exit(errno == ENOENT ? 127 : 126)`, and its
  `Wait` maps those two statuses back to `-1` with an error string — so a program
  that returned 127 and a program that never started arrive as the same answer. The
  statuses it maps are exactly the two a program is entitled to use, and this
  compiler used to report "cannot run" for a program whose `main` returned 127.
  Windows is lossy in a different place in the same layer (`status & 0xFF == 0` is
  answered with `1`, so a program exiting `256` is reported as exiting `1`). The fix
  is not to match on the error strings — those are prose, not an interface — it is
  to ask the platform, and `support/process/process.h` states the argument in full.
  What is *kept* from LLVM is the discovery of the linker driver
  (`llvm::sys::findProgramByName`, `PATHEXT` included): that is lookup, not spawn,
  and the platform's rules for it are worth inheriting.
- **The temporary executable is removed on every exit path.** The child is
  waited for before the directory is torn down, because the file is still being
  read by the kernel while the process starts.
- **`-g` composes with `run`**: `mincc run -g p.mx -- …` builds a binary with
  debug info, runs it, and leaves a usable `gdb` story for the same source,
  because it *is* a normal binary.

### Exit codes

The project's existing convention is kept and extended, because a command that
invents a new one is a command that breaks scripts:

| Code | Meaning |
| --- | --- |
| 0 | success |
| 1 | the program, the compiler, or the toolchain failed — a diagnostic was printed |
| 2 | the command line was wrong |

A program that did not exit normally is reported as a failure of the *program*,
with one sentence on stderr and status `1`. The shell's `128+n` is not used here,
and the reason is the rule above: `n` is the platform's answer, the number would
have to be fabricated from a status this stage refuses to interpret, and a
fabricated signal number is worse than a sentence that is always true.

`1` covering "a diagnostic was printed" is the existing rule and it stays: a
caller that needs to distinguish the compiler's failure from the *program's*
failure is looking at the wrong command, and inventing `3` for a link failure
would make the difference invisible in a shell and visible nowhere else.

## The three hosts, host by host

The point of this section is that it names what the driver has to do
*differently*, and finds that the list is short — because the linker driver
absorbs almost all of it, which is the argument for driving one.

### Linux (ELF)

- Linker driver: `clang` or `cc`. Startup objects, `libc`, the dynamic linker
  and `libgcc`/`compiler-rt` are the driver's.
- Relocation: PIC (above), so the host's default PIE works with no flag from us.
- Debug info: DWARF in the object; nothing extra to do at link.
- `-lm` and friends are ordinary `-l` arguments, forwarded unchanged.

### macOS (Mach-O)

- Linker driver: `clang` (the only supported one; `cc` is `clang`). The SDK and
  `-lSystem` are the driver's.
- Relocation: everything is PIC; the table says `PIC_`.
- Debug info: DWARF in the object, and **a `.dSYM` bundle is not produced by the
  link**. `dsymutil` is what a debugger uses to find the debug info for a
  stripped or relocatable-linked binary, and unlike `ld`, `clang` does not run
  it. So `-g` on macOS leaves the DWARF in the `.o` and the linker keeps what it
  keeps; the doc records that a `.dSYM` step is *the driver's to add* if the
  debugger story needs it, and that it is a separate program
  (`dsymutil`, which ships with LLVM and with Xcode) rather than a flag.
- One flag we must **not** pass: `-no-pie`. It does not exist here, and not
  needing it is the reason PIC is the decision above rather than a per-host
  switch.

### Windows (COFF)

- Linker driver: `clang` (which finds and drives `link.exe` from the Visual
  Studio installation) or `lld-link`. There is no `cc`.
- Relocation: `PIC_`, the same row the other two platforms read, and not "the
  platform's default" — see § *COFF: the same answer, for a sharper reason* for
  why the default is not linkable at `-O2`.
- Entry point: `main` resolves through the CRT, as on the others, because the
  driver supplies the entry shim.
- Debug info: **CodeView**, from the same metadata, chosen by the triple. The
  object carries `.debug$S`/`.debug$T` sections, and the **PDB is produced by the
  linker**, not by us — so `-g` on Windows means the link must be told to keep
  it (`/DEBUG`, and `/PDB:` if the user named one). That is a flag the driver
  adds and not a file the compiler writes, and the distinction is the same one
  as `.dSYM`: the artifact the debugger reads is the linker's, and pretending
  otherwise is how a tool ends up with a flag nobody's debugger honours.

The `-l`/`-L` spelling is spelled the same by `clang`, `gcc` and `ld`, and
`clang` accepts the Unix spellings on Windows and translates them for
`link.exe`, which is one more reason the search order puts `clang` first.

## Failure: every way this stage can refuse, and none of them is the program

The project's rule — `ir-unsupported-*` is a feature that is not built,
`ir-internal` is a bug in the compiler — extends here with a third category, and
naming it is the whole point of this section: **the environment.** A target LLVM
cannot emit, a linker nobody installed, a sysroot nobody provided. Those are
neither the program's fault nor the compiler's, and a message that blames the
wrong one is a message that wastes the reader's afternoon.

| Code | Class | Who fixes it |
| --- | --- | --- |
| `codegen-target-unavailable` | environment | install/rebuild LLVM with that backend, or choose another `--target` |
| `codegen-emit-unsupported` | environment | the target has no assembly printer for this file type |
| `codegen-linker-not-found` | environment | `--linker PATH`, or install a C toolchain |
| `codegen-linker-unavailable` | environment | linking a foreign triple needs `--linker` and `--sysroot` |
| `codegen-link-failed` | the program or the toolchain | the linker's own message, already printed |
| `driver-no-entry-point` | the program | write a `fn i32 main()` |
| `codegen-object-write-failed` | environment | permissions, disk, a read-only directory |
| `codegen-internal` | **this compiler** | a bug report, with the module dumped |

And the part that makes the promise "if the checker lets it pass, it must run"
more than a slogan: **there is no other way for this stage to fail.** A module
that reached this stage has been verified (`verifyModule`) and scanned against
the assumption list (`invariants.cc`), and the input has been type-checked with
no errors, so codegen has no *semantic* refusal to make. The interesting
consequence is what the list does to the way this stage is written: there is no
"unsupported construct" branch in it, and adding one would be admitting that
`ir` produced something it should not have. The only `codegen-internal` is
"LLVM returned a failure that is not one of the above", and it is a bug report
by construction.

## Tests

The stage is small and its environment is large, which is the opposite of the
usual shape and is exactly why the tests here are mostly *end-to-end* rather
than unit. They divide into four kinds, and the first is the one that has been
missing from this project since its first commit.

### 1. The end-to-end corpus: every example compiles, links, runs, and prints
what it should

`tests/e2e/` takes each `examples/*.mx`, builds it with the default target,
runs it, and asserts the exit status and the standard output against a
`// expect:` line in the file. This is the only test in the project that proves
the *whole* pipeline, and it is the one that would have caught the PIC problem,
the entry-point problem and the debug-record problem — not because it looks for
them, but because it does the thing a user does.

The corpus gains, with this stage, programs it could not have before: one that
returns its arithmetic through the exit status, one that loops, one that calls
`libc` (`printf`) — the first program in this language whose output is checked
against a reference rather than against a comment.

### 2. The differential oracle (`run` over `LLJIT`, in `tests/`)

The shape `ir.md` asked for, and the reason `LLJIT` stays despite `run` not
using it: for a subset of the corpus, the same program is written in C, both are
built, both are run, and the exit statuses and outputs must agree. This is the
test that turns the runtime contract from a statement about IR text into a
statement about arithmetic — `INT_MIN / -1` traps in both, a shift past the
width traps in both, `u16 <<= 9` is defined at `i32` in both — and it is the
only test that can disagree with the specification and be right.

### 3. The object, and the debug info in it

- **`--emit=obj` produces an object `llvm-nm`/`readelf` can read**, with `main`
  defined and the symbols a C program would have.
- **`-g` produces a line table that `llvm-dwarfdump` can walk**: the file, the
  function, a line, and a variable with a name and a size. Asserted on the
  *query* (`--lookup`/`--debug-line`) rather than on the bytes, because the
  bytes change with LLVM and this is a promise about what a tool can find.
- **`-g` changes no instruction.** Dump the module with and without, strip the
  debug info from the `-g` one, and compare — the test § *The conflict with the
  assumption scan* promises.
- **The module carries no `llvm.dbg.` intrinsic call**, the debug-records rule.
- **`gdb` is driven in batch mode** (`-batch -ex bt -ex 'print x'`) when it is
  present, and skipped with a message when it is not, so the test proves the
  thing a debugger needs rather than the thing the metadata looks like.

### 4. The refusals, one input per code

The table in § *Failure: every way this stage can refuse* is an enumeration, and
like `sema`'s and `ir`'s it has
a sweep: every code above has a test that produces it, and a code that stops
being reachable fails the sweep. Two of them are easy to reach with a temporary
`--linker` pointing at a program that exits non-zero; the target one is reached
by a triple this LLVM build does not have (which the test asks the build for,
rather than assuming).

## Decisions

| # | Decision | Why |
| --- | --- | --- |
| 1 | **`run` is `build` plus `exec`**, not an in-process JIT | JIT'd code runs inside the compiler: a crashing or `exit`ing program takes the compiler with it, and `run` would be a second implementation of the program's semantics that can disagree with `build` |
| 2 | **`LLJIT` is the test suite's differential oracle**, in `tests/`, governed by `ir.md`'s ORC rules | The oracle needs speed and no linker and *wants* in-process execution; a user-facing command needs the opposite. Same engine, opposite requirements |
| 3 | **The unit of codegen is one module, one `TargetMachine`, one thread** | A `TargetMachine` carries mutable emission state; sharing one across an editor's concurrent units is a race, and the rule is already `ir`'s rule for `LLVMContext` |
| 4 | **Optimise with the new PM, emit with the legacy one** | `addPassesToEmitFile` takes a `legacy::PassManagerBase&`; this is LLVM's own split and Clang's, and it is written down so a reader does not "fix" it |
| 5 | **`addPassesToEmitFile`'s inverted boolean and `DisableVerify=true` are wrapped once** | It returns true on *failure*, and it does *not* verify by default — two inversions, stated in one place instead of remembered in two |
| 6 | **Position-independent code on every platform, from a per-triple table** | Measured twice. ELF: this host's `cc` defaults to PIE and a static object links into `DT_TEXTREL` (a warning here, an error on other linkers and architectures). COFF: the small code model's 32-bit absolute address of a `.data` object is refused by `ld` (`relocation truncated to fit: IMAGE_REL_AMD64_ADDR32`), and the form only appears above `-O0`, so nothing local could see it. PIC links into a PIE, a non-PIE and a shared library, so we never pass `-no-pie` — a flag macOS does not have |
| 7 | **`build` drives a linker driver (`clang` → `cc` → `gcc`), never a linker** | Startup objects, `libc`, the dynamic linker, the SDK, the MSVC import libraries: platform knowledge that a C driver already has right, and reimplementing it is a second implementation with more ways to be wrong |
| 8 | **The child is spawned from an `argv` array, never a shell string** | No quoting to get wrong (including Windows paths with spaces), no injection, and the user's paths are passed as data because they are data |
| 9 | **Temporary objects live in a `createUniqueFile` directory owned by an RAII object** | `O_CREAT\|O_EXCL` makes the create atomic; a predictable name in `/tmp` is a symlink race, and cleanup on the error paths is the half that is usually forgotten |
| 10 | **No parsing of the linker's diagnostics** | Relaying the linker's own text is honest and free; parsing it is a second implementation of someone else's messages and a permanent maintenance cost |
| 11 | **A unit with no `main` is refused by us, before the link** | Three linkers spell this failure three unhelpful ways; the compiler already has the fact |
| 12 | **Linking a foreign triple is refused unless `--linker` and `--sysroot` are given** | Without a sysroot we would link a foreign object against the host's `libc` — a binary that does not work and does not say why. `--emit=obj` is the complete, honest way to cross-compile today |
| 13 | **Debug info is built in `ir` under `-g`; `codegen` only refuses to lose it** | The node and the instruction only coexist in the lowering; correlating them afterwards is a second walk over a correspondence that is already gone |
| 14 | **Debug *records*, never `llvm.dbg.*` intrinsics** | LLVM 19+ made records the default and the reference forbids mixing the two in one module; mixing produces a module that verifies and a debugger that lies |
| 15 | **`-g` sets `!llvm.dbg.cu`, `"Debug Info Version"=3`, `"Dwarf Version"=4`** | A consumer rejects metadata whose version flag is missing, and the failure message is about metadata rather than about the flag |
| 16 | **The assumption scan's rule becomes "no metadata except debug info"** | The list exists because metadata is an assumption the optimiser may exploit; `!dbg` licenses no transformation and says only where the code came from. A permit-list, not a deny-list, so the exception cannot widen silently |
| 17 | **`-g` changes no instruction**, and it is tested | It is what makes `-g` free to leave on, and it is the property a later lowering change is likeliest to break; strip and compare, rather than assert |
| 18 | **The compile unit's language is `DW_LANG_C99`** | A debugger picks its expression parser and type interpretation from that field, and `minc+` is a tighter C subset rather than a different language; a vendor string would send `gdb` to a minimal mode where our metadata is unreadable. One constant, written down, revisitable |
| 19 | **One refusal code per failure *class*, and the classes are environment / program / internal** | `ir.md`'s two classes (`unsupported` vs `internal`) plus the one this stage adds. A message that blames the wrong party wastes the reader's afternoon, and the third class is the one every other stage does not have |
| 20 | **There is no semantic refusal in this stage** | The promise is "if the checker lets it pass, it must run": `ir`'s module is verified and scanned, so a construct this stage cannot lower would mean `ir` emitted something it should not have — an internal error, not an unsupported feature |
| 21 | **The object is written by LLVM, the PDB by the linker, the `.dSYM` by `dsymutil`** | Which artifact a debugger reads differs by host, and a tool that pretends otherwise has a `-g` flag that no debugger honours |
| 22 | **A parameter's spill store carries no debug location** | LLVM places the line table's `prologue_end` flag on a function's first instruction that has a line, and a debugger reads that flag to decide where `break <function>` lands: a line on the spill puts every function breakpoint on the declaration, before the arguments have left their registers — `gdb` then prints `a=0 b=0` and stepping twice shows the same line. The parameter is not left without a line of its own: the `#dbg_declare` beside the store carries its span. `clang` emits the same store without a location, and the flag itself is worth having — it is what makes every debugger, not only this one, stop *after* the prologue |

## Non-goals

- **LTO and `-flto`.** They need the bitcode path (`--emit=bc`, `EmitBC`) and a
  linker that understands bitcode, and neither is a consequence of anything
  here. `ir.md` already lists this as a later decision.
- **`-static`, `--shared`, `-fPIC` as a choice.** PIC is now the only mode,
  which makes `-fPIC` a no-op and `-static`/`--shared` a decision about the
  *link*, not the object. They arrive when `src/cinterop` needs them, with the
  table they imply.
- **Writing our own linker or our own startup code.** `lld` 22.1.8 is installed
  on this machine and could be linked in as a library, and that is a *packaging*
  decision (it would remove the C-toolchain dependency for `build`) rather than
  a correctness one. It is not free: it moves "where is the target's `libc`"
  from the driver we are borrowing to us. Recorded as a real option, not taken.
- **`-g` at `-g3` (macro info), split DWARF, and `.dwo`.** Each is a real
  feature with a real cost and none is needed to debug a program in `gdb`.
- **`-O` beyond the six levels, and per-pass flags.** The levels are LLVM's
  pipelines; a flag that names one pass is a promise about LLVM's internals.
- **Incremental or cached builds.** The roadmap has it as an open question, and
  a cache is only worth having once a build is slow — which this one is not.
