// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// What the backend's own translation units share, and nothing a caller sees.
//
// Three things live here and each has a reason:
//
// - **The initialization guard.** LLVM's targets live in static registries a
//   linker may discard, so a program that uses LLVM registers them itself. `ir`
//   has its own guard for the *infos* it needs to build a data layout; this one
//   registers the printers as well, because object emission goes through them.
//   Two guards and not one shared module, because `ir` may not depend on
//   `codegen` (`ir.md`, decision 13) and a shared module below `sema` would put
//   `llvm/*` under the boundary. Registration is idempotent, so the redundancy
//   costs one function call per process.
//
// - **The relocation model.** One table, read in one place, so the answer cannot
//   depend on which LLVM default happened to apply (`codegen.md`, decision 6).
//
// - **The `TargetMachine` factory.** One function, so the CPU, the features, the
//   code model and the relocation model are stated once and every path that
//   needs a machine gets the same one.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "llvm/Target/TargetMachine.h"

#include "backend/codegen.h"
#include "sema/target.h"

namespace minc::backend {

// Registers LLVM's targets once per process. Safe to call from any thread and
// from any number of places.
void initializeTargets();

// The machine for a triple, or nothing with `refusal` filled in. The refusal is
// a sentence, not a code: the caller turns it into whichever diagnostic the
// context calls for (a probe, a refusal, a message).
[[nodiscard]] std::unique_ptr<llvm::TargetMachine>
createMachine(const sema::TargetInfo& target, unsigned optLevel, std::string& refusal);

// The relocation model for a triple, from the table. `std::nullopt` for a triple
// whose platform this compiler does not state -- which cannot happen for a
// `TargetInfo`, because `sema` refused it before one existed.
[[nodiscard]] std::optional<llvm::Reloc::Model> relocationModelFor(const sema::TargetInfo& target);

} // namespace minc::backend
