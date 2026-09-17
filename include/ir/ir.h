// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// IR: a typed unit in, an LLVM module out.
//
// The stage contract is the one every stage above follows -- an artifact in, an
// artifact out, errors as values -- with two differences that shape this header.
//
// **The module is LLVM's, and this header does not show it.** The stages that
// *use* the lowering (the driver above all) must not link LLVM: the isolation
// rule is "nothing up to and including `sema`", and a rule that a consumer can
// break by including a header is not a rule. So the artifact is opaque here --
// a handle that owns the `LLVMContext`, the `DataLayout` and the `Module` -- and
// the two things a consumer legitimately wants are free functions in their own
// headers: `dump.h` prints it, `invariants.h` scans it.
//
// **The lowering decides nothing.** It reads `sema`'s answers -- the type of
// every node, the conversion at every operand, the operation type of a compound
// assignment, and one access obligation per dereference -- and materialises
// them. A decision it cannot read is a refusal (`ir-internal`,
// `ir-missing-obligation`), not a guess, because a guess here is a second copy of
// a rule that lives one stage up and the two copies are what disagree.
//
// Design record: `docs/architectures/ir.md`; the memory rules it emits are
// `docs/architectures/memory.md`.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ast/ast.h"
#include "resolve/map.h"
#include "sema/type_store.h"
#include "sema/typed_ast.h"
#include "support/intern/interner.h"
#include "support/source/source_file.h"
#include "support/source/source_manager.h"
#include "support/span/span.h"

namespace minc::ir {

// The handle's innards: the `LLVMContext`, the `DataLayout` and the `Module`,
// owned together. Declared here only so `Module` can hold a `unique_ptr` to it;
// the *definition* lives in `src/ir/storage.h`, which includes LLVM -- so a
// consumer of this header still cannot see an LLVM type.
struct ModuleStorage;
struct ModuleAccess;

// Every way this stage can refuse, as a stable code. One table
// (`allDiagnosticCodes`) is the enumeration, so a code added without one is
// caught by a test rather than by a user.
//
// The two halves matter and are never the same message: `Unsupported*` is a
// feature this compiler has not built yet, and the rest are *this compiler is
// wrong*. They have opposite fixes -- one wants a feature, the other a bug
// report -- and a compiler that reports both the same way teaches its readers to
// mistrust both.
enum class IRDiagnosticCode : std::uint8_t {
  // A construct the language has that this stage does not lower yet. The
  // refusal names it, and there is a test input per code.
  UnsupportedType,
  UnsupportedNode,
  // LLVM has no rules for the target this unit was checked against, so a
  // `DataLayout` cannot be built and every size and alignment would be a guess.
  UnsupportedTarget,
  // A violated precondition: a missing type, a deferred type, a node the checker
  // never reached. A program that type-checked cannot produce one.
  Internal,
  // An access through a pointer with no recorded obligation. Also an internal
  // error, and named apart from `Internal` because the fix is specific: the
  // checker stopped recording where it used to.
  MissingObligation,
  // The module carries an assumption the language did not state (a metadata
  // node, a function attribute, an `inbounds` without a proof, a file-scope
  // object emitted `constant`). Reported by the scan, not by the lowering -- the
  // lowering has no way to emit one.
  Assumption,
  // An emitted alignment -- an access's, or a file-scope object's -- is not the
  // one its type requires. Overestimating it is undefined behaviour in LLVM, not
  // slow code.
  Alignment,
  // An operation the language defines as a trap was emitted bare.
  UnguardedOp,
  // A float → integer conversion of a *constant* whose value is outside the
  // destination's range. The conversion traps when the value is not
  // representable, and a file-scope object has no instruction to trap from -- so
  // this is a refusal and not a folded poison (`casts.md`).
  CastOutOfRange,
  // An object this function would place in its own frame is larger than
  // `kMaxStackObjectBytes`. A limit and not a bug: the frame is a subtraction
  // from the stack pointer, and the failure without the limit is a segfault at
  // the first instruction of the function. The sentence names the size, the
  // bound, and the place that asked for the slot.
  ObjectTooLarge,
  // A file-scope initializer whose bytes this compiler will not write out one
  // element at a time (`kMaxInitializerBytes`). A limit and not a bug: the row it
  // exists for is a non-zero fill, whose count is a number in the type. A zero
  // fill never reaches it -- that one is a single constant of any count.
  InitializerTooLarge,
};

struct IRDiagnosticCodeInfo {
  IRDiagnosticCode code;
  const char* name;
};

[[nodiscard]] std::string_view toString(IRDiagnosticCode code);
[[nodiscard]] const char* nameOf(IRDiagnosticCode code);
// Every code, derived from the table above.
[[nodiscard]] const std::vector<IRDiagnosticCode>& allDiagnosticCodes();

// One refusal. `span` is where it was written, so the renderer points at the
// bytes the reader has to change; an empty span means the diagnostic is about
// the unit as a whole.
struct IRDiagnostic {
  support::Span span;
  IRDiagnosticCode code = IRDiagnosticCode::Internal;
  std::string message;
};

// The lowered module, owned. Opaque on purpose: `ir.h` is included by stages
// that must not see LLVM, so the context, the layout and the module live behind
// one pointer and none of their types appear here.
//
// It owns its `LLVMContext` and not just the module, because an LLVM type is
// owned by the context that made it: a module outliving its context is a
// use-after-free that would show up as garbage in a dump. One unit, one context,
// one handle -- which is also what makes two units safe to lower on two threads.
class Module {
public:
  Module();
  ~Module();
  Module(Module&&) noexcept;
  Module& operator=(Module&&) noexcept;
  Module(const Module&) = delete;
  Module& operator=(const Module&) = delete;

