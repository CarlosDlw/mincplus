// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The checked build's guards, and the runtime entry they fail into.
//
// `memory.md` states one obligation per access and then says something unusual
// about violating one: the *access* leaves the model rather than the program
// becoming arbitrary. This file is the compiler watching its own program. It
// emits a test in front of an access, and when the test fails it prints the site
// and stops -- which is the difference between a compiler that is fast on the
// paths that are correct and one that is safe on the paths that are not.
//
// **The guard is read from the access record, never derived here.** Every check
// below is a *materialisation* of a field `sema` wrote at the same node
// (`sema::AccessObligation`), and there are exactly two callers -- `loadPlace` and
// `storePlace` -- so there is no path by which an access reaches the module
// unguarded, and no rule in this file that a reader has to check against the
// model. `checks.md` is the design record; `ir.md` § *The checked build's guards*
// is the table this implements, row for row.
//
// Three things are decisions rather than mechanics, and each is in the record:
// the null guard is emitted for every obligation even where the address is
// provably not null (a checker that deletes the checks it can prove stops
// reporting the day the proof has a bug), the bounds comparison is **unsigned** so
// one instruction catches both ends of the range, and the failure edge prints a
// message the compiler built at compile time rather than calling a formatting
// routine at run time.
#include "lowering.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "llvm/ADT/Twine.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/TargetParser/Triple.h"

#include "sema/typed_ast.h"
#include "support/line/line_col.h"
#include "support/source/source_file.h"
#include "support/source/source_manager.h"

