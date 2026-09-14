// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The target machine: which one, and with which choices.
//
// Every argument to `createTargetMachine` is a decision this file states, and
// three of them are the reason it exists rather than being a call at the emit
// site:
//
// - **The CPU and the feature set are empty.** Not `"native"`, which would make
//   the object depend on the machine that happened to compile it. That is what a
//   triple is for; the host's feature set belongs to a JIT and this is not one.
// - **The relocation model comes from a table.** The default one emits the
//   object that a host with `enable-default-pie` links into `DT_TEXTREL` --
//   measured, not reasoned about (`codegen.md`, § *Position independence*).
// - **The code model is `Small`**, which is what an ordinary program is.
//
// And one thing it deliberately does *not* decide: the triple. It arrives from
// the module, because `sema` already answered it and a second answer here would
// be a second spelling of one target (`ir.md`, decision 21).
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"

#include "backend/codegen.h"
#include "backend_llvm.h"
#include "support/fs/fs.h"

namespace minc::backend {

void initializeTargets() {
  static std::once_flag once;
  std::call_once(once, [] {
    // Every target, not the native one, for the same reason `ir` registers
    // every one: `--target` names an architecture the host may not be, and a
    // cross build is an ordinary case here rather than a curiosity.
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    // The printers are the difference from `ir`'s guard: object and assembly
    // emission both go through the target's `AsmPrinter`, and a registry with
    // the code generator but not the printer builds a machine that cannot emit.
    llvm::InitializeAllAsmPrinters();
    // Parsers are not needed to emit, and are here because a module may contain
    // a module-level `asm` string the target has to parse. Registering them
    // costs nothing and removes a class of "works until someone writes asm".
    llvm::InitializeAllAsmParsers();
  });
}

std::optional<llvm::Reloc::Model> relocationModelFor(const sema::TargetInfo& target) {
  switch (target.triple.os) {
  case sema::OsFamily::linux:
  case sema::OsFamily::freebsd:
  case sema::OsFamily::darwin:
    // **Position-independent, always.** Every ordinary ELF and Mach-O host either
    // defaults to PIE or links PIC objects happily, and PIC links into a PIE, a
    // non-PIE *and* a shared library. A static object links into exactly one of
    // those and needs the driver to be told which -- and the object that says
    // "static" is the one that produces `DT_TEXTREL` on a distribution whose
    // `cc` defaults to `-pie`, which is all of them.
    return llvm::Reloc::PIC_;
  case sema::OsFamily::windows:
    // COFF's default, and what `clang` uses there: the dynamic base is a linker
    // setting on this platform, not a code-generation one, and asking for `PIC_`
    // would produce GOT-relative addressing the platform's linker does not
    // expect.
    return llvm::Reloc::Static;
  }
  return std::nullopt;
}

namespace {

// `-O` as the *backend* wants it, which is not the IR pipeline's level: the
// middle end is where `-O2` happens and this is what instruction selection and
// scheduling are allowed to do on top of it. The two being "the same number" is
// the whole of it, and the mapping is `clang`'s.
[[nodiscard]] llvm::CodeGenOptLevel machineLevel(unsigned level) {
  switch (level) {
  case 0:
    return llvm::CodeGenOptLevel::None;
  case 1:
    return llvm::CodeGenOptLevel::Less;
  case 2:
    return llvm::CodeGenOptLevel::Default;
  default:
    return llvm::CodeGenOptLevel::Aggressive;
  }
}

} // namespace

std::unique_ptr<llvm::TargetMachine> createMachine(const sema::TargetInfo& target,
                                                   unsigned optLevel, std::string& refusal) {
  initializeTargets();
  refusal.clear();

  const std::string& text = target.name();
  const llvm::Triple triple(text);
  if (triple.getArch() == llvm::Triple::UnknownArch) {
    refusal = "LLVM cannot parse the triple `" + text + "`";
    return nullptr;
  }

  std::string error;
  // Both `ir` and this stage call this, and both get the same `Target*`: the
  // registry is process-global and read-only once populated.
  const llvm::Target* registered = llvm::TargetRegistry::lookupTarget(triple, error);
  if (registered == nullptr) {
    // The distinction that matters: this LLVM build has no *code generator* for
    // the target. `ir` only needed the target's *info* to build a data layout,
    // which is why `mincc ir` can succeed where `mincc build` refuses.
    refusal = "this build of LLVM has no code generator for `" + text + "`" +
              (error.empty() ? std::string{} : ": " + error) +
              "; `llvm-config --targets-built` lists the ones it has";
    return nullptr;
  }

  const std::optional<llvm::Reloc::Model> relocation = relocationModelFor(target);
  if (!relocation.has_value()) {
    refusal = "this compiler states no relocation model for the platform of `" + text + "`";
    return nullptr;
  }

  llvm::TargetOptions options;
  // The CPU and the features are empty strings and not `"native"`: an object
  // that depends on the machine that compiled it is not a cross-compilable
  // object, and this is a compiler, not a JIT.
  std::unique_ptr<llvm::TargetMachine> machine(
      registered->createTargetMachine(triple, /*CPU=*/"", /*Features=*/"", options, relocation,
                                      llvm::CodeModel::Small, machineLevel(optLevel)));
  if (machine == nullptr) {
    refusal = "LLVM will not build a target machine for `" + text + "`";
    return nullptr;
  }
  return machine;
}

std::string defaultExecutableName(const sema::TargetInfo& target) {
  return target.triple.os == sema::OsFamily::windows ? "a.exe" : "a.out";
}

std::string defaultOutputPath(std::string_view input, EmitKind kind) {
  const std::string extension = kind == EmitKind::Assembly ? ".s" : ".o";
  return support::replaceExtension(std::string(input), extension);
}

bool targetAvailable(const sema::TargetInfo& target) {
  std::string refusal;
  return createMachine(target, /*optLevel=*/0, refusal) != nullptr;
}

std::string targetRefusal(const sema::TargetInfo& target) {
  std::string refusal;
  (void)createMachine(target, /*optLevel=*/0, refusal);
  return refusal;
}

} // namespace minc::backend