  // False for a default-constructed handle and for one the lowering abandoned.
  // A caller must check it before `dumpModule`, and the driver treats "no
  // module" as part of the failure: a half-built module that reached a linker is
  // the outcome this whole stage is arranged to prevent.
  [[nodiscard]] bool built() const;

  // The identifier of the file the module was built for, so a caller can name
  // the unit a handle belongs to without keeping the `LoweredFile` alive too.
  [[nodiscard]] support::FileId file() const;

private:
  friend class Lowering;
  // The only way to reach the `llvm::Module` from outside the lowering. A
  // separate type and not public accessors, because giving `Module` a
  // `llvm::Module&` member function would put LLVM in a header that stages
  // above this one link -- which is the isolation rule, and a rule a header can
  // break is not a rule.
  friend struct ModuleAccess;
  std::unique_ptr<ModuleStorage> impl_;
};

struct IRResult {
  // Empty when a module was built. A unit with any diagnostic produces **no
  // module**: `check`'s rule, one stage down, and the reason `mincc ir` can
  // never hand a broken module to anything.
  std::vector<IRDiagnostic> diagnostics;
  Module module;

  [[nodiscard]] bool failed() const {
    return !diagnostics.empty();
  }
};

// What the lowering needs beyond the artifact it consumes.
//
// One field, `debugInfo`, and its inputs. The source file is handed over rather
// than reached for because the lowering has the *spans* -- byte offsets into a
// revision of a file -- and not the line table or the path, which live in the
// session's source table; and `src/ir` may not depend on the driver to find
// them. A `SourceFile*` rather than a path and a line table because the three
// things the debug info needs (the id, the path, and the offset-to-line
// mapping) are one object, and handing over three of its fields invites them to
// disagree.
//
// It is an *option* and not a parameter because `-g` is the only thing that
// changes here, and a default of "no debug info" is what every test and every
// `mincc ir` invocation wants.
struct LoweringOptions {
  // Off unless the driver read `-g`. Metadata is nodes in the module and a cost
  // in every pass, and a build that did not ask for a debugger's benefit should
  // not pay for it.
  bool debugInfo = false;
  // The **checked build**: the memory model's diagnostic half, where every access
  // that reaches memory through a pointer is guarded against null, against the
  // alignment its type requires, and -- where the access record has an extent --
  // against an index outside it (`docs/architectures/checks.md`).
  //
  // The driver turns it on for `-O0` and for `-fcheck`, and off for `-fno-check`
  // and every optimised build, which is the shape `memory.md` decision 19 asks
  // for: a language that cannot stop checking is unshippable, and one that never
  // checks has an aspirational model.
  bool checks = false;
  // The unit's main file. Read by the debug info under `-g`, and by the *site
  // messages* of the checked build's guards either way -- a guard that fires has
  // to say where, and that is a fact about the source rather than about `-g`.
  const support::SourceFile* source = nullptr;
  // The compilation's files, so a guard inside an included file names that file
  // and not the unit's. Null is allowed and means "the unit's own file only",
  // which is what a caller that has no `SourceManager` (a test) passes.
  const support::SourceManager* sources = nullptr;
  // `DW_AT_producer`, so a debugger (and `readelf --debug-dump`) can say which
  // compiler produced the line table.
  std::string producer = "minc+";
};

// Lowers one checked translation unit.
//
// Precondition, enforced rather than trusted: `typed` is the typing of `file`
// with no errors. A tree that carries the poison is refused with `ir-internal`
// and no module is built, because `TypeKind::Error` converts silently and a
// poisoned tree would otherwise lower to well-formed IR for a program whose
// meaning nobody decided.
//
// `types` is the compilation's store (so a type means the same thing here as it
// did one stage up) and `defs` is the resolution of the same unit, which is what
// ties a name *use* to the declaration an `alloca` was made for. `symbols` is
// the interner those ids came from.
// The store is mutable for the same reason `sema::Context::types()` is: lowering a
// generic body once per instance applies the instance's substitution as it reads a
// type, and a substitution interns the result.
[[nodiscard]] IRResult lowerUnit(const ast::LoweredFile& file, const resolve::DefMap& defs,
                                 const sema::TypedFile& typed, sema::TypeStore& types,
                                 const support::Interner& symbols,
                                 const LoweringOptions& options = {});

} // namespace minc::ir
