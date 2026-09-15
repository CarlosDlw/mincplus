// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The file scope: the bindings at the top of a unit, and the values they have
// before the program runs.
//
// Everything here follows from one decision (`globals.md`, decision 2): a
// file-scope object has **no dynamic initialization**. Its bytes are written by
// the compiler, so an initializer is an expression this stage evaluates to a
// *value* rather than a statement a later stage emits. Three rules fall out of
// that, and each is a rule about the language and not about this file:
//
//   * an initializer may read literals and file-scope `const`s. A call, a `let`,
//     a dereference, a local: each has no value at that point, and each gets a
//     sentence that names which one the reader wrote -- a diagnostic that says
//     "not a constant" about `x` where `x` is a `let` tells the reader nothing
//     they can act on;
//   * the bindings are checked in **dependency** order, because a file-scope name
//     is visible independently of order (`resolve.md`, decision A). In
//     `const a = b * 2; const b = 3;` the value of `b` has to exist before `a`
//     folds, or `a` would be refused for reading a name that is defined on the
//     next line -- which is the one thing the language promises cannot happen;
//   * a **cycle** is the one shape with no such order, so it is refused with the
//     chain that closes it. Evaluating one anyway would make the value of a
//     constant depend on the order the compiler happened to visit in.
//
// The result is *published* (`GlobalInfo`) rather than left to be re-derived: the
// bytes of a global are a value, and a lowering that recomputed them from the
// initializer would be a second copy of these rules that the first copy would
// eventually disagree with.
#include "checker.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace minc::sema {
namespace {

// The value kinds, in one table with the enumeration: a kind added without a row
// is a compile error here, and a row whose enumerator no longer exists is caught
// by `allGlobalValueKinds()` being derived from the table and compared against
// it in a test. Same shape as `access.cc`'s, for the same reason.
// NOLINTBEGIN(readability-identifier-naming): table name follows the project's
// convention for the other stages' tables.
constexpr std::array<GlobalValueKindInfo, 5> kGlobalValueKindInfos{{
    {GlobalValueKind::Zero, "zero"},
    {GlobalValueKind::Int, "int"},
    {GlobalValueKind::Literal, "literal"},
    {GlobalValueKind::Null, "null"},
    {GlobalValueKind::Aggregate, "aggregate"},
}};
// NOLINTEND(readability-identifier-naming)

template <std::size_t... Indexes>
[[nodiscard]] constexpr auto globalValueKindsFromTable(std::index_sequence<Indexes...>) {
  return std::array<GlobalValueKind, sizeof...(Indexes)>{kGlobalValueKindInfos[Indexes].kind...};
}

constexpr auto kAllGlobalValueKinds =
    globalValueKindsFromTable(std::make_index_sequence<kGlobalValueKindInfos.size()>{});

// Source order, for the messages this pass produces. The walk below is in
// *dependency* order, which is not the order a file is read in, and every other
// message this stage emits is already in source order -- so the two orders are
// reconciled once, here, rather than in the reader's head.
[[nodiscard]] bool sourceOrdered(const SemaError& a, const SemaError& b) {
  if (a.span.file != b.span.file) {
    return a.span.file < b.span.file;
  }
  if (a.span.begin != b.span.begin) {
    return a.span.begin < b.span.begin;
  }
  return a.span.end < b.span.end;
}

} // namespace

std::span<const GlobalValueKindInfo> globalValueKindInfos() {
  return kGlobalValueKindInfos;
}

std::span<const GlobalValueKind> allGlobalValueKinds() {
  return kAllGlobalValueKinds;
}

std::string_view toString(GlobalValueKind kind) {
  for (const GlobalValueKindInfo& info : kGlobalValueKindInfos) {
    if (info.kind == kind) {
      return info.name;
    }
  }
  return "unknown";
}

