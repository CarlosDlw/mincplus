// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "pp/macro.h"

#include <cstddef>
#include <string>
#include <utility>

#include "builtins/builtin.h"
#include "support/limits.h"

namespace minc::pp {
namespace {

// True when every body token points at a parameter that exists, or at nothing.
// A definition that fails this was built wrong, and catching it here keeps a
// malformed table from being read out of bounds during substitution.
[[nodiscard]] bool parametersAreConsistent(const MacroInfo& info) {
  const std::size_t slots = info.params.size() + (info.variadic ? 1U : 0U);
  for (const MacroBodyToken& token : info.body) {
    if (token.param != kNotAParameter && token.param >= slots) {
      return false;
    }
    if (token.spellingOffset > info.spellings.size() ||
        static_cast<std::size_t>(token.spellingOffset) + token.spellingLength >
            info.spellings.size()) {
      return false;
    }
  }
  return true;
}

} // namespace

std::string MacroInfo::bodyText() const {
  std::string out;
  for (std::size_t i = 0; i < body.size(); ++i) {
    if (i != 0) {
      out.push_back(' ');
    }
    out += spellingOf(i);
  }
  return out;
}

bool identicalDefinition(const MacroInfo& left, const MacroInfo& right) {
  if (left.kind != right.kind || left.variadic != right.variadic ||
      left.params.size() != right.params.size() || left.body.size() != right.body.size()) {
    return false;
  }
  for (std::size_t i = 0; i < left.params.size(); ++i) {
    if (left.params[i].name != right.params[i].name ||
        left.params[i].usedWithHash != right.params[i].usedWithHash ||
        left.params[i].usedWithPaste != right.params[i].usedWithPaste) {
      return false;
    }
  }
  // Spellings, not spans: the standard's rule is about the token sequence, so
  // the same constant defined the same way in two different headers is one
  // definition as far as a redefinition diagnostic is concerned.
  for (std::size_t i = 0; i < left.body.size(); ++i) {
    if (left.spellingOf(i) != right.spellingOf(i)) {
      return false;
    }
  }
  return true;
}

MacroTable::Change MacroTable::define(MacroInfo info) {
  Change change;
  if (!info.valid()) {
    change.error = PPError{{}, "macro definition has no name", PPErrorCode::MissingMacroName};
    return change;
  }
  // A macro is one of the two ways a name can be taken away from the compiler
  // (the other is a declaration, and `resolve` refuses that one), and it is the
  // *earlier* way: `#define __builtin_trap ...` would change what the call means
  // before any stage could have an opinion about it. Refused here, and the macro
  // is not stored -- so the name still means the row, and the reader gets one
  // sentence about the line they wrote.
  if (builtins::isReservedPrefix(symbols_->lookup(info.name))) {
    change.error = PPError{info.defineName.span(),
                           "'" + std::string(symbols_->lookup(info.name)) +
                               "' is a name the compiler keeps for itself",
                           PPErrorCode::ReservedIdentifier};
    return change;
  }

  if (info.params.size() + (info.variadic ? 1U : 0U) > support::kMaxMacroParameters) {
    change.error =
        PPError{info.defineName.span(),
                "macro '" + std::string(symbols_->lookup(info.name)) + "' has more than " +
                    std::to_string(support::kMaxMacroParameters) + " parameters",
                PPErrorCode::MacroParameterLimit};
    return change;
  }
  if (!parametersAreConsistent(info)) {
    change.error = PPError{info.defineName.span(), "internal error: inconsistent macro parameters",
                           PPErrorCode::MissingMacroArguments};
    return change;
  }

  const auto existing = index_.find(info.name);
  if (existing != index_.end()) {
    MacroInfo* previous = existing->second;
    // A redefinition with the *same* replacement list is explicitly allowed by
    // the standard and must be silent: it is what makes two headers that define
    // the same limit interchangeable in either order.
    if (identicalDefinition(*previous, info)) {
      change.previous = previous;
      return change;
    }
    change.previous = previous;
    change.redefined = true;
    change.error = PPError{info.defineName.span(),
                           "macro '" + std::string(symbols_->lookup(info.name)) +
                               "' redefined; previous definition is " + previous->bodyText(),
                           PPErrorCode::MacroRedefined};
    return change;
  }

  MacroInfo* stored = replace(std::move(info), /*record=*/true);
  change.changed = true;
  change.previous = nullptr;
  (void)stored;
  return change;
}

MacroTable::Change MacroTable::undef(support::SymId name, SourceLoc where) {
  Change change;
  const auto existing = index_.find(name);
  if (existing == index_.end()) {
    // Undefining something that was never defined is legal and silent, but it
    // is still recorded: "we undefined a name that does not exist" is a real
    // bug in a header, and the tooling should be able to see it.
    history_.push_back(MacroHistoryEntry{name, where, /*wasUndef=*/true});
    return change;
  }

  MacroInfo* victim = existing->second;
  index_.erase(existing);
  for (auto it = live_.begin(); it != live_.end(); ++it) {
    if (*it == victim) {
      live_.erase(it);
      break;
    }
  }
  history_.push_back(MacroHistoryEntry{name, where, /*wasUndef=*/true});
  change.changed = true;
  change.previous = victim;
  return change;
}

void MacroTable::install(MacroInfo info) {
  if (!info.valid()) {
    return;
  }
  // Deliberately the same path as `define`, minus the diagnostics: a builtin
  // and a `-D` from the command line both arrive here.
  const auto existing = index_.find(info.name);
  if (existing != index_.end()) {
    MacroInfo* previous = existing->second;
    *previous = std::move(info);
    return;
  }
  (void)replace(std::move(info), /*record=*/true);
}

MacroInfo* MacroTable::replace(MacroInfo info, bool record) {
  const support::SymId name = info.name;
  if (record) {
    history_.push_back(MacroHistoryEntry{name, info.define, /*wasUndef=*/false});
  }
  owned_.push_back(std::move(info));
  MacroInfo* stored = &owned_.back();
  index_[name] = stored;
  live_.push_back(stored);
  return stored;
}

const MacroInfo* MacroTable::find(support::SymId name) const {
  const auto it = index_.find(name);
  return it == index_.end() ? nullptr : it->second;
}

const MacroInfo* MacroTable::find(std::string_view spelling) const {
  // `contains` is allocation-free, so an unknown name costs one hash and no
  // interning -- which matters because every identifier in the file goes
  // through here.
  if (!symbols_->contains(spelling)) {
    return nullptr;
  }
  return find(symbols_->intern(spelling));
}

std::vector<const MacroInfo*> MacroTable::all() const {
  // `live_` is already in definition order and holds only live macros.
  return std::vector<const MacroInfo*>(live_.begin(), live_.end());
}

void MacroTable::clear() {
  owned_.clear();
  live_.clear();
  index_.clear();
  history_.clear();
}

} // namespace minc::pp
