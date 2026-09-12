// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Macro definitions, and the table that holds them.
//
// Two things are decided at *definition* time rather than per invocation,
// because both references do the same and both for the same reason -- the
// information is static, and re-deriving it per use is how an expander gets
// slow and subtly wrong:
//
//   * which body token is which parameter, stored beside the token as an index.
//     Substitution is then an array lookup instead of comparing every body token
//     against every parameter name. GCC stores a special argument token in the
//     body for the same reason; the index lives next to the token here rather
//     than inside `PPToken`, so no field has two meanings.
//   * whether each parameter is an operand of `#` or `##`, which decides whether
//     its argument is substituted raw or pre-expanded. That is the rule that
//     makes `#define F(x) #x` stringify the spelling while `#define G(x) x`
//     expands the value, and it is per parameter, not per macro.
//
// **The body's spellings are owned.** The tokens point at the file they were
// written in, which is right for diagnostics, but two things need the text
// itself: the standard's "identical redefinition is allowed" rule has to compare
// spellings *across files* (the same constant defined the same way in two
// headers), and `mincc pp --defines` prints the body. So the spellings are
// copied once, into one buffer, and each body token stores an (offset, length)
// pair into it. Offsets and not `string_view`s: a view into a member of the same
// object dangles the moment that object is moved, and short spellings live in
// the string's own storage.
//
// There are no scopes: a macro is defined or it is not, for the rest of the
// translation unit. That is the C model, and it is why the table keeps a history
// rather than pretending definitions nest.
//
// Design record: `docs/architectures/preprocessor.md`.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "pp/pp_error.h"
#include "pp/pp_token.h"
#include "support/intern/interner.h"
#include "support/intern/sym_id.h"

namespace minc::pp {

enum class MacroKind : std::uint8_t {
  ObjectLike,   // `#define NAME body`
  FunctionLike, // `#define NAME(a, b) body`
};

// The predefined macros, whose replacement is computed at the invocation because
// it depends on where the invocation is. `None` means "not a builtin".
enum class BuiltinKind : std::uint8_t {
  None,
  File,       // `__FILE__`
  Line,       // `__LINE__`
  Counter,    // `__COUNTER__`
  Date,       // `__DATE__`, gated on SOURCE_DATE_EPOCH
  Time,       // `__TIME__`, gated on SOURCE_DATE_EPOCH
  HasInclude, // `__has_include(...)`, valid only in `#if`
};

struct MacroParam {
  support::SymId name = support::kInvalidSym;
  // Operands of `#`/`##` are substituted raw; everything else is pre-expanded.
  bool usedWithHash = false;
  bool usedWithPaste = false;
};

// Marks "this body token is not a parameter" in `MacroBodyToken::param`.
inline constexpr std::uint8_t kNotAParameter = 0xFF;

struct MacroBodyToken {
  PPToken token;
  // The token's spelling in `MacroInfo::spellings`.
  std::uint32_t spellingOffset = 0;
  std::uint32_t spellingLength = 0;
  // Which parameter this token is, or `kNotAParameter`.
  std::uint8_t param = kNotAParameter;
  // `__VA_OPT__` and the `)` that closes it. The pair is recognized once, at
  // definition time, so the expander never re-derives which parentheses belong
  // to the operator; the content between them is substituted normally, which is
  // what lets it contain parameters. `__VA_OPT__` cannot nest, so one bit each
  // is enough and a bool in the substitution loop tracks whether the content is
  // being skipped.
  bool variadicOptOpen = false;
  bool variadicOptClose = false;
};

struct MacroInfo {
  support::SymId name = support::kInvalidSym;
  MacroKind kind = MacroKind::ObjectLike;
  BuiltinKind builtin = BuiltinKind::None;
  // The `...` parameter, when present. It is always last and needs no name of
  // its own: `__VA_ARGS__` names it. Its `#`/`##` operand flags live beside it
  // rather than in `params`, because `...` is not a named parameter.
  bool variadic = false;
  bool variadicUsedWithHash = false;
  bool variadicUsedWithPaste = false;

  // Named parameters, in order. For a variadic macro this excludes `...`.
  std::vector<MacroParam> params;

  // The replacement list, exactly as written.
  std::vector<MacroBodyToken> body;
  // Every body token's spelling, concatenated. Owned; see the header note.
  std::string spellings;

  SourceLoc define;     // the whole `#define` directive
  SourceLoc defineName; // just the name, for "defined here"
  std::uint32_t useCount = 0;