void Checker::checkGlobals() {
  globals_.clear();
  globalIndexByDef_.clear();
  collectGlobals();
  if (globals_.empty()) {
    return;
  }

  // Everything this pass reports is put back in source order at the end, so the
  // range is remembered here.
  const std::size_t firstError = out_.errors.size();
  const std::size_t firstWarning = out_.warnings.size();

  std::vector<std::vector<GlobalRead>> reads(globals_.size());
  for (std::size_t i = 0; i < globals_.size(); ++i) {
    reads[i] = readsOf(globals_[i].init);
  }

  // The name of a binding, for a sentence about a cycle. The `Name` node is what
  // the reader wrote, so it is what a message quotes.
  const auto nameOfGlobal = [&](std::size_t index) -> std::string {
    const ast::AstId name = childOf(globals_[index].decl, ast::NodeKind::Name);
    return name.valid() ? std::string(spelling(name)) : std::string("?");
  };

  // An explicit stack and not recursion: a chain is as long as the unit has
  // bindings, and a unit is somebody else's file.
  enum class Mark : std::uint8_t { White, Grey, Black };
  struct Frame {
    std::size_t index;
    std::size_t next;
  };
  std::vector<Mark> mark(globals_.size(), Mark::White);
  std::vector<Frame> stack;

  // A cycle, reported at the read that closed it -- which is the one node in it
  // that a reader can look at and recognise the loop from. It is reported once
  // per closing edge, and the chain is written out because "these two depend on
  // each other" is not actionable for a cycle of four.
  const auto reportCycle = [&](std::size_t start, ast::AstId at) {
    std::string chain;
    for (std::size_t frame = start; frame < stack.size(); ++frame) {
      chain += nameOfGlobal(stack[frame].index);
      chain += " -> ";
    }
    chain += nameOfGlobal(stack[start].index);
    error(at, SemaErrorCode::GlobalCycle,
          "`" + chain +
              "` is a cycle, and a file-scope value has to be computable first in some order: "
              "what a binding here needs is another value in the same cycle");
  };

  for (std::size_t root = 0; root < globals_.size(); ++root) {
    if (mark[root] != Mark::White) {
      continue;
    }
    mark[root] = Mark::Grey;
    stack.push_back(Frame{root, 0});
    while (!stack.empty()) {
      const std::size_t index = stack.back().index;
      const std::vector<GlobalRead>& dependencies = reads[index];
      if (stack.back().next < dependencies.size()) {
        const GlobalRead read = dependencies[stack.back().next];
        ++stack.back().next;
        switch (mark[read.index]) {
        case Mark::White:
          mark[read.index] = Mark::Grey;
          stack.push_back(Frame{read.index, 0});
          break;
        case Mark::Grey: {
          // On the stack: this edge closes a loop, and the stack from the entry
          // it returns to is the chain.
          std::size_t start = 0;
          for (std::size_t position = stack.size(); position-- > 0;) {
            if (stack[position].index == read.index) {
              start = position;
              break;
            }
          }
          reportCycle(start, read.at);
          break;
        }
        case Mark::Black:
          // Finished, so its value is recorded and there is nothing to do.
          break;
        }
        continue;
      }
      // Every dependency has a value, so this one can be decided. This is the
      // post-order visit, and it is what makes a forward reference fold.
      checkGlobal(globals_[index]);
      mark[index] = Mark::Black;
      stack.pop_back();
    }
  }

  // A binding in or after a cycle is still checked -- the pass is total, so the
  // published table has one entry per binding and the lowering never has to
  // reason about a missing one -- and a read of a value that was never decided
  // answers `Zero`. The only way to reach that is the cycle already reported, and
  // the unit has errors, so no module is built from it.
  std::stable_sort(out_.errors.begin() + static_cast<std::ptrdiff_t>(firstError), out_.errors.end(),
                   sourceOrdered);
  std::stable_sort(out_.warnings.begin() + static_cast<std::ptrdiff_t>(firstWarning),
                   out_.warnings.end(), sourceOrdered);
}

void Checker::collectGlobals() {
  // The file scope's bindings are the unit root's `let`/`const` children, in
  // source order. Source order and not walk order because it is what makes the
  // published table deterministic, which is what `ir` and the dump compare.
  for (const ast::AstId decl : operandsOf(file_.root())) {
    const ast::NodeKind kind = kindOf(decl);
    if (kind != ast::NodeKind::LetStmt && kind != ast::NodeKind::ConstStmt) {
      continue;
    }
    GlobalBinding binding;
    binding.decl = decl;
    binding.init = initializerOf(decl);
    if (const std::optional<resolve::DefId> def = defAtName(childOf(decl, ast::NodeKind::Name))) {
      binding.def = *def;
      // The first entry wins, which is the canonical declaration: `resolve` gives
      // a repeated name one canonical def and reports the repeat, so two entries
      // for one def cannot come out of a unit `resolve` accepted.
      globalIndexByDef_.emplace(def->index, globals_.size());
    }
    globals_.push_back(binding);
  }
}