namespace minc::ir {
namespace {

// The name of the module's own runtime entry. A leading double underscore and a
// name no language identifier can spell, and `internal` linkage on top of that:
// the checked build adds nothing to the program's namespace.
constexpr const char* kCheckFailName = "__minc_check_fail";

// The rule names, which are the codes `memory.md` gives the violations and the
// words a reader greps for. One row per guard below, and each is spelled the same
// way in the diagnostics of every other stage -- `memory-null` is a *name*, not a
// sentence, because the sentence is the message's job.
constexpr std::string_view kNullRule = "memory-null";
constexpr std::string_view kMisalignedRule = "memory-misaligned";
constexpr std::string_view kOutOfBoundsRule = "memory-out-of-bounds";

// `write` on every platform but Windows, `_write` on Windows: the same operation,
// and the two names are what the C runtime exports. Chosen from the module's own
// triple -- the single source of truth for the target everywhere else in this
// stage -- and never from a preprocessor macro, because this translation unit is
// compiled once for the host and emits for any target.
[[nodiscard]] bool isWindows(llvm::Module& module) {
  return llvm::Triple(module.getTargetTriple()).isOSWindows();
}

} // namespace

std::string Lowering::checkMessage(std::string_view rule, support::Span span) const {
  // The path comes from the *span's* file and not from the unit's, so a guard
  // inside an included header names the header: the reader is being told where
  // the access is written, and a macro-expanded access is the case where those
  // two differ. `SourceManager` is what answers it, and a caller that has none
  // (a unit test) falls back to the unit's own path.
  std::string_view path = options_.source != nullptr ? options_.source->path : std::string_view{};
  const support::SourceFile* file =
      options_.sources != nullptr && span.valid() ? options_.sources->find(span.file) : nullptr;
  if (file != nullptr) {
    path = file->path;
  }
  std::string message = "mincc: trap: ";
  message += rule;
  if (span.valid() && file != nullptr) {
    const support::LineCol position = file->lookup(span.begin);
    // A line of 0 is not a position anybody wrote: the span's file was found and
    // the offset resolved, or this arm is not reached at all.
    message += " at ";
    message += path;
    message += ':';
    message += std::to_string(position.line);
    message += ':';
    message += std::to_string(position.col);
  } else if (!path.empty()) {
    message += " at ";
    message += path;
  }
  message += '\n';
  return message;
}

llvm::GlobalVariable* Lowering::checkMessageGlobal(const std::string& text) {
  const auto existing = messages_.find(text);
  if (existing != messages_.end()) {
    return existing->second;
  }
  // A private array of bytes and its NUL, **not** `constant`.
  //
  // `memory.md` decision 15 forbids `constant` on a file-scope object, because
  // LLVM's flag says "nothing writes this" and a write through a pointer to a
  // constant object is undefined behaviour rather than a diagnostic. This object
  // is written once by the compiler and is reachable only from the call below, so
  // the flag would happen to be true -- and an invariant that holds *by accident*
  // is what the scan that caught this object the first time exists to refuse. The
  // rule is absolute, the compiler pays a byte of `.data` instead of `.rodata` per
  // site, and nothing here has to be argued about again.
  //
  // The name is not the text: a symbol named after a message is a symbol whose
  // name changes whenever the message is edited.
  llvm::Constant* bytes = llvm::ConstantDataArray::getString(context_, text, /*AddNull=*/true);
  auto* global = new llvm::GlobalVariable(module_, bytes->getType(), /*isConstant=*/false,
                                          llvm::GlobalValue::PrivateLinkage, bytes, "check.site");
  messages_.emplace(text, global);
  return global;
}

void Lowering::ensureCheckRuntime() {
  if (checkFail_ != nullptr) {
    return;
  }
  const bool windows = isWindows(module_);

  // The C library's two functions, declared rather than assumed to exist: a
  // module that references a symbol it did not declare does not verify.
  //
  // `fflush(ptr null)` is ISO C's \"flush every output stream\", and it is there so
  // the program's own buffered output is not lost behind the trap -- a reader
  // looking at a message wants the lines their program printed first.
  llvm::FunctionCallee flush =
      module_.getOrInsertFunction("fflush", llvm::FunctionType::get(indexType(), {pointerType()},
                                                                    /*isVarArg=*/false));
  // `write` takes the buffer length as `size_t` on a POSIX target and as
  // `unsigned int` on Windows, and returns the count written -- `ssize_t` there,
  // `int` here. Both are read from the target's `int` and pointer widths rather
  // than written as `i64`, which is what keeps a 32-bit target's ABI right.
  llvm::FunctionCallee write =
      windows
          ? module_.getOrInsertFunction(
                "_write", llvm::FunctionType::get(llvm::Type::getInt32Ty(context_),
                                                  {llvm::Type::getInt32Ty(context_), pointerType(),
                                                   llvm::Type::getInt32Ty(context_)},
                                                  /*isVarArg=*/false))
          : module_.getOrInsertFunction("write",
                                        llvm::FunctionType::get(indexType(),
                                                                {llvm::Type::getInt32Ty(context_),
                                                                 pointerType(), indexType()},
                                                                /*isVarArg=*/false));

  // The entry the failure edges call: flush, write the site to descriptor 2, then
  // the language's trap. `llvm.trap` and not `abort` or `exit`: it is what the
  // value traps already use (`runtime.cc`), it is what a debugger stops on with
  // the frame intact, and it cannot be confused with the program choosing to stop.
  llvm::FunctionType* signature =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context_), {pointerType(), indexType()},
                              /*isVarArg=*/false);
  checkFail_ = llvm::Function::Create(signature, llvm::GlobalValue::InternalLinkage, kCheckFailName,
                                      module_);
  llvm::BasicBlock* entry = llvm::BasicBlock::Create(context_, "entry", checkFail_);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(flush, {llvm::ConstantPointerNull::get(pointerType())});
  llvm::Value* length = checkFail_->getArg(1);
  if (windows) {
    // The third operand of `_write` is 32 bits, and the message length is a
    // compile-time constant that fits one: truncating it here is exact and is
    // what the ABI wants.
    length = builder.CreateTrunc(length, llvm::Type::getInt32Ty(context_), "trap.len");
  }
  builder.CreateCall(write, {llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 2),
                             checkFail_->getArg(0), length});
  builder.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(&module_, llvm::Intrinsic::trap));
  builder.CreateUnreachable();
}