  [[nodiscard]] bool isFunctionLike() const {
    return kind == MacroKind::FunctionLike;
  }
  [[nodiscard]] bool isBuiltin() const {
    return builtin != BuiltinKind::None;
  }
  // Arguments a call must supply: the named parameters. `...` may be empty.
  [[nodiscard]] std::size_t requiredArguments() const {
    return params.size();
  }
  [[nodiscard]] bool valid() const {
    return name != support::kInvalidSym;
  }
  [[nodiscard]] std::string_view spellingOf(std::size_t index) const {
    if (index >= body.size()) {
      return {};
    }
    const MacroBodyToken& token = body[index];
    return std::string_view(spellings).substr(token.spellingOffset, token.spellingLength);
  }
  // The whole replacement list as text, single-spaced, for the tooling output.
  [[nodiscard]] std::string bodyText() const;
};

// A definition that was replaced or removed, kept so a diagnostic can say
// "previously defined here" and so the tooling record can show the history.
struct MacroHistoryEntry {
  support::SymId name = support::kInvalidSym;
  SourceLoc where;       // the directive that did it
  bool wasUndef = false; // false: defined here, true: undefined here
};

class MacroTable {
public:
  explicit MacroTable(support::Interner& symbols) : symbols_(&symbols) {}

  MacroTable(const MacroTable&) = delete;
  MacroTable& operator=(const MacroTable&) = delete;

  // The result of a `define`/`undef`, as a value: the table decides what the
  // state becomes, the caller decides what to report.
  struct Change {
    // The table is different than it was (a new definition, or a replacement
    // after an `#undef`). False for a no-op.
    bool changed = false;
    // An existing macro with a *different* body was replaced.
    bool redefined = false;
    // Set when the definition is rejected: a conflicting redefinition, a builtin
    // being redefined, or past the parameter limit. On error the table is left
    // exactly as it was.
    std::optional<PPError> error;
    // The definition that was replaced, when there was one. Valid until the next
    // `define`/`undef`/`clear`.
    const MacroInfo* previous = nullptr;
  };

  // Defines `info`. A redefinition whose kind, parameters and replacement-list
  // spellings are identical (the standard's rule) is allowed silently and leaves
  // the table unchanged; anything else is a `MacroRedefined` error and the table
  // is unchanged. Over `kMaxMacroParameters` parameters is an error too.
  //
  // Precondition: `info.name` is valid and `info.paramOfBodyToken`-equivalent
  // bookkeeping is consistent, which `define` checks.
  [[nodiscard]] Change define(MacroInfo info);

  // Removes `name`. Undefining a name that is not a macro is legal and silent
  // (the standard) but is still recorded, because "we undefined something that
  // was never defined" is a real bug in a header.
  [[nodiscard]] Change undef(support::SymId name, SourceLoc where);

  // Replaces any definition with `info` and reports nothing. What a builtin is
  // installed with, and what a test builds a table by hand with.
  void install(MacroInfo info);

  [[nodiscard]] const MacroInfo* find(support::SymId name) const;
  // Interns `spelling` to look it up. Allocates only on a miss.
  [[nodiscard]] const MacroInfo* find(std::string_view spelling) const;

  [[nodiscard]] bool isDefined(support::SymId name) const {
    return find(name) != nullptr;
  }

  [[nodiscard]] std::span<const MacroHistoryEntry> history() const {
    return history_;
  }
  [[nodiscard]] std::size_t size() const {
    return live_.size();
  }
  // Live macros, in definition order, for `mincc pp --defines`.
  [[nodiscard]] std::vector<const MacroInfo*> all() const;

  void clear();

private:
  // Adds `info` to the owned storage and the live index. Returns the stored
  // copy, which is what `install` overwrites in place.
  [[nodiscard]] MacroInfo* replace(MacroInfo info, bool record);

  support::Interner* symbols_;
  // Owned, in definition order, and never erased: a `MacroInfo*` handed out by
  // `find` stays readable for the table's lifetime, which is what an editor
  // (and a diagnostic that says "previously defined here") needs. `live_` is
  // the same order with `#undef`ed names removed.
  std::deque<MacroInfo> owned_;
  std::vector<MacroInfo*> live_;
  std::unordered_map<support::SymId, MacroInfo*> index_;
  std::vector<MacroHistoryEntry> history_;
};

// True when two definitions are the same for the standard's identical-
// redefinition rule: same kind, same variadic-ness, same parameters (names and
// `#`/`##` flags), and the same body spellings. `#define A 1` twice is identical;
// `#define A 1` then `#define A 2` is not, wherever the two were written.
[[nodiscard]] bool identicalDefinition(const MacroInfo& left, const MacroInfo& right);

} // namespace minc::pp