std::vector<Checker::GlobalRead> Checker::readsOf(ast::AstId init) const {
  std::vector<GlobalRead> out;
  if (!init.valid()) {
    return out;
  }
  std::vector<ast::AstId> pending{init};
  while (!pending.empty()) {
    const ast::AstId node = pending.back();
    pending.pop_back();
    if (kindOf(node) == ast::NodeKind::PathExpr) {
      const std::optional<resolve::DefId> def = defOfPath(node);
      if (def.has_value()) {
        const auto found = globalIndexByDef_.find(def->index);
        if (found != globalIndexByDef_.end()) {
          out.push_back(GlobalRead{found->second, node});
        }
      }
    }
    for (const ast::AstId child : operandsOf(node)) {
      pending.push_back(child);
    }
  }
  // One edge per binding, and not one per name: an initializer that reads one
  // name twice would otherwise leave a dependency count that never reaches zero,
  // and a program with no cycle at all would be reported as having one. Sorting
  // by binding also makes the walk's order over the edges independent of where in
  // the expression a name happened to be written.
  std::sort(out.begin(), out.end(),
            [](const GlobalRead& a, const GlobalRead& b) { return a.index < b.index; });
  out.erase(
      std::unique(out.begin(), out.end(),
                  [](const GlobalRead& a, const GlobalRead& b) { return a.index == b.index; }),
      out.end());
  return out;
}

void Checker::checkGlobal(GlobalBinding& binding) {
  if (!binding.decl.valid() || inError(binding.decl)) {
    return;
  }
  // The binding is checked exactly as a block-scope one is: one production, one
  // checker. The annotation, the initializer's conversion, the `const` value and
  // the type written to the definition all come from `checkStatement`, so a
  // file-scope binding cannot end up checked by a different set of rules than the
  // local one it is spelled like.
  // Whether checking the binding itself already said something. The value
  // question below is only worth answering when the *type* is right: a
  // diagnostic about the value of an expression whose type was just refused
  // would be a second complaint about bytes the reader is going to change
  // anyway, and the first one already told them what to do.
  const std::size_t errorsBefore = out_.errors.size();
  checkStatement(binding.decl, kTypeError);
  binding.type = typeOfDef(binding.def);

  binding.value = evalInitializer(binding.init);
  if (!binding.value.ok && out_.errors.size() == errorsBefore) {
    // One sentence per binding, at the expression that stopped being an
    // initializer constant, or at the declaration when there is no such
    // expression. The sentence is *copied* into the diagnostic and not moved:
    // the record keeps what it said, so a later reader of the record and a
    // reader of the message cannot disagree about it.
    const ast::AstId at = binding.value.offender.valid() ? binding.value.offender : binding.decl;
    error(at, binding.value.code, binding.value.reason);
  }
  publishGlobal(binding);
  // Set last, so a read of this binding from inside its own initializer (which
  // is a cycle, and reported) finds no value rather than half of one.
  binding.decided = true;
}

