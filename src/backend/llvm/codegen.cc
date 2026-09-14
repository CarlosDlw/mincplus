// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The emit sequence, and the two pass managers that make it up.
//
// The split is LLVM's and it is written down here precisely so nobody "fixes"
// it: the **middle end** is the new pass manager, driven by `PassBuilder`, and
// the **codegen** pipeline is the legacy one, because `TargetMachine::
// addPassesToEmitFile` takes a `legacy::PassManagerBase&`. They are sequenced,
// never mixed. Clang does exactly this.
//
// Three traps of the emission call are wrapped once each, in this file, because
// all three are inversions or defaults that read as their opposite:
//
// - `addPassesToEmitFile` returns **true on failure** ("emission of this file
//   type is not supported"), the opposite of every other boolean here.
// - Its `DisableVerify` parameter defaults to **`true`** -- LLVM's machine
//   verifier is off unless asked for. This project asks.
// - `verifyModule` also returns **true on failure** (`ir.md` already wraps that
//   one; the polarity is restated here because this file calls it again).
#include <cstdint>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#include "backend/codegen.h"
#include "backend_llvm.h"
#include "ir/storage.h"
#include "sema/target.h"

namespace minc::backend {
namespace {

void add(std::vector<CodegenDiagnostic>& out, CodegenDiagnosticCode code, std::string message) {
  CodegenDiagnostic diagnostic;
  diagnostic.code = code;
  diagnostic.message = std::move(message);
  out.push_back(std::move(diagnostic));
}

// `-O` as the *pipeline* wants it. `OptimizationLevel` is a class and not an
// enum (the same letter is a different object at a different level), which is
// why the mapping is a function and not a cast.
[[nodiscard]] llvm::OptimizationLevel pipelineLevel(OptLevel level) {
  switch (level) {
  case OptLevel::O0:
    return llvm::OptimizationLevel::O0;
  case OptLevel::O1:
    return llvm::OptimizationLevel::O1;
  case OptLevel::O2:
    return llvm::OptimizationLevel::O2;
  case OptLevel::O3:
    return llvm::OptimizationLevel::O3;
  case OptLevel::Os:
    return llvm::OptimizationLevel::Os;
  case OptLevel::Oz:
    return llvm::OptimizationLevel::Oz;
  }
  return llvm::OptimizationLevel::O0;
}

[[nodiscard]] unsigned machineLevelOf(OptLevel level) {
  switch (level) {
  case OptLevel::O0:
    return 0;
  case OptLevel::O1:
    return 1;
  case OptLevel::O2:
  case OptLevel::Os:
    return 2;
  case OptLevel::O3:
  case OptLevel::Oz:
    return 3;
  }
  return 0;
}

// The middle end. New pass manager, target callbacks registered (skipping
// `registerPassBuilderCallbacks` compiles fine and silently omits the target's
// own passes), and the O0 pipeline named apart because LLVM spells it apart.
void optimize(llvm::Module& module, llvm::TargetMachine& machine, OptLevel level) {
  llvm::LoopAnalysisManager loopAnalyses;
  llvm::FunctionAnalysisManager functionAnalyses;
  llvm::CGSCCAnalysisManager cgsccAnalyses;
  llvm::ModuleAnalysisManager moduleAnalyses;

  llvm::PassBuilder builder(&machine);
  builder.registerModuleAnalyses(moduleAnalyses);
  builder.registerCGSCCAnalyses(cgsccAnalyses);
  builder.registerFunctionAnalyses(functionAnalyses);
  builder.registerLoopAnalyses(loopAnalyses);
  builder.crossRegisterProxies(loopAnalyses, functionAnalyses, cgsccAnalyses, moduleAnalyses);
  machine.registerPassBuilderCallbacks(builder);

  const llvm::OptimizationLevel pipeline = pipelineLevel(level);
  llvm::ModulePassManager manager = level == OptLevel::O0
                                        ? builder.buildO0DefaultPipeline(pipeline)
                                        : builder.buildPerModuleDefaultPipeline(pipeline);
  manager.run(module, moduleAnalyses);
}

// True when the module still verifies. `verifyModule` returns true on *failure*.
[[nodiscard]] bool verifies(const llvm::Module& module, std::string& report) {
  std::string text;
  llvm::raw_string_ostream stream(text);
  const bool broken = llvm::verifyModule(module, &stream, /*BrokenDebugInfo=*/nullptr);
  stream.flush();
  report = std::move(text);
  return !broken;
}

} // namespace

EmitResult emitModule(ir::Module& module, const EmitOptions& options) {
  EmitResult result;

  // The preconditions, enforced rather than trusted. Both are caller bugs: the
  // driver never asks for an emit with no output and never holds a module the
  // lowering abandoned ("no module" is part of `ir`'s contract).
  if (!module.built()) {
    add(result.diagnostics, CodegenDiagnosticCode::Internal,
        "codegen was handed a module that was never built");
    return result;
  }
  if (options.kind != EmitKind::None && options.outputPath.empty()) {
    add(result.diagnostics, CodegenDiagnosticCode::Internal,
        "codegen was asked to write a file with no path");
    return result;
  }

  llvm::Module& llvmModule = ir::ModuleAccess::llvmModule(module);

  // The target is the module's, and it is read rather than passed: `sema`
  // decided it, `ir` wrote it into the module, and a second parameter here would
  // be a second spelling of the same target with a chance to disagree.
  //
  // `getTargetTriple()` returns LLVM's `Triple` and not a string in LLVM 22, so
  // the spelling is taken once and used for both the lookup and the message.
  const std::string tripleName = llvmModule.getTargetTriple().str();
  const std::optional<sema::TargetInfo> target = sema::targetFromName(tripleName);
  if (!target.has_value()) {
    add(result.diagnostics, CodegenDiagnosticCode::Internal,
        "the module names the triple `" + tripleName +
            "`, which this compiler does not state; `ir` and `codegen` disagree about the target");
    return result;
  }

  std::string refusal;
  std::unique_ptr<llvm::TargetMachine> machine =
      createMachine(*target, machineLevelOf(options.level), refusal);
  if (machine == nullptr) {
    add(result.diagnostics, CodegenDiagnosticCode::TargetUnavailable, refusal);
    return result;
  }

  if (options.verbose) {
    result.trace = "target=" + target->name() + " opt=-" + std::string(toString(options.level)) +
                   " emit=" + toString(options.kind);
    if (options.kind != EmitKind::None) {
      result.trace += " output=" + options.outputPath;
    }
  }

  optimize(llvmModule, *machine, options.level);

  // After the pipeline, not only before it: `ir` verified the module it built,
  // and this proves the optimiser did not turn it into something else. A failure
  // here is this compiler being wrong, and the report is IR the user never wrote.
  std::string report;
  if (!verifies(llvmModule, report)) {
    add(result.diagnostics, CodegenDiagnosticCode::Internal,
        "the optimised module does not verify: " + report);
    return result;
  }

  if (options.kind == EmitKind::None) {
    return result;
  }

  std::error_code error;
  llvm::raw_fd_ostream stream(options.outputPath, error, llvm::sys::fs::OF_None);
  if (error) {
    add(result.diagnostics, CodegenDiagnosticCode::ObjectWriteFailed,
        "cannot write `" + options.outputPath + "`: " + error.message());
    return result;
  }

  const llvm::CodeGenFileType fileType = options.kind == EmitKind::Assembly
                                             ? llvm::CodeGenFileType::AssemblyFile
                                             : llvm::CodeGenFileType::ObjectFile;

  llvm::legacy::PassManager manager;
  // `false` for `DisableVerify`: LLVM's default is to *not* run the machine
  // verifier, and this project asks for it. A malformed instruction selection
  // should be a diagnostic here rather than a miscompile two stages down.
  if (machine->addPassesToEmitFile(manager, stream, /*DwoOut=*/nullptr, fileType,
                                   /*DisableVerify=*/!options.verifyMachineCode)) {
    add(result.diagnostics, CodegenDiagnosticCode::EmitUnsupported,
        "this LLVM has no " + std::string(toString(options.kind)) + " printer for `" +
            target->name() + "`");
    return result;
  }
  manager.run(llvmModule);
  stream.flush();

  // The stream reports its own failures: a full disk is not an exception here,
  // and an unchecked write is how a truncated object reaches a linker.
  if (stream.has_error()) {
    const std::error_code streamError = stream.error();
    add(result.diagnostics, CodegenDiagnosticCode::ObjectWriteFailed,
        "cannot write `" + options.outputPath + "`: " + streamError.message());
    return result;
  }

  return result;
}

void initialize() {
  initializeTargets();
}

} // namespace minc::backend