void Lowering::guardBranch(llvm::Value* bad, const std::string& message) {
  if (bad == nullptr) {
    return;
  }
  ensureCheckRuntime();
  llvm::GlobalVariable* text = checkMessageGlobal(message);
  // The *characters*, not the object: the array carries the message's NUL and
  // writing that byte to descriptor 2 puts a stray byte in the output.
  const std::uint64_t length = message.size();

  // **A condition that folded to a constant is answered by what it folded to.**
  // `icmp eq ptr @TABLE, null` is `false` -- LLVM's own constant folder, not a
  // proof this stage made -- and a `br i1 false` with a trap block behind it is
  // dead code that a reader would have to reason about. The other direction is a
  // guard that always fires, and there the trap is emitted straight: the access
  // behind it is unreachable, which is a *correct* module for a program that
  // dereferences a null constant, and the scan's constant-address escape is why it
  // is not mistaken for a missing guard (`invariants.cc`).
  if (auto* constant = llvm::dyn_cast<llvm::ConstantInt>(bad)) {
    if (constant->isZero()) {
      return;
    }
    emitCheckFail(text, length);
    // The access is still lowered, into a block nothing branches to: the guard
    // says it can never be reached, and *dropping* the access would mean an
    // expression's code vanishing where a reader cannot see it happen. LLVM
    // accepts the dead block and the optimizer deletes it.
    builder_.SetInsertPoint(llvm::BasicBlock::Create(context_, "check.dead", current_));
    return;
  }

  llvm::BasicBlock* trap = llvm::BasicBlock::Create(context_, "check.trap", current_);
  llvm::BasicBlock* ok = llvm::BasicBlock::Create(context_, "check.ok", current_);
  builder_.CreateCondBr(bad, trap, ok);
  builder_.SetInsertPoint(trap);
  emitCheckFail(text, length);
  // The access continues in the block the passing edge enters, which is what makes
  // this a *guard*: the guarded instruction is reached only through the test, and
  // `invariants.cc` reads exactly that shape back out of the finished module.
  builder_.SetInsertPoint(ok);
}

void Lowering::emitCheckFail(llvm::GlobalVariable* message, std::uint64_t length) {
  builder_.CreateCall(checkFail_, {message, llvm::ConstantInt::get(
                                                indexType(), static_cast<std::uint64_t>(length))});
  builder_.CreateUnreachable();
  // The insert point is deliberately **not** moved here: the conditional path has
  // a passing block to move to, and a block nothing branches to left behind on
  // that path would be the kind of noise that makes a reader stop reading the
  // module. `guardBranch` is the one place that knows which kind of ending it is
  // looking at.
}

void Lowering::guardAccess(const Place& place, const sema::AccessObligation& obligation,
                           support::Span at) {
  if (!checksEnabled() || place.addr == nullptr) {
    return;
  }
  // --- null ------------------------------------------------------------------
  //
  // Emitted for every obligation, including one whose address is provably not
  // null, which `ir.md` argues for and this file's header states again: the guard
  // is read from the record and not from a proof, and the release build is what
  // takes all of them away at once.
  llvm::Value* isNull = builder_.CreateICmpEQ(
      place.addr, llvm::ConstantPointerNull::get(pointerType()), "check.null");
  guardBranch(isNull, checkMessage(kNullRule, at));

  // --- alignment -------------------------------------------------------------
  //
  // The alignment is the *access type's*, read from the store, which is the same
  // number the load or store below states -- so a guard cannot disagree with the
  // instruction it guards about what \"aligned\" means. An alignment of one byte is
  // every address, so there is nothing to test.
  if (place.addr->getType()->isPointerTy()) {
    const std::uint64_t alignment = alignmentOf(obligation.type);
    if (alignment > 1) {
      llvm::Value* address = builder_.CreatePtrToInt(place.addr, indexType(), "check.addr");
      llvm::Value* low = builder_.CreateAnd(
          address, llvm::ConstantInt::get(indexType(), alignment - 1), "check.low");
      llvm::Value* misaligned =
          builder_.CreateICmpNE(low, llvm::ConstantInt::get(indexType(), 0), "check.misaligned");
      guardBranch(misaligned, checkMessage(kMisalignedRule, at));
    }
  }

  // --- bounds ----------------------------------------------------------------
  //
  // Only where the record has an extent *and* the place produced the two values:
  // an index below a length. The comparison is **unsigned**, so a negative index --
  // which is a very large unsigned number -- is caught by the same instruction as
  // an index past the end; a signed comparison plus a sign test would be two
  // instructions that can disagree.
  if (place.bounds.has_value() && place.bounds->index != nullptr &&
      place.bounds->extent != nullptr) {
    llvm::Value* outside =
        builder_.CreateICmpUGE(place.bounds->index, place.bounds->extent, "check.outside");
    guardBranch(outside, checkMessage(kOutOfBoundsRule, at));
  }
}

} // namespace minc::ir