Checker::IceValue Checker::evalAggregate(ast::AstId expr) const {
  // No sign bit and no parameter for one: a unary sign on an array is refused by
  // the checker, so the flag `evalInitializer` carries for a literal has nothing
  // to mean here. An aggregate is a *shape*, and a shape has no sign.
  IceValue out;
  const std::vector<ast::AstId> operands = operandsOf(expr);
  if (operands.empty()) {
    return notConstant(expr);
  }
  // The typed form's first operand is its `Type` node; the context form's type
  // came from the binding and lives in the record. The same split the checker
  // makes, because it is the same tree.
  const std::size_t first = kindOf(operands.front()) == ast::NodeKind::Type ? 1U : 0U;
  const std::span<const ast::AstId> elements(operands.data() + first, operands.size() - first);
  if (elements.empty()) {
    return notConstant(expr);
  }

  out.kind = GlobalValueKind::Aggregate;
  out.node = expr;
  out.splat = hasFillSeparator(expr);
  if (out.splat) {
    // A fill: the value is the one element, walked exactly as a list entry is.
    // The count is the type's and stays there -- this record is a *shape*.
    //
    // A nested fill is a fill too: `[2][3]i32{[1, 2, 3]; 2}` is `elements` of one
    // record whose own `splat` is set, which is the same recursion one level down
    // and not a special case -- the element of the outer array *is* an aggregate,
    // and this record has a form for an aggregate.
    IceValue filled = evalInitializer(elements.front());
    if (!filled.ok) {
      // The element's own refusal, and not a fresh one about the whole
      // initializer: the sentence and the caret have to be about the expression
      // that stopped being a constant -- `g()` -- because that is the one the
      // reader changes. Re-wrapping it here would answer "this is not a constant a
      // file-scope object can be initialized with", which is true, useless, and
      // points at eleven characters of table instead of at the call.
      return filled;
    }
    GlobalElementValue element;
    element.kind = filled.kind;
    element.intValue = filled.value;
    element.node = filled.node;
    element.negated = filled.negated;
    element.type = out_.typed.typeOf(elements.front());
    element.elements = filled.elements;
    element.splat = filled.splat;
    out.elements.push_back(std::move(element));
    return out;
  }

  for (const ast::AstId element : elements) {
    // One level down, and one level deeper when that element is itself a list:
    // the same function, because an element is an initializer of its own type.
    const IceValue value = evalInitializer(element);
    if (!value.ok) {
      // The element's own sentence, exactly as the fill's value propagates its own
      // above: the offender is inside the element, and the *first* element that is
      // not constant is the one worth naming (depth-first, source order, the same
      // rule `notConstant` uses when it descends into an operand).
      return value;
    }
    GlobalElementValue record;
    record.kind = value.kind;
    record.intValue = value.value;
    record.node = value.node;
    record.negated = value.negated;
    record.type = out_.typed.typeOf(element);
    // Both fields of the aggregate shape, and unconditionally: an element that is
    // not an aggregate holds no elements and is not a splat, which is what the
    // zero value of both already says.
    record.elements = value.elements;
    record.splat = value.splat;
    out.elements.push_back(std::move(record));
  }
  return out;
}

