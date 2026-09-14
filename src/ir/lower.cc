// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The entry point, the module handle, the diagnostic vocabulary, and the tree
// accessors the rest of the lowering reads.
//
// Two things in this file are worth reading before the others. The first is
// `Module::Impl`: it owns the `LLVMContext`, and the module and the data layout
// belong to *it* in that order, which is what makes `Module` safe to hand around
// by value while keeping every LLVM type out of `ir.h`. The second is the
// precondition check in `run()`: a typed tree with a poison in it is refused
// here rather than lowered, because `TypeKind::Error` converts to and from
// everything silently, so a poisoned tree lowers to *well-formed* IR for a
// program whose meaning nobody decided -- the one failure a verifier cannot see.
#include "lowering.h"

#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <mutex>

#include "llvm/IR/Verifier.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Triple.h"

#include "debug.h"
#include "ir/storage.h"

#include "ir/ir.h"
#include "sema/target.h"
#include "support/intern/interner.h"
#include "support/span/file_id.h"
#include "support/span/span.h"

namespace minc::ir {
namespace {

// The node kinds that produce a value. `Type` and `Name` are deliberately not
// here: they carry a type too, but a `void` annotation or a malformed one is a
// *sema* error, and the precondition below is about the poison reaching the
// lowering on a node that has to be evaluated.
[[nodiscard]] bool isExpressionKind(ast::NodeKind kind) {
  switch (kind) {
  case ast::NodeKind::LiteralExpr:
  case ast::NodeKind::PathExpr:
  case ast::NodeKind::ParenExpr:
  case ast::NodeKind::PrefixExpr:
  case ast::NodeKind::PostfixExpr:
  case ast::NodeKind::BinaryExpr:
  case ast::NodeKind::ConditionalExpr:
  case ast::NodeKind::AssignExpr:
  case ast::NodeKind::CallExpr:
  case ast::NodeKind::IndexExpr:
    return true;
  default:
    return false;
  }
}

// LLVM's rules for the target, as a `DataLayout`. The machine is *temporary*:
// this stage needs the layout, not a target machine, and the one that belongs to
// `codegen` is `codegen`'s (`ir.md`, decision 11). Note what this buys beyond a
// table -- the layout is LLVM's own answer, so nothing here can disagree with the
// backend about a size or an alignment.
//
// An architecture LLVM was not built with, or a triple it cannot parse, is a
// refusal with its own code and not a module with an empty layout string: an
// empty data layout makes every `sizeof` and every `align` a guess, which is the
// one thing this stage may not do.
// LLVM's targets are registered by an explicit call and not by linking alone:
// a shared `libLLVM` keeps the target registries in static constructors that a
// static link discards, and a compiler that only linked the umbrella would find
// an empty registry and no layout for a target it states by name. Once, for the
// process, and every target rather than the native one -- `--target` names an
// architecture the host may not be, and a cross build is the ordinary case here.
void registerTargets() {
  static std::once_flag once;
  std::call_once(once, [] {
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
  });
}

[[nodiscard]] std::optional<llvm::DataLayout> dataLayoutFor(const sema::TargetInfo& target,
                                                            std::string& refusal) {
  registerTargets();
  const std::string& text = target.name();
  llvm::Triple triple(text);
  if (triple.getArch() == llvm::Triple::UnknownArch) {
    refusal = "LLVM cannot parse the triple `" + text + "`";
    return std::nullopt;
  }
  std::string error;
  const llvm::Target* registered = llvm::TargetRegistry::lookupTarget(triple, error);
  if (registered == nullptr) {
    refusal = "this build of LLVM has no code generator for `" + text +
              "`, so the target's sizes and alignments are unknown";
    return std::nullopt;
  }
  llvm::TargetOptions options;
  std::unique_ptr<llvm::TargetMachine> machine(
      registered->createTargetMachine(triple, /*CPU=*/"", /*Features=*/"", options, std::nullopt,
                                      std::nullopt, llvm::CodeGenOptLevel::None));
  if (machine == nullptr) {
    refusal = "LLVM will not build a target machine for `" + text + "`";
    return std::nullopt;
  }
  return machine->createDataLayout();
}

} // namespace

// --- the diagnostic vocabulary -------------------------------------------------

namespace {
// NOLINTBEGIN(readability-identifier-naming): table name follows the project's
// convention for the other stages' tables.
constexpr IRDiagnosticCodeInfo kDiagnosticCodeInfos[] = {
    {IRDiagnosticCode::UnsupportedType, "ir-unsupported-type"},
    {IRDiagnosticCode::UnsupportedNode, "ir-unsupported-node"},
    {IRDiagnosticCode::UnsupportedTarget, "ir-unsupported-target"},
    {IRDiagnosticCode::Internal, "ir-internal"},
    {IRDiagnosticCode::MissingObligation, "ir-missing-obligation"},
    {IRDiagnosticCode::Assumption, "ir-assumption"},
    {IRDiagnosticCode::Alignment, "ir-alignment"},
    {IRDiagnosticCode::UnguardedOp, "ir-unguarded-op"},
};
// NOLINTEND(readability-identifier-naming)
} // namespace

const char* nameOf(IRDiagnosticCode code) {
  for (const IRDiagnosticCodeInfo& info : kDiagnosticCodeInfos) {
    if (info.code == code) {
      return info.name;
    }
  }
  return "ir-internal";
}

std::string_view toString(IRDiagnosticCode code) {
  return nameOf(code);
}

const std::vector<IRDiagnosticCode>& allDiagnosticCodes() {
  // Derived from the one table, so a code added to the enum without a row is
  // caught by the enumeration test instead of by a user.
  static const std::vector<IRDiagnosticCode> codes = [] {
    std::vector<IRDiagnosticCode> out;
    out.reserve(std::size(kDiagnosticCodeInfos));
    for (const IRDiagnosticCodeInfo& info : kDiagnosticCodeInfos) {
      out.push_back(info.code);
    }
    return out;
  }();
  return codes;
}

// --- the module handle ---------------------------------------------------------

// `ModuleStorage` is defined in `storage.h` so the printer and the scanner can
// reach the same object without `Module` exposing LLVM to the whole tree.

Module::Module() = default;
Module::~Module() = default;
Module::Module(Module&&) noexcept = default;
Module& Module::operator=(Module&&) noexcept = default;

bool Module::built() const {
  return impl_ != nullptr && impl_->module != nullptr;
}

support::FileId Module::file() const {
  return impl_ == nullptr ? support::kInvalidFile : impl_->file;
}

// --- the lowering --------------------------------------------------------------

namespace {

// Built before anything else in the initialiser list, because the context, the
// layout and the module all live in it.
[[nodiscard]] std::unique_ptr<ModuleStorage> makeImpl(support::FileId file) {
  auto impl = std::make_unique<ModuleStorage>();
  impl->file = file;
  impl->module =
      std::make_unique<llvm::Module>("minc+ unit " + std::to_string(file), impl->context);
  return impl;
}

} // namespace

Lowering::Lowering(const ast::LoweredFile& file, const resolve::DefMap& defs,
                   const sema::TypedFile& typed, const sema::TypeStore& types,
                   const support::Interner& symbols, const LoweringOptions& options)
    : file_(file), defs_(defs), typed_(typed), types_(types), symbols_(symbols), options_(options),
      impl_(makeImpl(file.file())), context_(impl_->context), module_(*impl_->module),
      layout_(impl_->layout), builder_(impl_->context), allocaBuilder_(impl_->context) {
  // The declarations are indexed first, because an index is what keeps a lookup
  // per `PathExpr` a hash instead of a scan. Both keys are unit offsets for the
  // same reason the checker uses them: a macro can give two names one *written*
  // location, and a unit offset is one token each, so it is unique.
  //
  // The answer is the def's **identity**, not the site's own index: a name
  // declared twice is one function, and `functions_` is keyed on this. Answering
  // with the site would create two `llvm::Function`s for one name, and LLVM
  // renames the loser to `f.1` -- leaving `f` declared and undefined while the
  // body lands under a name no call refers to.
  for (std::size_t i = 0; i < defs_.defs.size(); ++i) {
    const resolve::Def& def = defs_.defs[i];
    if (def.predefined) {
      continue;
    }
    const resolve::DefId site{def.unitSpan.file, static_cast<std::uint32_t>(i)};
    defByName_.emplace(offsetKey(def.unitSpan.file, def.unitSpan.begin),
                       resolve::canonicalOf(def, site));
  }
  for (const resolve::NameRef& ref : defs_.refs) {
    if (!ref.resolved()) {
      continue;
    }
    refByOffset_.emplace(offsetKey(ref.unitSpan.file, ref.unitSpan.begin), ref.target);
  }

  // The target and the layout next: every instruction below is built against
  // this layout, and a refusal here means nothing is built at all.
  const sema::TargetInfo& target = types_.target();
  std::string refusal;
  const std::optional<llvm::DataLayout> layout = dataLayoutFor(target, refusal);
  if (!layout.has_value()) {
    errorAt(support::Span{}, IRDiagnosticCode::UnsupportedTarget, std::move(refusal));
    return;
  }
  impl_->layout = *layout;
  module_.setTargetTriple(llvm::Triple(target.name()));
  module_.setDataLayout(impl_->layout);

  // Debug information last, because it reads the triple and the layout: the two
  // module flags it sets belong to a module that already knows its target, and a
  // `DIFile` built before the data layout would be attached to a module whose
  // sizes nobody has decided. `-g` with no source file is a driver bug -- the
  // driver always has one -- and is refused rather than defaulted, because a line
  // table for the wrong file is worse than none.
  if (options_.debugInfo) {
    if (options_.source == nullptr) {
      errorAt(support::Span{}, IRDiagnosticCode::Internal,
              "debug information was requested with no source file to point the line table at");
      return;
    }
    debug_ = std::make_unique<DebugInfo>(module_, *options_.source, options_.producer);
  }

  // The cross-check `ir.md` names as the one place the front end's target model
  // can be wrong with nothing else noticing: `sema` states the pointer width by
  // rule and cannot link LLVM to check itself, and this stage can. A disagreement
  // is not a guess to be papered over -- every `isize`, every pointer access and
  // every `sizeof` depends on it -- so it refuses.
  const std::uint32_t pointerBits =
      static_cast<std::uint32_t>(impl_->layout.getPointerSizeInBits(0));
  if (pointerBits != target.pointerBits) {
    errorAt(support::Span{}, IRDiagnosticCode::UnsupportedTarget,
            "`" + target.name() + "` has " + std::to_string(target.pointerBits) +
                "-bit pointers and LLVM's data layout says " + std::to_string(pointerBits) +
                ", so this compiler's target table and LLVM disagree");
  }
}

Lowering::~Lowering() = default;

Module Lowering::takeModule() {
  Module out;
  out.impl_ = std::move(impl_);
  return out;
}

void Lowering::error(ast::AstId at, IRDiagnosticCode code, std::string message) {
  errorAt(spanOf(at), code, std::move(message));
}

void Lowering::errorAt(support::Span span, IRDiagnosticCode code, std::string message) {
  IRDiagnostic diagnostic;
  diagnostic.span = span;
  diagnostic.code = code;
  diagnostic.message = std::move(message);
  diagnostics_.push_back(std::move(diagnostic));
}

void Lowering::fatal(support::Span span, IRDiagnosticCode code, std::string message) {
  errorAt(span, code, std::move(message));
  failed_ = true;
}

// --- debug information ----------------------------------------------------------

void Lowering::locate(ast::AstId id) {
  if (debug_ == nullptr || !id.valid()) {
    return;
  }
  locate(spanOf(id));
}

void Lowering::locate(support::Span span) {
  if (debug_ == nullptr) {
    return;
  }
  // Set on both builders: the alloca builder is the one that is *not* moved by
  // `SetInsertPoint` in the usual flow, so a location set only on `builder_`
  // would leave every frame slot pointing at whatever the entry block's first
  // instruction happens to carry.
  const llvm::DebugLoc location = debug_->locationAt(span);
  builder_.SetCurrentDebugLocation(location);
  allocaBuilder_.SetCurrentDebugLocation(location);
}

// --- tree access ---------------------------------------------------------------

std::vector<ast::AstId> Lowering::operandsOf(ast::AstId id) const {
  std::vector<ast::AstId> out;
  if (!id.valid()) {
    return out;
  }
  for (const ast::AstId child : file_.childrenOf(id)) {
    if (!file_.at(child).isToken()) {
      out.push_back(child);
    }
  }
  return out;
}

ast::AstId Lowering::tokenOf(ast::AstId id) const {
  if (!id.valid()) {
    return ast::AstId{};
  }
  for (const ast::AstId child : file_.childrenOf(id)) {
    if (file_.at(child).isToken()) {
      return child;
    }
  }
  return ast::AstId{};
}

lex::TokenKind Lowering::tokenKindOf(ast::AstId id) const {
  if (!id.valid()) {
    return lex::TokenKind::Invalid;
  }
  return parse::toTokenKind(kindOf(id));
}

// --- the published answers ------------------------------------------------------

const sema::Coercion* Lowering::coercionFor(ast::AstId consumer, ast::AstId child) const {
  if (!consumer.valid() || !child.valid()) {
    return nullptr;
  }
  for (const sema::Coercion& coercion : typed_.coercionsOf(consumer)) {
    if (coercion.node == child) {
      return &coercion;
    }
  }
  return nullptr;
}

const sema::AccessObligation* Lowering::obligationFor(ast::AstId place) const {
  return typed_.accessAt(place);
}

bool Lowering::isAccessNode(ast::AstId id) const {
  if (!id.valid()) {
    return false;
  }
  if (kindOf(id) == ast::NodeKind::IndexExpr) {
    return true;
  }
  if (kindOf(id) != ast::NodeKind::PrefixExpr) {
    return false;
  }
  const ast::AstId op = tokenOf(id);
  return op.valid() && tagOf(kindOf(op)) == kTokStar;
}

// --- declaration lookup ---------------------------------------------------------

std::optional<resolve::DefId> Lowering::defOfPath(ast::AstId pathExpr) const {
  if (!pathExpr.valid()) {
    return std::nullopt;
  }
  const auto found =
      refByOffset_.find(offsetKey(file_.at(pathExpr).unit.file, file_.at(pathExpr).unit.begin));
  if (found == refByOffset_.end()) {
    return std::nullopt;
  }
  return found->second;
}

std::optional<resolve::DefId> Lowering::defAtName(ast::AstId nameNode) const {
  if (!nameNode.valid()) {
    return std::nullopt;
  }
  const support::Span unit = file_.at(nameNode).unit;
  const auto found = defByName_.find(offsetKey(unit.file, unit.begin));
  if (found != defByName_.end()) {
    return found->second;
  }
  const support::Span span = spanOf(nameNode);
  for (std::size_t i = 0; i < defs_.defs.size(); ++i) {
    const resolve::Def& def = defs_.defs[i];
    if (def.predefined || def.nameSpan.file != span.file) {
      continue;
    }
    if (def.nameSpan.begin >= span.begin && def.nameSpan.end <= span.end) {
      return resolve::DefId{span.file, static_cast<std::uint32_t>(i)};
    }
  }
  return std::nullopt;
}

std::optional<resolve::DefId> Lowering::defOfPlace(ast::AstId expr) const {
  ast::AstId current = expr;
  while (current.valid()) {
    if (kindOf(current) == ast::NodeKind::PathExpr) {
      return defOfPath(current);
    }
    if (kindOf(current) != ast::NodeKind::ParenExpr) {
      return std::nullopt;
    }
    const std::vector<ast::AstId> operands = operandsOf(current);
    if (operands.empty()) {
      return std::nullopt;
    }
    current = operands.front();
  }
  return std::nullopt;
}

llvm::AllocaInst* Lowering::localOf(ast::AstId pathExpr) const {
  const std::optional<resolve::DefId> def = defOfPath(pathExpr);
  if (!def.has_value()) {
    return nullptr;
  }
  const auto found = locals_.find(defKey(*def));
  return found == locals_.end() ? nullptr : found->second;
}

llvm::AllocaInst* Lowering::declareLocal(resolve::DefId def, sema::TypeId type,
                                         std::string_view name, ast::AstId at) {
  const std::uint64_t key = defKey(def);
  const auto existing = locals_.find(key);
  if (existing != locals_.end()) {
    return existing->second;
  }
  // A type this stage cannot map has no size and so no slot. `llvmType` has
  // already refused it (`ir-unsupported-type` -- `f80` on a target with no x87
  // format, an array before its syntax exists), and an `alloca` built from the
  // null it returns would be a crash where the reader was just handed a
  // diagnostic. `locals_` stays unset, so the binding has no slot rather than a
  // wrong one.
  llvm::Type* slotType = storageType(type);
  if (slotType == nullptr) {
    return nullptr;
  }
  // At the *front* of the entry block, re-seated per slot. A binding declared
  // after the first branch would otherwise be appended after that branch's
  // terminator: the entry block is where frames belong, and "the end of the
  // entry block" stops being the front the moment anything branches.
  if (entryBlock_ != nullptr) {
    allocaBuilder_.SetInsertPoint(entryBlock_, entryBlock_->begin());
  }
  // Re-seated, `SetInsertPoint` copies the location of the instruction it lands
  // on, so the slot's own line number has to be set again after the seat.
  locate(at);
  llvm::AllocaInst* alloca = allocaBuilder_.CreateAlloca(slotType, nullptr, name);
  // The alignment is stated rather than left to the target's choice: the number
  // has to be the one the type states, because it is the same number every
  // access through this object will carry and the scan compares the two.
  alloca->setAlignment(llvm::Align(alignmentOf(type)));
  locals_.emplace(key, alloca);
  if (debug_ != nullptr && at.valid()) {
    debug_->declareBinding(*alloca, name, types_, type, spanOf(at));
  }
  return alloca;
}

// --- the unit ------------------------------------------------------------------

bool Lowering::run() {
  // A diagnostic from the constructor -- an unusable target -- is already a
  // refusal, and the module must not be built around it.
  if (failed_ || !diagnostics_.empty()) {
    return false;
  }

  // The precondition, checked rather than trusted. `TypeKind::Error` is a real
  // type that converts to and from everything silently, so a poisoned tree
  // lowers to well-formed IR for a program whose meaning nobody decided -- and
  // a deferred literal has no width, so it has no LLVM type at all. Both are
  // bugs in this compiler, not statements about the program, so both are
  // `ir-internal` and both stop the module.
  for (std::uint32_t index = 0; index < file_.nodeCount(); ++index) {
    const ast::AstId id{index};
    if (!isExpressionKind(file_.at(id).kind)) {
      continue;
    }
    const sema::TypeId type = typed_.typeOf(id);
    if (types_.isError(type)) {
      fatal(spanOf(id), IRDiagnosticCode::Internal,
            "an expression reached lowering with no type; the unit was not checked or the check "
            "failed and reported nothing");
      return false;
    }
    if (types_.isDeferred(type)) {
      fatal(spanOf(id), IRDiagnosticCode::Internal,
            "an expression reached lowering with a deferred literal type; `sema` is supposed to "
            "decide every one of them");
      return false;
    }
  }

  declareFunctions();
  if (failed_) {
    return false;
  }
  for (const sema::FunctionInfo& info : typed_.functionTable) {
    defineFunction(info);
    if (failed_) {
      return false;
    }
  }
  // The debug metadata is resolved before the verifier sees it, and the order is
  // load-bearing: until `finalize` runs, the compile unit's retained arrays and
  // every subprogram are temporaries, and a module with temporaries verifies
  // intermittently and prints `<temporary>` in the places a reader looks.
  if (debug_ != nullptr) {
    debug_->finalize();
  }

  // LLVM's own verifier, last: it cannot see a rule of *this* language (that is
  // `invariants.cc`), but it does prove the module is well formed, and a stage
  // that handed on a malformed module would make every later failure somebody
  // else's bug report. A failure here is this compiler's, so it is `ir-internal`
  // and the module is discarded like any other refusal.
  std::string reason;
  llvm::raw_string_ostream stream(reason);
  if (llvm::verifyModule(module_, &stream)) {
    errorAt(support::Span{}, IRDiagnosticCode::Internal,
            "the module this compiler built is not well formed: " + stream.str());
    return false;
  }

  // "A stage that cannot answer a question does not emit": a refusal anywhere
  // below discards the module rather than handing on a partial one.
  return diagnostics_.empty();
}

// --- the entry point ------------------------------------------------------------

IRResult lowerUnit(const ast::LoweredFile& file, const resolve::DefMap& defs,
                   const sema::TypedFile& typed, const sema::TypeStore& types,
                   const support::Interner& symbols, const LoweringOptions& options) {
  Lowering lowering(file, defs, typed, types, symbols, options);
  IRResult result;
  if (!lowering.run()) {
    result.diagnostics = lowering.takeDiagnostics();
    return result;
  }
  result.module = lowering.takeModule();
  result.diagnostics = lowering.takeDiagnostics();
  return result;
}

} // namespace minc::ir
