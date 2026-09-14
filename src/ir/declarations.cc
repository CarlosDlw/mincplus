// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The function signatures, and the names the linker sees.
//
// Every function is declared before any body is lowered, which is the same
// decision `sema::runSignatures` makes one stage up and for the same reason: a
// body may call a function written after it, so a call must find a `Function*`
// that already exists rather than one it has to create. The pass is therefore
// *all* signatures first, then *all* bodies (`lower.cc`), and the two loops are
// the shape of that.
//
// A declaration contributes a `declare` and nothing else: `extern fn` is how the
// language writes "this symbol is defined somewhere this compiler is not looking",
// and the linker is the stage that resolves it. An `extern` declaration that is
// never called emits no symbol at all, because LLVM drops a declaration nothing
// references -- which is why declaring a function the program never uses costs
// nothing and needs no bookkeeping here.
#include "lowering.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"

#include "sema/typed_ast.h"

namespace minc::ir {

std::string Lowering::linkageName(resolve::DefId def) const {
  if (!def.valid() || def.index >= defs_.defs.size()) {
    return {};
  }
  const support::SymId name = defs_.defs[def.index].name;
  if (name == support::kInvalidSym) {
    return {};
  }
  return std::string(symbols_.lookup(name));
}

void Lowering::declareFunctions() {
  for (const sema::FunctionInfo& info : typed_.functionTable) {
    if (failed_) {
      return;
    }
    if (!info.decl.valid() || inError(info.decl)) {
      continue;
    }
    const ast::AstId nameNode = childOf(info.decl, ast::NodeKind::Name);
    const std::optional<resolve::DefId> def = defAtName(nameNode);

    // The name, from the declaration when it resolves and from the interned
    // signature otherwise. Two sources rather than one because the fallback is
    // what makes a name *always* printable: a diagnostic and a symbol both need
    // to spell the function, and a stage that gave up on an unresolved
    // declaration would have to say "a function" in a message about a name the
    // reader wrote down.
    std::string name;
    if (def.has_value()) {
      name = linkageName(*def);
    }
    if (name.empty() && info.name != support::kInvalidSym) {
      name = std::string(symbols_.lookup(info.name));
    }
    if (name.empty()) {
      error(info.decl.valid() ? info.decl : file_.root(), IRDiagnosticCode::Internal,
            "a function reached lowering with no name");
      continue;
    }

    llvm::Type* mapped = llvmFunctionType(info.functionType);
    if (mapped == nullptr) {
      continue;
    }
    auto* type = llvm::dyn_cast<llvm::FunctionType>(mapped);
    if (type == nullptr) {
      fatal(spanOf(info.decl), IRDiagnosticCode::Internal,
            "`" + name + "` is declared with a type that is not a function type");
      continue;
    }

    // One `Function` per definition, never two. Two entries in the table can
    // name the same def -- `extern fn i32 f();` above `fn i32 f() { }` is the
    // ordinary pair -- and a second `Function::Create` would leave a stray
    // symbol behind: LLVM renames the loser to `f.1`, so the module would carry
    // a declaration of `f`, a `define` of `f.1` nobody calls, and a link error
    // for the call the program actually wrote. `sema` has already refused two
    // *disagreeing* signatures for one name, so the type chosen here is the type
    // of the one function.
    if (def.has_value() && functions_.contains(defKey(*def))) {
      continue;
    }

    // `External`: the language has no `static` keyword yet, and a file-scope
    // function in C is external unless it says otherwise -- which is exactly the
    // decision `resolve` already recorded as `Linkage::External` for one. When
    // `static` arrives, this reads the def's linkage instead of naming one.
    llvm::Function* function =
        llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage, name, module_);

    if (def.has_value()) {
      functions_.emplace(defKey(*def), function);
    }
  }
}

} // namespace minc::ir