Checker::IceValue Checker::evalInitializer(ast::AstId expr, bool negated) const {
  IceValue out;
  if (!expr.valid()) {
    // No initializer at all. The object is zero, which is the C ABI's `.bss` and
    // a decision rather than an omission (`memory.md`, decision 12).
    return out;
  }

  // The two wrappers that are not expressions of their own are followed *first*,
  // before anything is decided about the expression: a parenthesis is not a
  // value, and a sign is a bit. Doing it here is what makes the sentence below be
  // about the expression that stopped being an initializer constant -- `(x)`
  // should point at `x`, and `-(1 + 2.0)` should be about the float sum.
  switch (kindOf(expr)) {
  case ast::NodeKind::ParenExpr: {
    const std::vector<ast::AstId> operands = operandsOf(expr);
    return operands.empty() ? out : evalInitializer(operands.front(), negated);
  }
  case ast::NodeKind::PrefixExpr: {
    const ast::AstId op = tokenOf(expr);
    const std::vector<ast::AstId> operands = operandsOf(expr);
    if (op.valid() && !operands.empty()) {
      const Tag kind = tagOf(kindOf(op));
      if (kind == kTokPlus || kind == kTokMinus) {
        // A sign, and not arithmetic: the operand's own value with one bit
        // changed. That is exact for a float (its sign bit) and for a
        // two's-complement integer, so `-1.0` is still a literal -- which is what
        // keeps the most ordinary negative constant there is from being refused
        // by a language that refuses float *arithmetic* at file scope.
        return evalInitializer(operands.front(), kind == kTokMinus ? !negated : negated);
      }
    }
    break;
  }
  default:
    break;
  }

  // An aggregate, before the `isConstant` test and not after it. That fact is
  // deliberately false for an array -- the checker refuses to claim a value it
  // has no record for (`arrays.md` decision 15, step 8) -- so the question here
  // is the one this walk is: is every element one of the values above? The
  // recursion below is the whole rule, which is what makes "an array is constant"
  // the same sentence as "its elements are", with no second notion of constness.
  if (kindOf(expr) == ast::NodeKind::TypedInitializer ||
      kindOf(expr) == ast::NodeKind::ArrayLiteral) {
    return evalAggregate(expr);
  }

  const ExprInfo& facts = out_.typed.infoOf(expr);
  if (!facts.isConstant) {
    return notConstant(expr);
  }
  if (facts.hasIntValue) {
    // Every integer, `char` and `bool` constant, and any arithmetic over them:
    // the checker folded it with the same 64-bit core `#if` uses
    // (`support/consteval`), so the value is here and nothing below reads a
    // spelling to find it.
    //
    // The sign the wrappers above carried in is applied *here*, to the value, and
    // not kept as a flag for a later stage: two's complement has exactly one
    // representation for every value, so the negation is exact and `-7` is the
    // value `-7`. Leaving it to the lowering would mean the lowering negating a
    // number, which is arithmetic, and `Literal`'s flag exists so that a float
    // never needs it.
    out.kind = GlobalValueKind::Int;
    out.value = negated ? support::negate(facts.value) : facts.value;
    return out;
  }
  switch (kindOf(expr)) {
  case ast::NodeKind::LiteralExpr:
    // A float, a `str`, or an integer wider than that core: a value with no
    // folded form. The lowering reads it from the literal itself, so the record
    // names the *node* and no arithmetic happens a stage later.
    out.kind = GlobalValueKind::Literal;
    out.node = expr;
    out.negated = negated;
    return out;
  case ast::NodeKind::PathExpr: {
    const std::optional<resolve::DefId> def = defOfPath(expr);
    if (!def.has_value() || def->index >= defs_.defs.size()) {
      return notConstant(expr);
    }
    if (defs_.defs[def->index].predefined == resolve::Predefined::Null) {
      // `null` is `*void` and it is a value the compiler knows. `true` and
      // `false` never reach here: they carry an integer value.
      out.kind = GlobalValueKind::Null;
      return out;
    }
    const IceValue* earlier = globalValueOf(*def);
    if (earlier == nullptr || !earlier->ok) {
      // No value, and there are exactly two ways to want one that is not there:
      // the binding is in a cycle (reported by the walk, with the chain), or its
      // own initializer was refused (reported where it was written). Both are one
      // mistake that has already been said, and propagating a refused value would
      // carry its *sentence* into this binding's message -- where it would be a
      // second diagnostic, at the wrong place, about something the reader has
      // already been told.
      return out;
    }
    out = *earlier;
    if (negated) {
      out.negated = !out.negated;
    }
    return out;
  }
  default:
    return notConstant(expr);
  }
}

Checker::IceValue Checker::notConstant(ast::AstId expr) const {
  IceValue out;
  out.ok = false;
  out.offender = expr;
  out.code = SemaErrorCode::GlobalNotConstant;

  const ExprInfo& facts = out_.typed.infoOf(expr);

  switch (kindOf(expr)) {
  case ast::NodeKind::LiteralExpr:
    // A literal the literal reader refused -- a digit out of base, a redundant
    // leading zero -- was reported *by the lexer*, and `checkLiteral`
    // deliberately leaves it neither constant nor a value. Saying "not a
    // constant" about those bytes would be a second diagnostic for one mistake,
    // so nothing is said about them here.
    if (!facts.isConstant) {
      out.ok = true;
      return out;
    }
    break;
  case ast::NodeKind::PathExpr: {
    const std::string name = std::string(spelling(expr));
    const std::optional<resolve::DefId> def = defOfPath(expr);
    if (def.has_value() && def->index < defs_.defs.size()) {
      const resolve::Def& declaration = defs_.defs[def->index];
      switch (declaration.kind) {
      case resolve::DefKind::Variable:
        // A `let`, local or file-scope: either way it is a name whose value the
        // compiler does not write down, which is the one thing an initializer
        // needs.
        out.reason = "`" + name +
                     "` is a `let`: a file-scope object's bytes are written by the compiler, and a "
                     "mutable binding has no value yet at that point";
        return out;
      case resolve::DefKind::Function:
        out.reason = "`" + name +
                     "` is a function: where the program goes when it is called is not a value "
                     "this compiler can write into an object";
        return out;
      default:
        break;
      }
      // A **file-scope** binding that got this far is a constant whose value was
      // never decided, which is a cycle -- and the walk has already reported it,
      // with the chain that closes it. This name *is* a constant, so any sentence
      // here would be a second one about that cycle and a false one besides.
      if (globalIndexByDef_.contains(def->index) && declaration.scope == defs_.fileScope) {
        out.ok = true;
        return out;
      }
      if (declaration.scope != defs_.fileScope) {
        out.reason = "`" + name +
                     "` is a local: it has no value until its function runs, and a file-scope "
                     "initializer is written before anything runs";
        return out;
      }
    }
    out.reason = "`" + name + "` is not a constant a file-scope initializer can read";
    return out;
  }
  case ast::NodeKind::CallExpr:
    out.reason = "a call is not a constant: a file-scope object's value has to exist before the "
                 "program starts, and a function's result is not available until then";
    return out;
  case ast::NodeKind::IndexExpr:
    out.reason = "an indexed access reads memory, and nothing has run yet at file scope";
    return out;
  case ast::NodeKind::AssignExpr:
    out.reason = "an assignment is not a value an object can be created with";
    return out;
  case ast::NodeKind::PrefixExpr: {
    const ast::AstId op = tokenOf(expr);
    if (op.valid()) {
      const Tag kind = tagOf(kindOf(op));
      if (kind == kTokStar) {
        out.reason = "a dereference reads memory, and nothing has run yet at file scope";
        return out;
      }
      if (kind == kTokAmp) {
        out.reason =
            "the address of an object is not a value this language writes at file scope: a "
            "`str` literal is the one address an initializer can hold, and its bytes the "
            "compiler writes itself";
        return out;
      }
    }
    break;
  }
  default:
    break;
  }

  if (facts.isConstant) {
    // A *computed* constant this stage has no value for: a float operation, whose
    // folding would need a float reader that this compiler deliberately does not
    // have (`globals.md`, *What an initializer may be*). Saying "not a constant"
    // about `1.0 + 2.0` would be false, and a reader who is told something false
    // stops trusting the diagnostic.
    out.reason = "this is a constant this compiler does not fold at file scope: integers are "
                 "folded, and a float value is read from the literal that spells it";
    return out;
  }

  // The expression is not constant, and none of the arms above is the reason: so
  // the reason is one of its operands. Asking the *first* one that is not
  // constant, depth-first in source order, is what makes the sentence and the
  // caret be about a leaf the reader can change -- `a + b` where `a` is a `let`
  // is a story about `a`, and "this is not a constant" about the sum is a story
  // about nothing. The recursion is strictly downward, so it terminates, and a
  // node whose operands are all constant is answered by the sentence below.
  for (const ast::AstId operand : operandsOf(expr)) {
    if (!out_.typed.infoOf(operand).isConstant) {
      return notConstant(operand);
    }
  }

  out.reason = "this is not a constant a file-scope object can be initialized with";
  return out;
}

const Checker::IceValue* Checker::globalValueOf(resolve::DefId def) const {
  if (!def.valid()) {
    return nullptr;
  }
  const auto found = globalIndexByDef_.find(def.index);
  if (found == globalIndexByDef_.end()) {
    return nullptr;
  }
  const GlobalBinding& binding = globals_[found->second];
  return binding.decided ? &binding.value : nullptr;
}

void Checker::publishGlobal(const GlobalBinding& binding) {
  GlobalInfo info;
  info.decl = binding.decl;
  info.init = binding.init;
  info.type = binding.type;
  info.value = binding.value.kind;
  info.intValue = binding.value.value;
  info.node = binding.value.node;
  info.negated = binding.value.negated;
  // The aggregate's *shape*, copied and never re-derived: the lowering reads this
  // record instead of re-walking the tree for what makes an element constant,
  // because that walk is this pass's rule and a second copy of it is the copy
  // that disagrees (`arrays.md` decision 15, step 8).
  info.elements = binding.value.elements;
  info.splat = binding.value.splat;
  out_.typed.addGlobal(info);
}

} // namespace minc::sema
